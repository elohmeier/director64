// Web Audio for the runtime's sound channels. Channel lifetime (when the score
// may consider a sound finished) is the runtime's service clock; this plays
// what the runtime started, with its loop points and gain, and pauses with
// the game.
import {FlashSounds} from "./flash-audio.js";

// Converted sounds are PCM RIFF WAVE files, decoded synchronously so a sound
// starts on the tick the score started it.
export function wavInfo(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const tag = (at) => String.fromCharCode(...bytes.subarray(at, at + 4));
  if (bytes.length < 12 || tag(0) !== "RIFF" || tag(8) !== "WAVE") throw new Error("not a WAVE file");
  let format = null, data = null;
  for (let at = 12; at + 8 <= bytes.length;) {
    const id = tag(at), size = view.getUint32(at + 4, true), body = at + 8;
    if (body + size > bytes.length) throw new Error("truncated WAVE chunk");
    if (id === "fmt ") {
      if (size < 16) throw new Error("truncated WAVE format");
      format = {
        code: view.getUint16(body, true), channels: view.getUint16(body + 2, true),
        rate: view.getUint32(body + 4, true), bits: view.getUint16(body + 14, true),
      };
    } else if (id === "data") {
      data = bytes.subarray(body, body + size);
    }
    at = body + size + (size & 1);
  }
  if (!format || !data || format.code !== 1 || ![8, 16].includes(format.bits) ||
      format.channels < 1 || format.channels > 2 || !format.rate) {
    throw new Error("unsupported WAVE format");
  }
  const stride = (format.bits / 8) * format.channels;
  if (data.length % stride) throw new Error("incomplete WAVE sample frame");
  const frames = Math.floor(data.length / stride);
  return {...format, frames, data};
}
export function decodeWav(bytes) {
  const format = wavInfo(bytes), {data, frames} = format;
  const stride = (format.bits / 8) * format.channels;
  const samples = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const channels = [];
  for (let c = 0; c < format.channels; c++) {
    const out = new Float32Array(frames);
    for (let i = 0; i < frames; i++) {
      const at = i * stride + c * (format.bits / 8);
      out[i] = format.bits === 8
        ? (data[at] - 128) / 128
        : samples.getInt16(at, true) / 32768;
    }
    channels.push(out);
  }
  return {rate: format.rate, frames, channels};
}

// The loop rule: the loop end clamps to the decoded length, and an empty loop
// is not a loop.
export function loopBounds(start, end, frames) {
  end = Math.min(end, frames);
  return start < end ? {start, end} : null;
}

// Imported sounds are Opus (platforms/web/import/worker.mjs encodeAudio):
// "D64A", a u32 header length, a JSON header {codec, sampleRate, channels,
// frames, sourceRate, sourceFrames, description (base64), chunks:
// [[microseconds, offset, size]]} and the packets. Local builds and the Node
// importer keep WAV.
export function isOpusSound(bytes) {
  return bytes.length > 8 && bytes[0] === 0x44 && bytes[1] === 0x36 && bytes[2] === 0x34 && bytes[3] === 0x41;
}
export function isOggOpus(bytes) {
  return bytes.length >= 47 && String.fromCharCode(...bytes.subarray(0, 4)) === "OggS" &&
    String.fromCharCode(...bytes.subarray(28, 36)) === "OpusHead";
}
export function oggOpusInfo(bytes) {
  if (!isOggOpus(bytes)) throw new Error("not Ogg Opus");
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const preSkip = view.getUint16(38, true);
  let granule = 0, sourceRate = 0, sourceFrames = null, at = 0;
  const decoder = new TextDecoder();
  while (at < bytes.length) {
    if (at + 27 > bytes.length || decoder.decode(bytes.subarray(at, at + 4)) !== "OggS")
      throw new Error("invalid Ogg page");
    const segments = bytes[at + 26], body = at + 27 + segments;
    if (body > bytes.length) throw new Error("truncated Ogg lacing");
    let size = 0;
    for (let i = at + 27; i < body; i++) size += bytes[i];
    const end = body + size;
    if (end > bytes.length) throw new Error("truncated Ogg page");
    const position = view.getBigInt64(at + 6, true);
    if (position >= 0n) granule = Number(position);
    if (decoder.decode(bytes.subarray(body, body + 8)) === "OpusTags") {
      let p = body + 8;
      if (p + 4 > end) throw new Error("truncated Opus tags");
      p += 4 + view.getUint32(p, true);
      if (p + 4 > end) throw new Error("truncated Opus vendor");
      const count = view.getUint32(p, true); p += 4;
      for (let i = 0; i < count; i++) {
        if (p + 4 > end) throw new Error("truncated Opus comment");
        const length = view.getUint32(p, true); p += 4;
        if (p + length > end) throw new Error("truncated Opus comment body");
        const text = decoder.decode(bytes.subarray(p, p + length)); p += length;
        if (text.startsWith("D64_SOURCE_RATE=")) sourceRate = Number(text.slice(16));
        if (text.startsWith("D64_SOURCE_FRAMES=")) sourceFrames = Number(text.slice(18));
      }
    }
    at = end;
  }
  const duration = Math.max(0, granule - preSkip) / 48000;
  if (!sourceRate) sourceRate = view.getUint32(40, true) || 48000;
  if (sourceFrames == null) sourceFrames = Math.round(duration * sourceRate);
  if (!Number.isSafeInteger(sourceRate) || sourceRate <= 0 ||
      !Number.isSafeInteger(sourceFrames) || sourceFrames < 0)
    throw new Error("invalid Opus source sample bounds");
  return {rate: sourceRate, frames: sourceFrames, duration: sourceFrames / sourceRate};
}
export function readOpusHeader(bytes) {
  const length = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(4, true);
  const header = JSON.parse(new TextDecoder().decode(bytes.subarray(8, 8 + length)));
  return {...header, data: bytes.subarray(8 + length)};
}
// Decodes with WebCodecs into a buffer of exactly the header's frames; loop
// points stay in source frames at the source rate.
export async function decodeOpusSound(context, bytes) {
  return decodeOpusPackets(context, readOpusHeader(bytes));
}
export async function decodeOggOpusSound(context, bytes) {
  const info = oggOpusInfo(bytes), packets = [];
  let partial = [], at = 0;
  while (at < bytes.length) {
    const segments = bytes[at + 26];
    let p = at + 27 + segments;
    for (let i = 0; i < segments; i++) {
      const length = bytes[at + 27 + i];
      partial.push(bytes.subarray(p, p + length)); p += length;
      if (length < 255) {
        const packet = new Uint8Array(partial.reduce((n, part) => n + part.length, 0));
        let offset = 0;
        for (const part of partial) {packet.set(part, offset); offset += part.length;}
        packets.push(packet); partial = [];
      }
    }
    at = p;
  }
  if (partial.length || packets.length < 2) throw new Error("incomplete Opus packet");
  const chunks = [], data = new Uint8Array(packets.slice(2).reduce((n, packet) => n + packet.length, 0));
  let offset = 0;
  for (let i = 2; i < packets.length; i++) {
    // Local FFmpeg exports use 20 ms Opus packets. Output order is retained;
    // the header's pre-skip and exact source bounds determine audible frames.
    chunks.push([(i - 2) * 20000, offset, packets[i].length]);
    data.set(packets[i], offset); offset += packets[i].length;
  }
  return decodeOpusPackets(context, {codec: "opus", sampleRate: 48000, channels: packets[0][9],
    frames: Math.round(info.frames * 48000 / info.rate), sourceRate: info.rate,
    sourceFrames: info.frames, descriptionBytes: packets[0], chunks, data});
}
async function decodeOpusPackets(context, sound) {
  const planes = Array.from({length: sound.channels}, () => []);
  let failure = null;
  const decoder = new AudioDecoder({
    output: (data) => {
      for (let c = 0; c < sound.channels; c++) {
        const plane = new Float32Array(data.numberOfFrames);
        data.copyTo(plane, {planeIndex: c, format: "f32-planar"});
        planes[c].push(plane);
      }
      data.close();
    },
    error: (error) => { failure = error; },
  });
  const description = sound.descriptionBytes ?? Uint8Array.from(atob(sound.description), (c) => c.charCodeAt(0));
  decoder.configure({codec: sound.codec, sampleRate: sound.sampleRate, numberOfChannels: sound.channels,
    ...(description.length ? {description} : {})});
  for (const [timestamp, offset, size] of sound.chunks)
    decoder.decode(new EncodedAudioChunk({type: "key", timestamp, data: sound.data.subarray(offset, offset + size)}));
  await decoder.flush();
  decoder.close();
  if (failure) throw failure;
  const buffer = context.createBuffer(sound.channels, Math.max(1, sound.frames), sound.sampleRate);
  planes.forEach((pieces, c) => {
    const plane = new Float32Array(sound.frames);
    let at = 0;
    for (const piece of pieces) {
      if (at >= sound.frames) break;
      plane.set(piece.subarray(0, sound.frames - at), at);
      at += piece.length;
    }
    buffer.copyToChannel(plane, c);
  });
  return {buffer, rate: sound.sourceRate, frames: sound.sourceFrames};
}

export class AudioEngine {
  constructor(lookup, channelCount) {
    this.lookup = lookup;
    this.context = new AudioContext();
    this.master = this.context.createGain();
    this.master.connect(this.context.destination);
    this.gains = [];
    this.voices = [];
    for (let i = 0; i < channelCount; i++) {
      const gain = this.context.createGain();
      gain.connect(this.master);
      this.gains.push(gain);
      this.voices.push(null);
    }
    this.pending = [];
    this.paused = [];
    this.playback = [];
    this.loopReleased = [];
    this.video = new Map();
    this.buffers = new Map();
    this.decoding = new Map();
    this.bufferBytes = 0;
    this.durations = new Map();
    this.errors = [];
    this.flash = new FlashSounds(this);
  }
  // A sound's decoded buffer with its source rate and length (loop points
  // and the loop rule are in source frames): at once for a WAV or
  // a sound decoded before, null while an Opus sound decodes (whenLoaded).
  load(name) {
    const entry = this.buffers.get(name);
    if (entry) {
      this.buffers.delete(name); // re-insert as most recently used
      this.buffers.set(name, entry);
      return entry;
    }
    const bytes = this.lookup(name);
    if (!bytes) throw new Error(`missing sound ${name}`);
    if (isOpusSound(bytes) || isOggOpus(bytes)) {
      this.whenLoaded(name);
      return null;
    }
    const wav = decodeWav(bytes);
    const buffer = this.context.createBuffer(wav.channels.length, Math.max(1, wav.frames), wav.rate);
    wav.channels.forEach((samples, c) => buffer.copyToChannel(samples, c));
    return this.keep(name, {buffer, rate: wav.rate, frames: wav.frames});
  }
  whenLoaded(name) {
    const entry = this.buffers.get(name);
    if (entry) return Promise.resolve(entry);
    if (!this.decoding.has(name)) {
      const bytes = this.lookup(name);
      const decoding = (bytes ? (isOggOpus(bytes)
        ? decodeOggOpusSound(this.context, bytes)
        : decodeOpusSound(this.context, bytes)) : Promise.reject(new Error(`missing sound ${name}`)))
        .then((decoded) => this.keep(name, decoded))
        .finally(() => this.decoding.delete(name));
      this.decoding.set(name, decoding);
    }
    return this.decoding.get(name);
  }
  keep(name, entry) {
    this.buffers.set(name, entry);
    this.bufferBytes += entry.buffer.length * entry.buffer.numberOfChannels * 4;
    // Decoded sounds are many times their stored size; keep the recent ones.
    for (const [key, old] of this.buffers) {
      if (this.bufferBytes <= 96 * 1024 * 1024) break;
      this.buffers.delete(key);
      this.bufferBytes -= old.buffer.length * old.buffer.numberOfChannels * 4;
    }
    return entry;
  }
  // Starts `start(entry, offset)` now when the sound is ready, or once it is
  // decoded, `offset` seconds in by then, unless the channel moved on.
  whenReady(channel, name, start, preserveOnset = false) {
    let entry;
    try {
      entry = this.load(name);
    } catch (error) {
      this.errors.push(`cannot play sound ${name}: ${error}`);
      console.warn(error);
      return;
    }
    if (entry) return start(entry, 0);
    const token = {requested: this.context.currentTime, pausedAt: null, held: 0};
    this.pending[channel] = token;
    this.whenLoaded(name).then((ready) => {
      if (this.pending[channel] !== token) return;
      this.pending[channel] = null;
      const late = Math.max(0, (token.pausedAt ?? this.context.currentTime) - token.requested - token.held);
      start(ready, preserveOnset && ready.frames < ready.rate ? 0 : late);
    }, (error) => {
      if (this.pending[channel] !== token) return;
      this.pending[channel] = null;
      this.errors.push(`cannot decode sound ${name}: ${error}`);
      console.warn(error);
    });
  }
  play(channel, name, looping, loopStart, loopEnd) {
    this.stop(channel);
    if (!name || channel >= this.voices.length) return;
    this.whenReady(channel, name, ({buffer, rate, frames}, offset) => {
      const bounds = looping ? loopBounds(loopStart, loopEnd, frames) : null;
      const start = bounds ? bounds.start / rate : 0;
      const end = bounds ? bounds.end / rate : buffer.duration;
      const released = this.loopReleased[channel];
      if (looping && released != null) {
        const position = end > start && released >= end ?
          start + (released - start) % (end - start) : released;
        offset = position + Math.max(0, offset - released);
      }
      this.playback[channel] = {buffer, offset, looping: looping && released == null,
        loopStart: start, loopEnd: end};
      if (!this.paused[channel]) this.startChannel(channel);
    }, !looping);
  }
  // A playFile stream (the streams pack): WAV starts at once, as members do;
  // MP3 decodes in the browser first and starts when ready, unless the
  // channel has moved on meanwhile. The runtime holds the channel busy for
  // the stream's recorded length either way.
  playStream(channel, bytes) {
    this.stop(channel);
    if (!bytes || channel >= this.voices.length) return;
    const token = {requested: this.context.currentTime, pausedAt: null, held: 0};
    this.pending[channel] = token;
    const start = (buffer) => {
      if (this.pending[channel] !== token) return;
      this.pending[channel] = null;
      const offset = Math.max(0, (token.pausedAt ?? this.context.currentTime) - token.requested - token.held);
      this.playback[channel] = {buffer, offset, looping: false, loopStart: 0, loopEnd: buffer.duration};
      if (!this.paused[channel]) this.startChannel(channel);
    };
    const failed = (error) => {
      if (this.pending[channel] !== token) return;
      this.pending[channel] = null;
      this.errors.push(String(error));
      console.warn(error);
    };
    if (isOggOpus(bytes)) {
      decodeOggOpusSound(this.context, bytes).then(({buffer}) => start(buffer), failed);
    } else if (isOpusSound(bytes)) {
      decodeOpusSound(this.context, bytes).then(({buffer}) => start(buffer), failed);
    } else if (bytes[0] === 0x52 && bytes[1] === 0x49 && bytes[2] === 0x46 && bytes[3] === 0x46) {
      try {
        const wav = decodeWav(bytes);
        const buffer = this.context.createBuffer(wav.channels.length, Math.max(1, wav.frames), wav.rate);
        wav.channels.forEach((samples, c) => buffer.copyToChannel(samples, c));
        start(buffer);
      } catch (error) {
        failed(error);
      }
    } else {
      this.context.decodeAudioData(bytes.slice().buffer).then(start, failed);
    }
  }
  // Buffer sources are single-use. Resume with a new source reading the
  // same buffer at the saved offset; the channel's gain and loop survive.
  startChannel(channel) {
    const state = this.playback[channel];
    if (!state) return;
    if (state.looping && state.loopEnd > state.loopStart && state.offset >= state.loopEnd)
      state.offset = state.loopStart + (state.offset - state.loopStart) % (state.loopEnd - state.loopStart);
    if (!state.looping && state.offset >= state.buffer.duration) {
      this.playback[channel] = null;
      return;
    }
    const source = this.context.createBufferSource();
    source.buffer = state.buffer;
    source.loop = state.looping;
    source.loopStart = state.loopStart;
    source.loopEnd = state.loopEnd;
    source.connect(this.gains[channel]);
    source.onended = () => {
      if (this.voices[channel] === source) {
        this.voices[channel] = null;
        this.playback[channel] = null;
      }
      source.disconnect();
    };
    state.startedAt = this.context.currentTime;
    source.start(0, state.offset);
    this.voices[channel] = source;
  }
  pauseChannel(channel, paused = true) {
    if (channel < 0 || channel >= this.voices.length || !!this.paused[channel] === paused) return;
    if (!this.playback[channel] && !this.pending[channel]) return;
    const now = this.context.currentTime;
    const token = this.pending[channel];
    if (token) {
      if (paused) token.pausedAt = now;
      else { token.held += now - token.pausedAt; token.pausedAt = null; }
    }
    this.paused[channel] = paused;
    if (paused) {
      const state = this.playback[channel];
      if (state && this.voices[channel]) state.offset += Math.max(0, now - state.startedAt);
      this.stopSource(channel);
    } else {
      this.startChannel(channel);
    }
  }
  stopSource(channel) {
    const voice = this.voices[channel];
    this.voices[channel] = null;
    if (voice) {
      voice.onended = null;
      voice.stop();
      voice.disconnect();
    }
  }
  breakLoop(channel) {
    if (channel < 0 || channel >= this.voices.length || this.loopReleased[channel] != null) return;
    const now = this.context.currentTime;
    const token = this.pending[channel];
    if (token) {
      this.loopReleased[channel] = Math.max(0, (token.pausedAt ?? now) - token.requested - token.held);
      return;
    }
    const state = this.playback[channel];
    if (!state?.looping) return;
    let offset = state.offset + (this.voices[channel] ? Math.max(0, now - state.startedAt) : 0);
    if (state.loopEnd > state.loopStart && offset >= state.loopEnd)
      offset = state.loopStart + (offset - state.loopStart) % (state.loopEnd - state.loopStart);
    state.offset = offset;
    state.startedAt = now;
    state.looping = false;
    this.loopReleased[channel] = 0;
    if (this.voices[channel]) this.voices[channel].loop = false;
  }
  stop(channel) {
    this.pending[channel] = null;
    this.paused[channel] = false;
    this.playback[channel] = null;
    this.loopReleased[channel] = null;
    this.stopSource(channel);
  }
  // A sound's length in seconds (0 when missing), without decoding it.
  duration(name) {
    if (this.durations.has(name)) return this.durations.get(name);
    try {
      const bytes = this.lookup(name);
      if (!bytes) throw new Error(`missing sound ${name}`);
      let duration;
      if (isOpusSound(bytes)) {
        const {frames, sampleRate} = readOpusHeader(bytes);
        duration = frames / sampleRate;
      } else if (isOggOpus(bytes)) {
        duration = oggOpusInfo(bytes).duration;
      } else {
        const {frames, rate} = wavInfo(bytes);
        duration = frames / rate;
      }
      if (!Number.isFinite(duration) || duration < 0) throw new Error("invalid sound duration");
      this.durations.set(name, duration);
      return duration;
    } catch (error) {
      this.durations.set(name, 0);
      this.errors.push(`cannot read sound duration ${name}: ${error}`);
      return 0;
    }
  }
  // Digital video sound, one voice per video sprite beside the channels.
  playVideo(sprite, name, offset) {
    this.stopVideo(sprite);
    const key = `video:${sprite}`;
    this.whenReady(key, name, ({buffer}, late) => {
      const gain = this.context.createGain();
      gain.connect(this.master);
      const source = this.context.createBufferSource();
      source.buffer = buffer;
      source.connect(gain);
      source.start(0, Math.min(offset + late, buffer.duration));
      this.video.set(sprite, {source, gain});
    });
  }
  videoRate(sprite, rate, gain) {
    const voice = this.video.get(sprite);
    if (!voice) return;
    voice.source.playbackRate.value = rate;
    voice.gain.gain.value = gain;
  }
  stopVideo(sprite) {
    this.pending[`video:${sprite}`] = null;
    const voice = this.video.get(sprite);
    if (!voice) return;
    this.video.delete(sprite);
    voice.source.stop();
    voice.source.disconnect();
    voice.gain.disconnect();
  }
  gain(channel, value) {
    if (this.gains[channel]) this.gains[channel].gain.value = value;
  }
  flashSound(id, sound) { this.flash.update(id, sound); }
  // The player's volume and mute (settings.js), over the game's own gains.
  setVolume(volume, muted) {
    this.volume = volume;
    this.mutedByPlayer = muted;
    this.master.gain.value = muted ? 0 : volume;
  }
  get muted() { return !!this.mutedByPlayer; }
  suspend() { return this.context.suspend(); }
  resume() { return this.context.resume(); }
  stopAll() {
    this.flash.stopAll();
    this.voices.forEach((_, i) => this.stop(i));
    for (const sprite of [...this.video.keys()]) this.stopVideo(sprite);
  }
}

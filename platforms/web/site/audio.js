// Web Audio for the runtime's sound channels. Channel lifetime (when the score
// may consider a sound finished) is the runtime's service clock; this plays
// what the runtime started, with its loop points and gain, and pauses with
// the game.

// Converted sounds are PCM RIFF WAVE files, decoded synchronously so a sound
// starts on the tick the score started it.
export function decodeWav(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const tag = (at) => String.fromCharCode(...bytes.subarray(at, at + 4));
  if (bytes.length < 12 || tag(0) !== "RIFF" || tag(8) !== "WAVE") throw new Error("not a WAVE file");
  let format = null, data = null;
  for (let at = 12; at + 8 <= bytes.length;) {
    const id = tag(at), size = view.getUint32(at + 4, true), body = at + 8;
    if (body + size > bytes.length) throw new Error("truncated WAVE chunk");
    if (id === "fmt ") {
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
  const frames = Math.floor(data.length / stride);
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

// The console's rule (platforms/n64/audio_bounds.h): the loop end clamps to
// the decoded length, and an empty loop is not a loop.
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
export function readOpusHeader(bytes) {
  const length = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(4, true);
  const header = JSON.parse(new TextDecoder().decode(bytes.subarray(8, 8 + length)));
  return {...header, data: bytes.subarray(8 + length)};
}
// Decodes with WebCodecs into a buffer of exactly the header's frames; loop
// points stay in source frames at the source rate.
export async function decodeOpusSound(context, bytes) {
  const sound = readOpusHeader(bytes);
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
  const description = Uint8Array.from(atob(sound.description), (c) => c.charCodeAt(0));
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
    this.video = new Map();
    this.buffers = new Map();
    this.decoding = new Map();
    this.bufferBytes = 0;
    this.errors = [];
  }
  // A sound's decoded buffer with its source rate and length (loop points
  // and the console's loop rule are in source frames): at once for a WAV or
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
    if (isOpusSound(bytes)) {
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
      const decoding = (bytes ? decodeOpusSound(this.context, bytes) : Promise.reject(new Error(`missing sound ${name}`)))
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
  whenReady(channel, name, start) {
    let entry;
    try {
      entry = this.load(name);
    } catch (error) {
      this.errors.push(String(error));
      console.warn(error);
      return;
    }
    if (entry) return start(entry, 0);
    const token = {};
    this.pending[channel] = token;
    const requested = this.context.currentTime;
    this.whenLoaded(name).then((ready) => {
      if (this.pending[channel] !== token) return;
      this.pending[channel] = null;
      start(ready, Math.max(0, this.context.currentTime - requested));
    }, (error) => {
      this.errors.push(String(error));
      console.warn(error);
    });
  }
  play(channel, name, looping, loopStart, loopEnd) {
    this.stop(channel);
    if (!name || channel >= this.voices.length) return;
    this.whenReady(channel, name, ({buffer, rate, frames}, offset) => {
      const source = this.context.createBufferSource();
      source.buffer = buffer;
      if (looping) {
        source.loop = true;
        const bounds = loopBounds(loopStart, loopEnd, frames);
        if (bounds) {
          source.loopStart = bounds.start / rate;
          source.loopEnd = bounds.end / rate;
        }
      } else if (offset >= buffer.duration) return;
      source.connect(this.gains[channel]);
      source.onended = () => {
        if (this.voices[channel] === source) this.voices[channel] = null;
      };
      source.start(0, offset);
      this.voices[channel] = source;
    });
  }
  // A playFile stream (the streams pack): WAV starts at once, as members do;
  // MP3 decodes in the browser first and starts when ready, unless the
  // channel has moved on meanwhile. The runtime holds the channel busy for
  // the stream's recorded length either way.
  playStream(channel, bytes) {
    this.stop(channel);
    if (!bytes || channel >= this.voices.length) return;
    const token = {};
    this.pending[channel] = token;
    const start = (buffer) => {
      if (this.pending[channel] !== token) return;
      this.pending[channel] = null;
      const source = this.context.createBufferSource();
      source.buffer = buffer;
      source.connect(this.gains[channel]);
      source.onended = () => {
        if (this.voices[channel] === source) this.voices[channel] = null;
      };
      source.start();
      this.voices[channel] = source;
    };
    const failed = (error) => {
      this.errors.push(String(error));
      console.warn(error);
    };
    if (isOpusSound(bytes)) {
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
  stop(channel) {
    this.pending[channel] = null;
    const voice = this.voices[channel];
    this.voices[channel] = null;
    if (voice) {
      voice.onended = null;
      voice.stop();
      voice.disconnect();
    }
  }
  // A sound's length in seconds (0 when missing), without decoding it.
  duration(name) {
    const bytes = this.lookup(name);
    if (!bytes) return 0;
    try {
      if (isOpusSound(bytes)) {
        const {frames, sampleRate} = readOpusHeader(bytes);
        return frames / sampleRate;
      }
      const wav = decodeWav(bytes);
      return wav.frames / wav.rate;
    } catch {
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
  set muted(value) { this.master.gain.value = value ? 0 : 1; }
  get muted() { return this.master.gain.value === 0; }
  suspend() { return this.context.suspend(); }
  resume() { return this.context.resume(); }
  stopAll() {
    this.voices.forEach((_, i) => this.stop(i));
    for (const sprite of [...this.video.keys()]) this.stopVideo(sprite);
  }
}

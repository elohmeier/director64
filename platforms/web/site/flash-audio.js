// Independent event sounds, Adobe SWF19 pp180–181. Loop bounds use source
// samples; envelope positions always use 44,100-Hz samples. The envelope is
// applied to a single decoded pass, then Web Audio repeats that pass exactly
// the authored finite number of times. Director channels are separate.
export function flashEnvelope(points, sample, rate, mono = false) {
  if (!points.length) return [1, 1];
  const position = sample * 44100 / rate;
  let n = 0;
  while (n + 1 < points.length && points[n + 1][0] <= position) n++;
  let [, left, right] = points[n];
  if (n + 1 < points.length && position > points[n][0]) {
    const next = points[n + 1], f = (position - points[n][0]) / (next[0] - points[n][0]);
    left += (next[1] - left) * f; right += (next[2] - right) * f;
  }
  if (mono) left = right = (left + right) / 2;
  return [left / 32768, right / 32768];
}
export function validateFlashSound(sound) {
  for (const key of ["rate", "frames", "start", "end", "loops"])
    if (!Number.isSafeInteger(sound[key]) || sound[key] < 0) throw new Error(`invalid Flash sound ${key}`);
  if (!sound.rate || sound.start >= sound.end || sound.end > sound.frames || !sound.loops ||
      !Number.isFinite(sound.position) || sound.position < 0 || !Number.isFinite(sound.gain))
    throw new Error("invalid Flash sound bounds or position");
  let previous = -1;
  for (const [position, left, right] of sound.envelopes) {
    if (!Number.isSafeInteger(position) || position <= previous ||
        !Number.isInteger(left) || !Number.isInteger(right) || left < 0 || right < 0 || left > 32768 || right > 32768)
      throw new Error("invalid Flash sound envelope");
    previous = position;
  }
}
export function flashSoundBuffer(context, entry, sound) {
  if (entry.rate !== sound.rate || entry.frames !== sound.frames)
    throw new Error("Flash sound metadata disagrees with decoded audio");
  const input = entry.buffer, rate = input.sampleRate;
  const frames = Math.max(1, Math.round((sound.end - sound.start) * rate / sound.rate));
  if (frames * 8 > 96 * 1024 * 1024) throw new Error("Flash event sound decoded capacity exceeded");
  const output = context.createBuffer(2, frames, rate);
  const mono = input.numberOfChannels === 1;
  const channels = [input.getChannelData(0), input.getChannelData(mono ? 0 : 1)];
  for (let c = 0; c < 2; c++) {
    const plane = output.getChannelData(c), source = channels[c];
    for (let n = 0; n < frames; n++) {
      const sample = sound.start + n * sound.rate / rate;
      const at = sample * rate / sound.rate, index = Math.floor(at), fraction = at - index;
      const next = Math.min(source.length - 1, index + 1);
      plane[n] = ((source[index] ?? 0) * (1 - fraction) + (source[next] ?? 0) * fraction) *
        flashEnvelope(sound.envelopes, sample, sound.rate, mono)[c];
    }
  }
  return output;
}

export class FlashSounds {
  constructor(engine) { this.engine = engine; this.voices = new Map(); this.bytes = 0; }
  fail(error) { this.engine.errors.push(`Flash event audio: ${error}`); }
  update(id, sound) {
    if (!sound) { this.stop(id); return; }
    try {
      validateFlashSound(sound);
      let state = this.voices.get(id);
      if (!state) {
        if (this.voices.size >= 64) throw new Error("concurrent Flash event sound capacity exceeded");
        state = {sound, updated: this.engine.context.currentTime, source: null, buffer: null};
        this.voices.set(id, state);
        const ready = (entry) => {
          if (this.voices.get(id) !== state) return;
          try {
            state.buffer = flashSoundBuffer(this.engine.context, entry, sound);
            state.bytes = state.buffer.length * 8;
            if (this.bytes + state.bytes > 96 * 1024 * 1024) throw new Error("Flash event sound decoded capacity exceeded");
            this.bytes += state.bytes; state.accounted = true;
            this.start(state);
          } catch (error) { this.stop(id); this.fail(error); }
        };
        const entry = this.engine.load(sound.name);
        if (entry) ready(entry);
        else this.engine.whenLoaded(sound.name).then(ready, (error) => {
          if (this.voices.get(id) === state) { this.stop(id); this.fail(error); }
        });
      } else {
        const wasPaused = state.sound.paused;
        state.sound = sound; state.updated = this.engine.context.currentTime;
        if (sound.paused && !wasPaused) this.stopSource(state);
        if (!sound.paused && wasPaused) this.start(state);
        if (state.gain) state.gain.gain.value = sound.gain;
      }
    } catch (error) { this.stop(id); this.fail(error); }
  }
  start(state) {
    if (!state.buffer || state.sound.paused || state.source) return;
    const context = this.engine.context, sound = state.sound;
    const elapsed = sound.position / sound.rate + Math.max(0, context.currentTime - state.updated);
    const duration = (sound.end - sound.start) * sound.loops / sound.rate;
    if (elapsed >= duration) return;
    const gain = context.createGain(); gain.gain.value = sound.gain; gain.connect(this.engine.master);
    const source = context.createBufferSource(); source.buffer = state.buffer;
    source.loop = sound.loops > 1; source.loopStart = 0; source.loopEnd = state.buffer.duration;
    source.connect(gain); state.source = source; state.gain = gain;
    source.onended = () => { if (state.source === source) { state.source = null; state.gain = null; }
      source.disconnect(); gain.disconnect(); };
    source.start(0, elapsed % state.buffer.duration, duration - elapsed);
  }
  stopSource(state) {
    if (!state.source) return;
    state.source.onended = null; state.source.stop(); state.source.disconnect(); state.gain.disconnect();
    state.source = null; state.gain = null;
  }
  stop(id) {
    const state = this.voices.get(id); if (!state) return;
    this.stopSource(state); if (state.accounted) this.bytes -= state.bytes; this.voices.delete(id);
  }
  stopAll() { for (const id of this.voices.keys()) this.stop(id); }
}

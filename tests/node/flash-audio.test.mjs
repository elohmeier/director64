import assert from "node:assert/strict";
import test from "node:test";
import {FlashSounds, flashEnvelope, flashSoundBuffer, validateFlashSound} from "../../platforms/web/site/flash-audio.js";
class Buffer {
  constructor(channels, length, sampleRate) {Object.assign(this, {numberOfChannels: channels, length, sampleRate, duration: length / sampleRate});
    this.channels = Array.from({length: channels}, () => new Float32Array(length));}
  getChannelData(c) {return this.channels[c];}
}
class Context {
  currentTime = 0; sources = [];
  createBuffer(...args) {return new Buffer(...args);}
  createGain() {return {connect() {}, disconnect() {}, gain: {value: 1}};}
  createBufferSource() {const source = {connect() {}, disconnect() {}, stop() {this.stopped = true;},
    start(when, offset, duration) {Object.assign(this, {when, offset, duration});}};
    this.sources.push(source); return source;}
}
const spec = (more = {}) => ({name: "sound", rate: 11025, frames: 11025, start: 2205, end: 6615, loops: 3,
  position: 0, gain: 1, paused: false, envelopes: [[0, 0, 32768], [44100, 32768, 0]], ...more});
function engine() {
  const context = new Context(), buffer = context.createBuffer(2, 11025, 11025);
  buffer.channels.forEach(p => p.fill(1));
  const engine = {context, master: {}, errors: [], load: () => ({buffer, rate: 11025, frames: 11025})};
  return {engine, player: new FlashSounds(engine), context, buffer};
}
test("envelopes use 44.1k positions, stereo interpolation, and mono averaging", () => {
  const points = spec().envelopes;
  assert.deepEqual(flashEnvelope(points, 5512.5, 11025), [0.5, 0.5]);
  assert.deepEqual(flashEnvelope(points, 1102.5, 11025), [0.1, 0.9]);
  assert.deepEqual(flashEnvelope(points, 1102.5, 11025, true), [0.5, 0.5]);
  assert.deepEqual(flashEnvelope(points, 99999, 11025), [1, 0]);
  assert.throws(() => validateFlashSound(spec({envelopes: [[10, 1, 1], [10, 2, 2]]})), /envelope/);
});
test("decoded pass preserves in/out boundaries and envelopes at every sample", () => {
  const {engine: e, context} = engine();
  const out = flashSoundBuffer(context, e.load(), spec());
  assert.equal(out.length, 4410);
  for (let n = 0; n < out.length; n++) {
    const f = (2205 + n) / 11025;
    assert.ok(Math.abs(out.channels[0][n] - f) < 0.000001);
    assert.ok(Math.abs(out.channels[1][n] - (1 - f)) < 0.000001);
  }
});
test("independent event voices retain finite loops, pause, gain, and cancellation", () => {
  const {player, context} = engine();
  player.update(1, spec()); player.update(2, spec({loops: 1, gain: 0.25}));
  assert.equal(context.sources.length, 2);assert.equal(context.sources[0].loop, true);
  assert.equal(context.sources[1].loop, false);assert.equal(context.sources[0].duration, 1.2);
  context.currentTime = 0.25;
  player.update(1, spec({position: 2756, paused: true}));assert.equal(context.sources[0].stopped, true);
  assert.equal(context.sources[1].stopped, undefined);
  context.currentTime = 10;
  player.update(1, spec({position: 2756, gain: 0.5}));
  assert.equal(context.sources.length, 3);
  assert.ok(Math.abs(context.sources[2].offset - 2756 / 11025) < 1e-12);
  assert.ok(Math.abs(context.sources[2].duration - (1.2 - 2756 / 11025)) < 1e-12);
  player.update(1, null);assert.equal(context.sources[2].stopped, true);assert.equal(player.voices.size, 1);
  player.stopAll();assert.equal(player.voices.size, 0);assert.equal(player.bytes, 0);
});
test("late decode uses latest service position and cannot revive stopped voices", async () => {
  const {engine: e, player, context} = engine();const entry = e.load();let resolve;
  e.load = () => null;e.whenLoaded = () => new Promise(done => {resolve = done;});
  player.update(1, spec());context.currentTime = 0.1;player.update(1, spec({position: 1102}));
  context.currentTime = 0.15;resolve(entry);await Promise.resolve();
  assert.ok(Math.abs(context.sources[0].offset - (1102 / 11025 + 0.05)) < 1e-12);
  player.update(2, spec());player.update(2, null);resolve(entry);await Promise.resolve();
  assert.equal(context.sources.length, 1);
  player.update(3, spec());player.update(3, spec({position: 1, paused: true}));resolve(entry);await Promise.resolve();
  assert.equal(context.sources.length, 1);player.stopAll();
});

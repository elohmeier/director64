import assert from "node:assert/strict";
import test from "node:test";
import {AudioEngine, decodeWav, wavInfo} from "../../platforms/web/site/audio.js";

class Context {
  currentTime = 0;
  sources = [];
  destination = {};
  createGain() { return {connect() {}, gain: {value: 1}}; }
  createBufferSource() {
    const node = {connect() {}, disconnect() {}, stop() { this.stopped = true; },
      start(when, offset) { this.offset = offset; }};
    this.sources.push(node);
    return node;
  }
}
function engine() {
  const original = globalThis.AudioContext;
  globalThis.AudioContext = Context;
  const audio = new AudioEngine(() => null, 2);
  globalThis.AudioContext = original;
  const entry = {buffer: {duration: 10}, rate: 10, frames: 100};
  audio.load = () => entry;
  return {audio, entry, ctx: audio.context};
}

test("loop release keeps the current pass and tail through pause/resume", () => {
  const {audio, ctx} = engine();
  audio.play(0, "voice", true, 20, 60);
  const first = ctx.sources[0];
  ctx.currentTime = 7.25; // after one wrap, source position is 3.25
  audio.breakLoop(0);
  assert.equal(first.loop, false);
  assert.equal(first.stopped, undefined);
  assert.equal(audio.playback[0].offset, 3.25);
  ctx.currentTime = 8.25;
  audio.pauseChannel(0, true);
  ctx.currentTime = 90;
  audio.breakLoop(0); // repeated release cannot re-wrap or resume
  audio.pauseChannel(0, false);
  assert.equal(ctx.sources[1].offset, 4.25);
  assert.equal(ctx.sources[1].loop, false);
  audio.play(0, "replacement", true, 20, 60);
  assert.equal(ctx.sources[2].loop, true);
  ctx.currentTime = 97.25;
  audio.pauseChannel(0, true);
  audio.breakLoop(0);
  audio.pauseChannel(0, false);
  assert.equal(ctx.sources[3].offset, 3.25);
  assert.equal(ctx.sources[3].loop, false);
});

test("a loop released during decode starts at its release position plus the tail", async () => {
  const {audio, entry, ctx} = engine();
  let ready;
  audio.load = () => null;
  audio.whenLoaded = () => new Promise((resolve) => { ready = resolve; });
  audio.play(0, "voice", true, 20, 60);
  ctx.currentTime = 7.25;
  audio.breakLoop(0); // release at source3.25
  ctx.currentTime = 9.25;
  audio.pauseChannel(0, true);
  ctx.currentTime = 90;
  ready(entry);
  await Promise.resolve();
  assert.equal(ctx.sources.length, 0);
  audio.pauseChannel(0, false);
  assert.equal(ctx.sources[0].offset, 5.25);
  assert.equal(ctx.sources[0].loop, false);
});

test("channel pause is silent and resume preserves offset, loop and gain", () => {
  const {audio, entry, ctx} = engine();
  audio.play(0, "voice", true, 20, 60);
  const first = ctx.sources[0];
  ctx.currentTime = 3.25;
  audio.pauseChannel(0, true);
  assert.equal(first.stopped, true);
  assert.equal(audio.voices[0], null);
  ctx.currentTime = 90;
  audio.pauseChannel(0, true); // repeated pause does not move the offset
  audio.pauseChannel(0, false);
  const second = ctx.sources[1];
  assert.equal(second.buffer, entry.buffer);
  assert.equal(second.offset, 3.25);
  assert.equal(second.loop, true);
  assert.equal(second.loopStart, 2);
  assert.equal(second.loopEnd, 6);
  ctx.currentTime = 94;
  audio.pauseChannel(0, true);
  audio.pauseChannel(0, false);
  assert.equal(ctx.sources[2].offset, 3.25); // wrapped through [2,6)
  audio.stop(0);
  audio.pauseChannel(0, false);
  assert.equal(ctx.sources.length, 3);
});

test("paused channels are independent and replacement sounds start immediately", () => {
  const {audio, ctx} = engine();
  audio.play(0, "one", false, 0, 0);
  audio.play(1, "two", false, 0, 0);
  ctx.currentTime = 1;
  audio.pauseChannel(0, true);
  assert.equal(ctx.sources[1].stopped, undefined);
  audio.play(0, "replacement", false, 0, 0);
  assert.equal(audio.paused[0], false);
  assert.equal(ctx.sources[2].offset, 0);
});

test("pausing during asynchronous decode preserves active time only", async () => {
  const {audio, entry, ctx} = engine();
  let resolve;
  audio.load = () => null;
  audio.whenLoaded = () => new Promise((done) => { resolve = done; });
  audio.play(0, "slow", false, 0, 0);
  ctx.currentTime = 1;
  audio.pauseChannel(0, true);
  ctx.currentTime = 12;
  resolve(entry);
  await Promise.resolve();
  assert.equal(ctx.sources.length, 0);
  ctx.currentTime = 20;
  audio.pauseChannel(0, false);
  assert.equal(ctx.sources[0].offset, 1);
});

test("decode completion after a pause/resume excludes the paused interval", async () => {
  const {audio, entry, ctx} = engine();
  let resolve;
  audio.load = () => null;
  audio.whenLoaded = () => new Promise((done) => { resolve = done; });
  audio.play(0, "slow", false, 0, 0);
  ctx.currentTime = 1;
  audio.pauseChannel(0, true);
  ctx.currentTime = 11;
  audio.pauseChannel(0, false);
  ctx.currentTime = 12;
  resolve(entry);
  await Promise.resolve();
  assert.equal(ctx.sources[0].offset, 2);
});

test("active decode failures reach the player while cancelled requests stay cancelled", async () => {
  const {audio} = engine();
  let reject;
  audio.load = () => null;
  audio.whenLoaded = () => new Promise((resolve, fail) => { reject = fail; });
  audio.play(0, "cancelled", false, 0, 0);
  audio.stop(0);
  reject(new Error("obsolete request"));
  await Promise.resolve();
  assert.deepEqual(audio.errors, []);
  audio.play(0, "broken", false, 0, 0);
  const warn = console.warn;
  console.warn = () => {};
  try {
    reject(new Error("bad audio header"));
    await Promise.resolve();
  } finally {
    console.warn = warn;
  }
  assert.match(audio.errors[0], /broken.*bad audio header/);
  assert.equal(audio.pending[0], null);
});

test("video duration reads cached metadata without allocating decoded samples", () => {
  const rate = 48000, frames = rate * 10, wav = Buffer.alloc(44 + frames * 4);
  wav.write("RIFF"); wav.writeUInt32LE(wav.length - 8, 4); wav.write("WAVEfmt ", 8);
  wav.writeUInt32LE(16, 16); wav.writeUInt16LE(1, 20); wav.writeUInt16LE(2, 22);
  wav.writeUInt32LE(rate, 24); wav.writeUInt32LE(rate * 4, 28);
  wav.writeUInt16LE(4, 32); wav.writeUInt16LE(16, 34);
  wav.write("data", 36); wav.writeUInt32LE(frames * 4, 40);
  const {audio} = engine(); let lookups = 0, allocations = 0;
  audio.lookup = () => { lookups++; return wav; };
  const original = globalThis.Float32Array;
  globalThis.Float32Array = class extends original {
    constructor(...args) { super(...args); allocations++; }
  };
  try { for (let i = 0; i < 120; i++) assert.equal(audio.duration("clip"), 10); }
  finally { globalThis.Float32Array = original; }
  assert.equal(lookups, 1);
  assert.equal(allocations, 0);
  assert.equal(wavInfo(wav).frames, frames);
  wav.writeInt16LE(-32768, 44); wav.writeInt16LE(32767, 46);
  const decoded = decodeWav(wav);
  assert.equal(decoded.channels[0][0], -1);
  assert.equal(decoded.channels[1][0], 32767 / 32768);
  audio.lookup = () => null;
  assert.equal(audio.duration("missing"), 0);
  assert.match(audio.errors[0], /missing sound missing/);
});

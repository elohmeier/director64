import test from "node:test";
import assert from "node:assert/strict";
import {spawnSync} from "node:child_process";
import {parseVideo, frameAt, VideoPlayers} from "../../platforms/web/site/video.js";
import {videoProbe} from "../../platforms/web/node/video.mjs";

function pack(header, data = Buffer.from([1])) {
  const text = Buffer.from(JSON.stringify(header)), size = Buffer.alloc(4);
  size.writeUInt32LE(text.length);
  return Buffer.concat([size, text, data]);
}
const minimal = {codec: "vp8", width: 2, height: 2, frames: [[0, 1, 0, 1]]};
test("video indexes reject invalid dimensions, timestamps, keys and ranges", () => {
  assert.equal(parseVideo(pack(minimal)).width, 2);
  assert.throws(() => parseVideo(Buffer.alloc(2)), /truncated video header/);
  assert.throws(() => parseVideo(Buffer.from([255, 255, 255, 255])), /truncated video index/);
  assert.throws(() => parseVideo(pack(null)), /no indexed frames/);
  for (const change of [{width: -1}, {height: 0}, {width: 8192}, {height: "2"}])
    assert.throws(() => parseVideo(pack({...minimal, ...change})), /dimensions/);
  for (const frame of [[-1, 1, 0, 1], [0, 2, 0, 1], [0, 1, 0, 2], [0, 1, -1, 1]])
    assert.throws(() => parseVideo(pack({...minimal, frames: [frame]})), /frame index/);
  assert.throws(() => parseVideo(pack({...minimal, frames: [[0, 0, 0, 1]]})), /keyframe/);
  assert.equal(parseVideo(pack({...minimal, codec: "avc1.42C00A", width: 0, height: 0})).width, 0);
  assert.equal(frameAt([[0], [500000], [1000000]], 499999), 0);
  assert.equal(frameAt([[0], [500000], [1000000]], 500000), 1);
});

function audio() {
  const calls = [];
  return {calls, duration: () => 10,
    playVideo: (...args) => calls.push(["play", ...args]),
    stopVideo: (...args) => calls.push(["stop", ...args]),
    videoRate: (...args) => calls.push(["rate", ...args])};
}
test("browser video failures remain visible and close failed decoders", () => {
  const saved = {VideoDecoder: globalThis.VideoDecoder, OffscreenCanvas: globalThis.OffscreenCanvas,
    EncodedVideoChunk: globalThis.EncodedVideoChunk};
  let closed = 0, callbacks;
  try {
    globalThis.VideoDecoder = undefined;
    const missing = new VideoPlayers(() => null, audio());
    missing.update(1, "missing", "", 0, 1, 0, 1);
    assert.match(missing.errors[0], /missing video missing/);
    const unavailable = new VideoPlayers(() => pack(minimal), audio());
    unavailable.update(1, "clip", "", 0, 1, 0, 1);
    assert.match(unavailable.errors[0], /unavailable/);
    globalThis.OffscreenCanvas = class { getContext() { return {}; } };
    globalThis.VideoDecoder = class {
      state = "configured";
      configure() { throw new Error("unsupported configuration"); }
      close() { this.state = "closed"; closed++; }
    };
    const configure = new VideoPlayers(() => pack(minimal), audio());
    configure.update(1, "clip", "", 0, 1, 0, 1);
    assert.match(configure.errors[0], /unsupported configuration/);
    assert.equal(closed, 1);
    globalThis.EncodedVideoChunk = class { constructor(value) { Object.assign(this, value); } };
    globalThis.VideoDecoder = class {
      state = "configured";
      constructor(value) { callbacks = value; }
      configure() {}
      decode() { throw new Error("corrupt chunk"); }
      close() { this.state = "closed"; closed++; }
    };
    const decode = new VideoPlayers(() => pack(minimal), audio());
    decode.update(1, "clip", "", 0, 1, 0, 1);
    assert.match(decode.errors[0], /corrupt chunk/);
    callbacks.error(new Error("asynchronous failure"));
    assert.match(decode.errors[1], /asynchronous failure/);
    decode.closeAll();
    assert.equal(closed, 2);
  } finally { Object.assign(globalThis, saved); }
});
test("browser player identities keep linked movie audio and seeks independent", () => {
  const sound = audio(), players = new VideoPlayers(() => null, sound);
  players.update(1, "", "root", 0, 1, 0, 1);
  players.update(1002, "", "child", 120, 1, 7, 0.5);
  players.update(1002, "", "child", 120, 0, 7, 0.5);
  assert.deepEqual(sound.calls.filter(([op]) => op === "play"), [["play", 1, "root", 0], ["play", 1002, "child", 0.2]]);
  assert.deepEqual(sound.calls.filter(([op]) => op === "stop"), [["stop", 1002]]);
  assert.equal(players.players.size, 2);
  players.closeAll();
  assert.equal(players.players.size, 0);
});

test("audio-only QuickTime plays, pauses, seeks and finishes without a video decoder", () => {
  const sound = audio();
  sound.duration = () => 20.37;
  const players = new VideoPlayers(() => { throw new Error("audio-only media requested a picture"); }, sound);
  players.update(1, "", "sound-track", 0, 1, 1, 1);
  assert.equal(players.frame(1), null);
  assert.deepEqual(sound.calls.slice(0, 2), [["play", 1, "sound-track", 0], ["rate", 1, 1, 1]]);
  players.update(1, "", "sound-track", 600, 0, 2, 1);
  assert.deepEqual(sound.calls.at(-1), ["stop", 1]);
  players.update(1, "", "sound-track", 1200, 0, 3, 1);
  assert.equal(sound.calls.filter(([op]) => op === "play").length, 1);
  players.update(1, "", "sound-track", 1200, 1, 4, 0.5);
  assert.deepEqual(sound.calls.slice(-2), [["play", 1, "sound-track", 2], ["rate", 1, 1, 0.5]]);
  players.update(1, "", "sound-track", 12222, 0, 4, 0.5);
  assert.deepEqual(sound.calls.at(-1), ["stop", 1]);
  players.update(1, "", "sound-track", 0, 1, 5, 1);
  assert.deepEqual(sound.calls.slice(-2), [["play", 1, "sound-track", 0], ["rate", 1, 1, 1]]);
  players.update(1, null, "", 0, 0, 0, 0);
  assert.equal(players.players.size, 0);
  assert.deepEqual(players.errors, []);
});

test("late video outputs remain visible, queues stay bounded and rewinds reject old generations", () => {
  const saved = {VideoDecoder: globalThis.VideoDecoder, OffscreenCanvas: globalThis.OffscreenCanvas,
    EncodedVideoChunk: globalThis.EncodedVideoChunk};
  const decoders = [];
  let drawn = -1, closedFrames = 0, peakQueue = 0;
  try {
    globalThis.OffscreenCanvas = class {
      getContext() { return {drawImage(frame) { drawn = frame.timestamp; },
        getImageData() { return {data: new Uint8Array([drawn / 100000 + 1])}; }}; }
    };
    globalThis.EncodedVideoChunk = class { constructor(chunk) { Object.assign(this, chunk); } };
    globalThis.VideoDecoder = class {
      state = "configured"; queue = [];
      constructor(callbacks) { this.callbacks = callbacks; decoders.push(this); }
      get decodeQueueSize() { return this.queue.length; }
      configure() {} close() { this.state = "closed"; }
      decode(chunk) { this.queue.push(chunk.timestamp); peakQueue = Math.max(peakQueue, this.queue.length); }
      deliver(timestamp = this.queue.shift()) {
        this.callbacks.output({timestamp, close() { closedFrames++; }});
        this.ondequeue?.();
      }
    };
    const frames = Array.from({length: 60}, (_, i) => [i * 100000, i === 0 ? 1 : 0, i, 1]);
    const bytes = pack({...minimal, frames}, Buffer.alloc(60));
    const players = new VideoPlayers(() => bytes, audio());
    for (let i = 0; i < 12; i++) {
      players.update(1, "clip", "", i * 60, 1, 0, 1);
      if (i) decoders[0].deliver(); // every output is one requested frame late
    }
    assert.equal(players.revision, 11);
    assert.equal(players.frame(1).pixels[0], 11);
    decoders[0].deliver();
    assert.equal(players.frame(1).pixels[0], 12);
    players.update(1, "clip", "", 19 * 60, 1, 0, 1);
    assert(peakQueue <= 8);
    while (decoders[0].queue.length) decoders[0].deliver();
    assert.equal(players.frame(1).pixels[0], 20);
    players.update(1, "clip", "", 50 * 60, 1, 1, 1);
    assert.equal(decoders.length, 2);
    assert.equal(decoders[1].queue.length, 8);
    players.update(1, "clip", "", 51 * 60, 1, 1, 1);
    assert.equal(decoders.length, 2); // clock progress must not restart a bounded catch-up queue
    while (decoders[1].queue.length) decoders[1].deliver();
    assert.equal(players.frame(1).pixels[0], 52);
    assert(peakQueue <= 8);
    players.update(1, "clip", "", 0, 0, 1, 1);
    assert.equal(decoders.length, 3);
    assert.equal(players.frame(1), null);
    const revision = players.revision;
    decoders[0].deliver(1900000); // late callback from the closed decoder
    decoders[0].callbacks.error(new Error("obsolete decoder"));
    assert.equal(players.revision, revision);
    assert.equal(players.frame(1), null);
    decoders[2].deliver();
    assert.equal(players.frame(1).pixels[0], 1);
    assert.deepEqual(players.errors, []);
    players.update(1, "clip", "", -1, 0, 2, 1);
    assert.equal(players.frame(1), null);
    assert(closedFrames > 20);
    players.closeAll();
  } finally { Object.assign(globalThis, saved); }
});

function run(binary, args, input) {
  const result = spawnSync(binary, args, {input, maxBuffer: 4 * 1024 * 1024, timeout: 30000});
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stderr?.toString());
  return result.stdout;
}
// All frames are generated here. No retail or recovered media fixtures.
function fixture(codec) {
  const colors = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 0]];
  const rgb = Buffer.concat(colors.map((color) => {
    const out = Buffer.alloc(16 * 16 * 3);
    for (let i = 0; i < out.length; i += 3) out.set(color, i);
    return out;
  }));
  const h264 = codec === "h264";
  const bytes = run(process.env.FFMPEG ?? "ffmpeg", ["-hide_banner", "-loglevel", "error",
    "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "16x16", "-r", "2", "-i", "pipe:0",
    "-c:v", h264 ? "libx264" : "libvpx", "-pix_fmt", "yuv420p", "-g", "2", "-threads", "1",
    ...(h264 ? ["-bf", "0", "-x264-params", "repeat-headers=1:aud=1:scenecut=0"] : ["-auto-alt-ref", "0"]),
    "-f", h264 ? "h264" : "ivf", "pipe:1"], rgb);
  let frames, data;
  if (h264) {
    const info = JSON.parse(run(process.env.FFPROBE ?? "ffprobe", ["-v", "error", "-f", "h264", "-i", "pipe:0",
      "-show_packets", "-show_entries", "packet=pos,size,flags", "-of", "json"], bytes));
    frames = info.packets.map((p, i) => [i * 500000, p.flags.includes("K") ? 1 : 0, Number(p.pos), Number(p.size)]);
    data = bytes;
  } else {
    frames = []; const chunks = []; let offset = 0;
    for (let at = 32; at < bytes.length;) {
      const length = bytes.readUInt32LE(at), chunk = bytes.subarray(at + 12, at + 12 + length);
      frames.push([frames.length * 500000, (chunk[0] & 1) ? 0 : 1, offset, length]);
      chunks.push(chunk); offset += length; at += 12 + length;
    }
    data = Buffer.concat(chunks);
  }
  assert.equal(frames.length, 4);
  return pack({codec: h264 ? "avc1.42C00A" : "vp8", width: h264 ? 0 : 16,
    height: h264 ? 0 : 16, frames}, data);
}
for (const codec of ["vp8", "h264"]) test(`probe decodes generated ${codec}, keyframe seeks and independent slots`, () => {
  const bytes = fixture(codec), probe = videoProbe(() => bytes);
  probe.update(1, "clip", "", 0);
  let frame = probe.frame(1);
  assert.equal(frame.width, 16); assert.equal(frame.height, 16);
  assert(frame.pixels[0] > 220 && frame.pixels[1] < 25 && frame.pixels[2] < 25);
  probe.update(1002, "clip", "", 300);
  frame = probe.frame(1002);
  assert(frame.pixels[0] < 25 && frame.pixels[1] > 220 && frame.pixels[2] < 25);
  assert(probe.frame(1).pixels[0] > 220);
  probe.update(1, "clip", "", 900);
  frame = probe.frame(1);
  assert(frame.pixels[0] > 220 && frame.pixels[1] > 220 && frame.pixels[2] < 25);
  probe.update(1, "clip", "", 0);
  assert(probe.frame(1).pixels[0] > 220 && probe.frame(1).pixels[1] < 25);
  const revision = probe.revision();
  probe.update(1, "clip", "", 0);
  assert.equal(probe.revision(), revision);
  probe.update(1002, null, "", 0);
  assert.equal(probe.frame(1002), null);
});
test("probe failures expose missing assets, unsupported codecs and damaged frames", () => {
  assert.throws(() => videoProbe(() => null).update(1, "missing", "", 0), /missing probe video/);
  assert.throws(() => videoProbe(() => pack({...minimal, codec: "unknown"})).update(1, "clip", "", 0), /unsupported/);
  assert.throws(() => videoProbe(() => pack(minimal)).update(1, "clip", "", 0), /probe video|PAM picture/);
  const saved = process.env.FFMPEG;
  process.env.FFMPEG = "/nonexistent/director64-test-ffmpeg";
  try { assert.throws(() => videoProbe(() => pack(minimal)).update(1, "clip", "", 0), /ENOENT/); }
  finally { if (saved === undefined) delete process.env.FFMPEG; else process.env.FFMPEG = saved; }
});

import test from "node:test";
import assert from "node:assert/strict";
import {spawnSync} from "node:child_process";
import {encodeVideo, encodeMpeg, mpegAudio} from "../../platforms/android/export/video.mjs";
import {parseVideo} from "../../platforms/web/site/video.js";
import {videoProbe} from "../../platforms/web/node/video.mjs";

test("APK encoding preserves frame times and reused decoder pixel buffers", async () => {
  let index = 0;
  const pixels = new Uint8Array(16 * 16 * 4), times = [0, 100000, 250000];
  const bytes = await encodeVideo({width: 16, height: 16, next() {
    if (index === times.length) return null;
    pixels.fill(0);
    for (let i = index; i < pixels.length; i += 4) pixels[i] = 255;
    return {microseconds: times[index++], rgbx: pixels};
  }});
  const video = parseVideo(bytes);
  assert.equal(video.codec, "vp09.00.10.08");
  assert.deepEqual(video.frames.map((frame) => frame[0]), times);
  const probe = videoProbe(() => bytes);
  for (let i = 0; i < times.length; i++) {
    probe.update(1, "generated", "", times[i] * 600 / 1e6);
    const rgb = probe.frame(1).pixels.subarray(0, 3);
    assert(rgb[i] > 220 && rgb[(i + 1) % 3] < 25);
  }
});

test("APK encoder process and source failures reject without hanging", async () => {
  await assert.rejects(encodeVideo({width: 16, height: 16, next() {
    throw new Error("synthetic source decoder failure");
  }}), /synthetic source decoder failure/);
  const saved = process.env.FFMPEG;
  process.env.FFMPEG = "/nonexistent/director64-test-ffmpeg";
  try {
    await assert.rejects(encodeVideo({width: 16, height: 16,
      next: () => ({microseconds: 0, rgbx: new Uint8Array(1024)})}), /ENOENT/);
  } finally {
    if (saved === undefined) delete process.env.FFMPEG;
    else process.env.FFMPEG = saved;
  }
});

function mpeg(withAudio) {
  const result = spawnSync(process.env.FFMPEG ?? "ffmpeg", ["-hide_banner", "-loglevel", "error",
    "-f", "lavfi", "-i", "testsrc2=size=32x32:rate=25:duration=0.4",
    ...(withAudio ? ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=0.4",
      "-c:a", "mp2", "-b:a", "128k"] : []),
    "-c:v", "mpeg1video", "-bf", "2", "-f", "mpeg", "pipe:1"],
  {maxBuffer: 1024 * 1024, timeout: 30000});
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stderr.toString());
  return result.stdout;
}
test("APK MPEG export separates silence, audio alignment and decoder failures", async () => {
  const silent = mpeg(false), voiced = mpeg(true);
  assert.equal(await mpegAudio(silent), null);
  const video = parseVideo(await encodeMpeg(voiced));
  assert.equal(video.frames.length, 10);
  assert.equal(video.frames[0][0], 0);
  assert.equal(video.frames[9][0], 360000);
  const plain = await mpegAudio(voiced);
  const early = await mpegAudio(voiced, {audio_lead_seconds: 0.01});
  const late = await mpegAudio(voiced, {audio_lead_seconds: -0.01});
  const view = new DataView(plain.buffer, plain.byteOffset, plain.byteLength);
  const step = Math.round(view.getUint32(24, true) / 100) * view.getUint16(22, true) * 2;
  assert.equal(plain.length - early.length, step);
  assert.equal(late.length - plain.length, step);
  await assert.rejects(mpegAudio(new Uint8Array([1, 2, 3])), /ffmpeg exited/);
  await assert.rejects(encodeMpeg(new Uint8Array([1, 2, 3])), /ffmpeg exited/);
});

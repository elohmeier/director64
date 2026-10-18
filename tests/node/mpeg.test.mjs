import test from "node:test";
import assert from "node:assert/strict";
import {mpegTools} from "../../platforms/web/import/mpeg.mjs";
import {encodeVideo} from "../../platforms/web/import/vp8.mjs";

function decoder({frames = 3, audio = true, opens = true} = {}) {
  let next = 32;
  const handles = new Map(), freed = [], closed = [];
  const module = {
    HEAPU8: new Uint8Array(65536),
    _malloc: (size) => { const ptr = next; next += size; return ptr; },
    _free: (ptr) => freed.push(ptr),
    _d64m_open: () => { if (!opens) return 0; const h = next++; handles.set(h, {video: 0, audio: 0}); return h; },
    _d64m_close: (h) => { closed.push(h); handles.delete(h); },
    _d64m_width: () => 2, _d64m_height: () => 2,
    _d64m_corrupt: () => 0,
    _d64m_frame: (h) => handles.get(h).video++ < frames ? 4096 : 0,
    _d64m_audio_streams: () => audio ? 1 : 0,
    _d64m_sample_rate: () => 48000,
    _d64m_samples: (h) => handles.get(h).audio++ === 0 ? 8192 : 0,
  };
  return {module, handles, freed, closed};
}
const info = {width: 2, height: 2, rate: 25, scale: 1, frames: 3, decoded_frames: 3};
const source = new Uint8Array([0, 0, 1]);
async function collect(frames) {
  const times = [];
  for (let f; (f = frames.next());) times.push(f.microseconds);
  return times;
}
test("MPEG decoders isolate simultaneous imports and release their source buffers", async () => {
  const d = decoder();
  const tools = await mpegTools(async () => d.module, null, {mpegStreams: (bytes) => ({video: bytes, audio: bytes}), mpegInfo: () => info}, collect);
  assert.deepEqual(await Promise.all([tools.encodeMpeg(source), tools.encodeMpeg(source)]),
    [[0, 40000, 80000], [0, 40000, 80000]]);
  assert.equal(d.handles.size, 0);
  assert.equal(d.closed.length, 2);
  assert.equal(d.freed.length, 2);
});
test("leading open-GOP pictures are skipped and display timestamps start at zero", async () => {
  const d = decoder();
  const tools = await mpegTools(async () => d.module, null,
    {mpegStreams: (bytes) => ({video: bytes, audio: bytes}), mpegInfo: () => ({...info, skipped_initial_frames: 2, decoded_frames: 1})}, collect);
  assert.deepEqual(await tools.encodeMpeg(source), [0]);
});
test("a missing decoded frame fails and closes the decoder", async () => {
  const d = decoder({frames: 2});
  const tools = await mpegTools(async () => d.module, null, {mpegStreams: (bytes) => ({video: bytes, audio: bytes}), mpegInfo: () => info}, collect);
  await assert.rejects(tools.encodeMpeg(source), /decoded 2 of 3/);
  assert.equal(d.handles.size, 0);
  assert.equal(d.freed.length, 1);
});
test("decoder and encoder failures release owned input", async () => {
  const d = decoder({opens: false});
  const tools = await mpegTools(async () => d.module, null, {mpegStreams: (bytes) => ({video: bytes, audio: bytes}), mpegInfo: () => info}, collect);
  await assert.rejects(tools.encodeMpeg(source), /stream headers/);
  assert.equal(d.freed.length, 1);
  const e = decoder();
  const failing = await mpegTools(async () => e.module, null, {mpegStreams: (bytes) => ({video: bytes, audio: bytes}), mpegInfo: () => info},
    async () => { throw new Error("encoder failure"); });
  await assert.rejects(failing.encodeMpeg(source), /encoder failure/);
  assert.equal(e.handles.size, 0);
});
test("MPEG audio aligns to video PTS and distinguishes silent streams", async () => {
  for (const lead of [-0.01, 0, 0.01]) {
    const d = decoder();
    const tools = await mpegTools(async () => d.module, null,
      {mpegStreams: (bytes) => ({video: bytes, audio: bytes}), mpegInfo: () => ({...info, audio_lead_seconds: lead})}, collect);
    const wav = await tools.mpegAudio(source);
    assert.equal(wav.length, 44 + (1152 - Math.round(lead * 48000)) * 4);
    assert.equal(new DataView(wav.buffer).getUint32(40, true), wav.length - 44);
    assert.equal(d.handles.size, 0);
  }
  const d = decoder({audio: false});
  const tools = await mpegTools(async () => d.module, null, {mpegStreams: (bytes) => ({video: bytes, audio: bytes}), mpegInfo: () => info}, collect);
  assert.equal(await tools.mpegAudio(source), null);
  assert.equal(d.handles.size, 0);
});
test("VP8 configuration failure closes its encoder", async () => {
  const saved = globalThis.VideoEncoder;
  let closed = false;
  globalThis.VideoEncoder = class {
    configure() { throw new Error("unsupported encoder"); }
    close() { closed = true; }
  };
  try {
    await assert.rejects(encodeVideo({width: 1, height: 1}), /unsupported encoder/);
    assert.equal(closed, true);
  } finally { globalThis.VideoEncoder = saved; }
});

import assert from "node:assert/strict";
import test from "node:test";
import {decodeWav, isOpusSound, loopBounds, readOpusHeader} from "../../platforms/web/site/audio.js";
import {FLASH_BYTES, SaveStore, plausibleFlash} from "../../platforms/web/site/saves.js";

function wav({bits = 8, channels = 1, rate = 22050, samples = [], code = 1, trailing = []}) {
  const bytesPerSample = bits / 8;
  const data = Buffer.alloc(samples.length * bytesPerSample);
  samples.forEach((s, i) => bits === 8 ? data.writeUInt8(s, i) : data.writeInt16LE(s, i * 2));
  const fmt = Buffer.alloc(16);
  fmt.writeUInt16LE(code, 0);
  fmt.writeUInt16LE(channels, 2);
  fmt.writeUInt32LE(rate, 4);
  fmt.writeUInt32LE(rate * channels * bytesPerSample, 8);
  fmt.writeUInt16LE(channels * bytesPerSample, 12);
  fmt.writeUInt16LE(bits, 14);
  const chunk = (id, body) => {
    const head = Buffer.alloc(8);
    head.write(id, 0, "latin1");
    head.writeUInt32LE(body.length, 4);
    return Buffer.concat([head, body, body.length & 1 ? Buffer.alloc(1) : Buffer.alloc(0)]);
  };
  const body = Buffer.concat([Buffer.from("WAVE"), chunk("fmt ", fmt), chunk("LIST", Buffer.from("x")),
    chunk("data", data), Buffer.from(trailing)]);
  const riff = Buffer.alloc(8);
  riff.write("RIFF", 0, "latin1");
  riff.writeUInt32LE(body.length, 4);
  return new Uint8Array(Buffer.concat([riff, body]));
}

test("converted PCM sounds decode synchronously to float samples", () => {
  const eight = decodeWav(wav({samples: [128, 0, 255]}));
  assert.equal(eight.rate, 22050);
  assert.equal(eight.frames, 3);
  assert.deepEqual([...eight.channels[0]], [0, -1, 127 / 128]);
  const sixteen = decodeWav(wav({bits: 16, channels: 2, rate: 44100, samples: [0, -32768, 16384, 32767]}));
  assert.equal(sixteen.frames, 2);
  assert.deepEqual([...sixteen.channels[0]], [0, 0.5]);
  assert.deepEqual([...sixteen.channels[1]], [-1, 32767 / 32768]);
});

test("malformed or unsupported sounds are refused", () => {
  assert.throws(() => decodeWav(new Uint8Array(8)), /WAVE/);
  assert.throws(() => decodeWav(wav({code: 3, samples: [0]})), /unsupported/);
  assert.throws(() => decodeWav(wav({bits: 24, samples: []})), /unsupported/);
  const truncated = wav({samples: [1, 2, 3, 4]}).subarray(0, 50);
  assert.throws(() => decodeWav(truncated), /truncated|unsupported/);
});

test("imported Opus sounds carry their exact length and source timing", () => {
  const header = {codec: "opus", sampleRate: 48000, channels: 1, frames: 96000, sourceRate: 22050,
    sourceFrames: 44100, description: "", chunks: [[0, 0, 3]]};
  const text = Buffer.from(JSON.stringify(header));
  const bytes = new Uint8Array(Buffer.concat([Buffer.from("D64A"), Buffer.alloc(4), text, Buffer.from([1, 2, 3])]));
  new DataView(bytes.buffer).setUint32(4, text.length, true);
  assert.ok(isOpusSound(bytes));
  assert.ok(!isOpusSound(new Uint8Array(wav({samples: [128]}))));
  const sound = readOpusHeader(bytes);
  assert.equal(sound.frames / sound.sampleRate, 2);
  assert.equal(sound.sourceFrames / sound.sourceRate, 2);
  assert.deepEqual([...sound.data], [1, 2, 3]);
});

test("loop bounds follow the console: clamp the end, refuse an empty loop", () => {
  assert.deepEqual(loopBounds(10, 100, 50), {start: 10, end: 50});
  assert.deepEqual(loopBounds(0, 20, 50), {start: 0, end: 20});
  assert.equal(loopBounds(50, 60, 50), null);
  assert.equal(loopBounds(5, 5, 50), null);
});

test("imported saves must look like an archive or erased flash", () => {
  const erased = new Uint8Array(FLASH_BYTES).fill(255);
  assert.ok(plausibleFlash(erased));
  const saved = erased.slice();
  saved.set(Buffer.from("F64D"), 0);
  assert.ok(plausibleFlash(saved));
  assert.ok(!plausibleFlash(erased.subarray(1)));
  const garbage = new Uint8Array(FLASH_BYTES);
  garbage[0] = 7;
  assert.ok(!plausibleFlash(garbage));
});

// A fake origin-private directory: files as byte arrays; writes can be failed.
function directory({fail = null} = {}) {
  const files = new Map();
  return {
    files,
    async getFileHandle(name, options = {}) {
      if (!files.has(name) && !options.create) throw Object.assign(new Error("missing"), {name: "NotFoundError"});
      return {
        async getFile() { return {arrayBuffer: async () => files.get(name).buffer.slice(0)}; },
        async createWritable() {
          let staged;
          return {
            async write(bytes) {
              if (fail) throw Object.assign(new Error("quota"), {name: fail});
              staged = bytes.slice();
            },
            async close() { files.set(name, staged); },
            async abort() {},
          };
        },
      };
    },
    async removeEntry(name) { files.delete(name); },
  };
}
function store(dir) {
  const saves = new SaveStore("director64/test");
  saves.directory = dir;
  const states = [];
  saves.addEventListener("state", (event) => states.push(event.detail.kind));
  return {saves, states};
}
const settled = (saves) => new Promise((resolve) => {
  const poll = () => (saves.busy ? setTimeout(poll, 1) : resolve());
  poll();
});

test("a save reports saved only after its write closed, newest generation last", async () => {
  const dir = directory();
  const {saves, states} = store(dir);
  const first = new Uint8Array(FLASH_BYTES).fill(1), second = new Uint8Array(FLASH_BYTES).fill(2);
  saves.commit(first);
  saves.commit(second);
  assert.ok(saves.busy);
  await settled(saves);
  assert.deepEqual(dir.files.get("flash.bin"), second);
  assert.equal(states.at(-1), "saved");
  assert.deepEqual(await saves.read(), second);
});

test("a failed write is reported and keeps the previous generation", async () => {
  const dir = directory();
  const previous = new Uint8Array(FLASH_BYTES).fill(9);
  dir.files.set("flash.bin", previous);
  const {saves, states} = store(dir);
  dir.getFileHandle = directory({fail: "QuotaExceededError"}).getFileHandle.bind({files: dir.files});
  saves.commit(new Uint8Array(FLASH_BYTES));
  await settled(saves);
  assert.equal(states.at(-1), "failed");
  assert.match(saves.state.message, /quota/);
  assert.deepEqual(dir.files.get("flash.bin"), previous);
});

test("import keeps a backup until it loads; restoring brings it back", async () => {
  const dir = directory();
  const {saves} = store(dir);
  const old = new Uint8Array(FLASH_BYTES).fill(3), imported = new Uint8Array(FLASH_BYTES).fill(4);
  dir.files.set("flash.bin", old);
  await saves.replace(imported);
  assert.deepEqual(dir.files.get("flash.bin"), imported);
  assert.deepEqual(dir.files.get("flash.bak"), old);
  assert.ok(await saves.restoreBackup());
  assert.deepEqual(dir.files.get("flash.bin"), old);
  await saves.discardBackup();
  assert.ok(!dir.files.has("flash.bak"));
});

test("without durable storage a commit says so rather than claiming success", () => {
  const {saves, states} = store(null);
  saves.commit(new Uint8Array(FLASH_BYTES));
  assert.deepEqual(states, ["unavailable"]);
});

import assert from "node:assert/strict";
import test from "node:test";
import {fileOperations, loadConverter, wasmSpan} from "../../platforms/web/import/convert.mjs";

test("wasm32 high-bit addresses are unsigned and invalid ranges fail explicitly", () => {
  const observed = [];
  const view = {length: 0x90000000, subarray: (first, last) => observed.push([first, last])};
  wasmSpan(view, -2144845120, 28);
  assert.deepEqual(observed, [[2150122176, 2150122204]]);
  assert.throws(() => wasmSpan(view, 0x8fffffff, 2), /exceeds/);
  assert.throws(() => wasmSpan(view, 0, -1), /exceeds/);
  const full = {length: 0x100000000, subarray: (first, last) => [first, last]};
  assert.deepEqual(wasmSpan(full, -16, 16), [0xfffffff0, 0x100000000]);
});

test("host file replies use the current memory after growth and preserve high-bit offsets", () => {
  const name = new TextEncoder().encode("/source"), data = Uint8Array.of(4, 5, 6);
  let current = {length: 0x90000000, subarray: (first, last) => {
    assert.equal(first, 0x80000008); assert.equal(last, first + name.length); return name;
  }};
  const host = fileOperations(() => ({read: (path) => {
    assert.equal(path, "/source"); return data;
  }}), () => current);
  assert.equal(host.d64c_fs(0, -2147483640, name.length, 0, 0), 3);
  let written;
  current = {length: 0x90010000, subarray: (first, last) => {
    assert.equal(first, 0x80010000); assert.equal(last, first + 3);
    return {set: (bytes) => { written = [...bytes]; }};
  }};
  host.d64c_fs_take(-2147418112);
  assert.deepEqual(written, [4, 5, 6]);
});

test("converter allocations and returned data refresh detached views after real memory growth", async (t) => {
  const memory = new WebAssembly.Memory({initial: 1, maximum: 3});
  const before = new Uint8Array(memory.buffer);
  let resultPointer;
  const exports = {
    memory,
    d64c_alloc: () => { memory.grow(1); return 65536; },
    d64c_free: () => {},
    d64c_aiff_wav: (pointer, length) => {
      assert.equal(before.byteLength, 0);
      assert.deepEqual([...new Uint8Array(memory.buffer, pointer, length)], [9, 8]);
      memory.grow(1); resultPointer = 131072;
      new Uint8Array(memory.buffer, resultPointer, 3).set([7, 6, 5]); return 0;
    },
    d64c_result_ptr: () => resultPointer,
    d64c_result_len: () => 3,
  };
  const original = WebAssembly.instantiate;
  t.after(() => { WebAssembly.instantiate = original; });
  WebAssembly.instantiate = async () => ({instance: {exports}});
  const converter = await loadConverter(new Uint8Array());
  assert.deepEqual([...converter.aiffWav(Uint8Array.of(9, 8))], [7, 6, 5]);
});

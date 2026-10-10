import assert from "node:assert/strict";
import test from "node:test";
import {readLogical} from "../../platforms/web/import/pipeline.mjs";

// Logical sectors 0..3, each filled with its number, framed as a raw rip.
function raw(mode) {
  const data = mode === 1 ? 16 : 24, image = new Uint8Array(4 * 2352);
  for (let n = 0; n < 4; n++) {
    const frame = image.subarray(n * 2352, (n + 1) * 2352);
    frame.set([0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0]);
    frame[15] = mode;
    frame.fill(n, data, data + 2048);
  }
  return {image, layout: {stride: 2352, data}};
}

test("a raw rip reads at logical offsets across sectors", () => {
  for (const mode of [1, 2]) {
    const {image, layout} = raw(mode);
    const readAt = (offset, length) => image.subarray(offset, offset + length);
    const bytes = readLogical(readAt, layout, 2040, 2060);
    assert.deepEqual([bytes[0], bytes[7], bytes[8], bytes[2055], bytes[2056]], [0, 0, 1, 1, 2]);
    assert.equal(bytes.length, 2060);
    assert.equal(readLogical(readAt, layout, 4096, 0).length, 0);
  }
});

test("a cooked image reads directly; a damaged raw sector reads as it is", () => {
  const cooked = new Uint8Array([1, 2, 3, 4]);
  assert.deepEqual(readLogical((o, l) => cooked.subarray(o, o + l), {stride: 2048, data: 0}, 1, 2), new Uint8Array([2, 3]));
  const {image, layout} = raw(1);
  image[2352 + 15] = 0x62;
  assert.deepEqual(readLogical((o, l) => image.subarray(o, o + l), layout, 2048, 3), new Uint8Array([1, 1, 1]));
});

test("past the image's end a file reads as zeros", () => {
  const cooked = new Uint8Array([1, 2]);
  assert.deepEqual(readLogical((o, l) => cooked.subarray(o, o + l), {stride: 2048, data: 0}, 1, 3), new Uint8Array([2, 0, 0]));
  const {image, layout} = raw(1);
  const bytes = readLogical((o, l) => image.subarray(o, o + l), layout, 3 * 2048 + 2046, 4);
  assert.deepEqual(bytes, new Uint8Array([3, 3, 0, 0]));
});

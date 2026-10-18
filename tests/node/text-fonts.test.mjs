import assert from "node:assert/strict";
import test from "node:test";
import {textRasterizer} from "../../platforms/web/site/player.js";

function environment() {
  const previous = {document: globalThis.document, FontFace: globalThis.FontFace};
  const canvas = {};
  const scratch = {canvas, measureText: (text) => ({fontBoundingBoxAscent: 11, width: text.length * 8}),
    clearRect() {}, fillText() {}, getImageData: (x, y, width, height) => ({data: new Uint8Array(width * height * 4)})};
  globalThis.document = {fonts: {add() {}}, createElement: () => ({getContext: () => scratch})};
  globalThis.FontFace = class {
    constructor(name, bytes) { this.name = name; this.bytes = bytes; }
    async load() {
      if (this.bytes instanceof Uint8Array && this.bytes[0] === 0) throw new Error("invalid font bytes");
      return this;
    }
  };
  return () => { Object.assign(globalThis, previous); };
}

test("authored text never silently switches to the builtin font", async () => {
  const restore = environment();
  try {
    const rasterize = await textRasterizer({index: {3: [0, 1]}, blob: Uint8Array.of(1)});
    assert.equal(rasterize("abc", 10, 10, null).length, 100);
    assert.equal(rasterize("abc", 10, 10, {font: 3, size: 12}).length, 100);
    assert.equal(rasterize.layout("abc", 10, 10, {font: 3, size: 12}).length, 12);
    const missing = {font: 6, size: 16};
    assert.throws(() => rasterize("abc", 10, 10, missing), /missing authored font 6 at size 16/);
    assert.throws(() => rasterize.layout("abc", 10, 10, missing), /missing authored font 6 at size 16/);
    await assert.rejects(textRasterizer({index: {6: [0, 1]}, blob: Uint8Array.of(0)}), /font 6 could not be loaded/);
  } finally { restore(); }
});

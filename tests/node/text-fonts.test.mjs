import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";
import {runInNewContext} from "node:vm";
import {textRasterizer} from "../../platforms/web/site/player.js";
import {formatText} from "../../platforms/web/site/rich-text.js";

test("Wasm rich formats retain signed metrics and no-wrap flags across 20-byte records", () => {
  const bytes=new Uint8Array(80), view=new DataView(bytes.buffer);
  let library;
  runInNewContext(readFileSync("platforms/web/library.js","utf8"),{
    addToLibrary: value=>{library=value;}, HEAPU8:bytes, HEAPU16:new Uint16Array(bytes.buffer),
    HEAP16:new Int16Array(bytes.buffer), HEAPU32:new Uint32Array(bytes.buffer),
  });
  view.setUint32(20,4,true);view.setUint32(24,0x123456,true);
  bytes.set([2,18,3,3],28);view.setInt16(32,-2,true);view.setInt16(34,24,true);
  view.setInt16(36,-1,true);view.setUint16(38,1,true);
  view.setUint32(40,9,true);bytes.set([3,12,0,0],48);
  const formats=library.$d64_text_formats(20,2);
  assert.deepEqual(JSON.parse(JSON.stringify(formats[0])),{offset:4,color:0x123456,
    font:2,size:18,face:3,align:3,ascent:-2,lineHeight:24,charSpacing:-1,layoutFlags:1});
  assert.equal(formats[1].offset,9);assert.equal(formats[1].font,3);assert.equal(formats[1].layoutFlags,0);
});

function environment() {
  const previous = {document: globalThis.document, FontFace: globalThis.FontFace};
  const canvas = {}, drawn = [];
  const scratch = {canvas, measureText: (text) => ({fontBoundingBoxAscent: 11, width: text.length * 8}),
    clearRect() {drawn.length=0;}, fillText(text,x,y) {drawn.push({text,x,y,font:this.font,color:this.fillStyle});}, fillRect() {}, getImageData: (x, y, width, height) => ({data: new Uint8Array(width * height * 4)})};
  globalThis.document = {fonts: {add() {}}, createElement: () => ({getContext: () => scratch})};
  globalThis.FontFace = class {
    constructor(name, bytes) { this.name = name; this.bytes = bytes; }
    async load() {
      if (this.bytes instanceof Uint8Array && this.bytes[0] === 0) throw new Error("invalid font bytes");
      return this;
    }
  };
  const restore = () => { Object.assign(globalThis, previous); };
  restore.drawn = drawn;
  return restore;
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

test("long words wrap at glyphs in the same rows used for text hit boxes", async () => {
  const restore = environment();
  try {
    const rasterize = await textRasterizer({index: {3: [0, 1]}, blob: Uint8Array.of(1)});
    const style = {font: 3, size: 12, lineHeight: 14};
    const box = text => Array.from(rasterize.layout(text, 40, 100, style));
    rasterize("abcdefghijk",40,100,style);
    assert.deepEqual(restore.drawn.map(({text})=>text),["abcde","fghij","k"]);
    rasterize("abc",40,100,{...style,face:3});
    assert.match(restore.drawn[0].font,/^italic bold 12px/);
    const long = box("abcdefghijk");
    assert.deepEqual([long[1],long[5*4+1],long[10*4+1]], [0,14,28]);
    assert.deepEqual([long[5*4],long[10*4]], [0,0]);
    const paragraphs = box("abc\r\nabcdefghijk");
    assert.equal(paragraphs[5*4+1],14);
    assert.equal(paragraphs[10*4+1],28);
    assert.equal(paragraphs[15*4+1],42);
    const narrow = Array.from(rasterize.layout("ab",1,100,style));
    assert.deepEqual([narrow[1],narrow[5]], [0,14]);
  } finally { restore(); }
});

test("mixed runs retain font, size, face, colour and shared row baseline", async () => {
  const restore=environment();
  try {
    const rasterize=await textRasterizer({index:{1:[0,1],2:[1,1]},blob:Uint8Array.of(1,1)});
    const formats=[{offset:0,font:1,size:12,face:0,align:1,ascent:11,lineHeight:14,color:0xff0000},
      {offset:2,font:2,size:24,face:3,align:1,ascent:21,lineHeight:28,color:0x0000ff}];
    rasterize.richText("ABCD EF",40,100,formats);
    assert.equal(restore.drawn[0].y,21);assert.match(restore.drawn[0].font,/12px "d64-font-1"/);
    assert.equal(restore.drawn[0].color,"#ff0000");
    assert.equal(restore.drawn[1].y,21);assert.match(restore.drawn[1].font,/italic bold 24px "d64-font-2"/);
    assert.equal(restore.drawn[1].color,"#0000ff");
    const boxes=rasterize.richLayout("ABCD EF",40,100,formats);
    assert.equal(boxes[3],28);assert.equal(boxes[5*4+1],28);
    assert.throws(()=>rasterize.richText("x",40,40,[{...formats[0],font:99}]),/missing authored font 99/);
  } finally {restore();}
});

test("full justification pads interword hit boxes and keeps the paragraph's last row left", async () => {
  const restore=environment();
  try {
    const rasterize=await textRasterizer({index:{1:[0,1]},blob:Uint8Array.of(1)});
    const style={offset:0,font:1,size:12,align:3,ascent:11,lineHeight:14,color:0};
    const text="AA BB CC DD", boxes=rasterize.richLayout(text,50,100,[style]);
    assert.deepEqual(Array.from(boxes.slice(2*4,3*4)),[16,0,18,14]);
    assert.deepEqual(Array.from(boxes.slice(3*4,4*4)),[34,0,8,14]);
    assert.deepEqual(Array.from(boxes.slice(6*4,7*4)),[0,14,8,14]);
    assert.deepEqual(Array.from(boxes.slice(8*4,9*4)),[16,14,8,14]);
    rasterize.richText(text,50,100,[style]);
    assert.equal(restore.drawn.find(part=>part.text==="BB").x,34);
  } finally {restore();}
});

test("no-wrap retains full offscreen character geometry and explicit paragraph breaks", async () => {
  const restore=environment();
  try {
    const rasterize=await textRasterizer({index:{1:[0,1]},blob:Uint8Array.of(1)});
    const style={offset:0,font:1,size:12,align:0,ascent:11,lineHeight:14,color:0,layoutFlags:1};
    const text="AA BB CC\rDD", boxes=rasterize.richLayout(text,24,40,[style]);
    assert.deepEqual(Array.from(boxes.slice(6*4,7*4)),[48,0,8,14]);
    assert.deepEqual(Array.from(boxes.slice(9*4,10*4)),[0,14,8,14]);
    rasterize.richText(text,24,40,[style]);
    assert.deepEqual(restore.drawn.map(({text,x,y})=>({text,x,y})),
      [{text:"AA BB CC",x:0,y:11},{text:"DD",x:0,y:25}]);
    // A stage hit inside the viewport cannot accidentally match an offscreen
    // letter, although charPosToLoc retains its complete unwrapped position.
    assert.equal(boxes[6*4] < 24,false);
  } finally {restore();}
});

test("classic fields keep baseline-position cells separate from row advance", async () => {
  const restore=environment();
  try {
    const rasterize=await textRasterizer({index:{1:[0,1]},blob:Uint8Array.of(1)});
    const style={offset:0,font:1,size:12,ascent:11,lineHeight:15,layoutFlags:2};
    const boxes=rasterize.richLayout("AB\rCD",100,80,[style]);
    assert.equal(boxes[3],14);assert.equal(boxes[3*4+1],15);
    assert.equal(boxes[3*4+1]+boxes[3*4+3]-1,28);
    const fixed=rasterize.richLayout("AB\rCD",100,80,[{...style,lineHeight:32}]);
    assert.equal(fixed[3],14);assert.equal(fixed[3*4+1],32);
    assert.equal(fixed[3*4+1]+fixed[3*4+3]-1,45);
  }finally {restore();}
});

test("large negative tracking keeps offscreen positions from wrapping back into the stage", async () => {
  const restore=environment();
  try {
    const rasterize=await textRasterizer({index:{1:[0,1]},blob:Uint8Array.of(1)});
    const style={offset:0,font:1,size:12,ascent:11,lineHeight:15,charSpacing:-1000};
    const boxes=rasterize.richLayout("A".repeat(70),100,40,[style]);
    assert.equal(boxes[1*4],-992);assert.equal(boxes[34*4],-32768);
    assert.equal(boxes[69*4],-32768);assert.equal(boxes[69*4+2],-992);
  }finally {restore();}
});

test("negative advances retain the final caret observed in original Arial18", () => {
  const measure=value=>[...value].reduce((sum,c)=>sum+(c==="C"||c==="D"?13:12),0);
  const {boxes}=formatText("ABCD",100,[{offset:0,size:18,ascent:17,lineHeight:21,charSpacing:-1000}],measure);
  assert.deepEqual([boxes[0],boxes[4],boxes[8],boxes[12]],[0,-988,-1976,-2963]);
  assert.equal(boxes[12]+boxes[14],-3950);
});

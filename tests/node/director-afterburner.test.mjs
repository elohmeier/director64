import test from "node:test";
import assert from "node:assert/strict";
import {deflateSync} from "node:zlib";
import {afterburnerMap, decodeSound} from "../../tools/director/afterburner.mjs";
import {quotedLiterals} from "../../tools/director/dump-format.mjs";

function integer(n) {
  const bytes = [n % 128];
  for (n = Math.floor(n / 128); n; n = Math.floor(n / 128)) bytes.unshift(n % 128 | 128);
  return Buffer.from(bytes);
}
const tag = s => Buffer.from(s).reverse();
const chunk = (name, bytes) => Buffer.concat([tag(name), integer(bytes.length), bytes]);
function fixture() {
  const codecs = Buffer.alloc(34);
  codecs.writeUInt16LE(2);
  Buffer.from("89a80472d0afcf11a22200a02453444c", "hex").copy(codecs, 18);
  const inline = deflateSync(Buffer.concat([integer(4), Buffer.from("test")]));
  const resource = (id, offset, size, length, codec, type) =>
    Buffer.concat([...[id, offset, size, length, codec].map(integer), tag(type)]);
  const map = Buffer.concat([integer(0), integer(0), integer(3),
    resource(2, 0, inline.length, 5, 0, "FGEI"),
    resource(3, inline.length, 4, 100, 1, "snd "),
    resource(4, 0xffffffff, 4, 4, 0, "TEST")]);
  return Buffer.concat([Buffer.from("XFIR\0\0\0\0MDGF", "latin1"), chunk("Fver", integer(0x501)),
    chunk("Fcdr", deflateSync(codecs)),
    chunk("ABMP", Buffer.concat([integer(0), integer(map.length), deflateSync(map)])),
    tag("FGEI"), integer(0), inline, Buffer.from("mp3!")]);
}

test("Afterburner uses the complete bounded map and inline segment", () => {
  const bytes = fixture(), resources = afterburnerMap(bytes);
  assert.equal(resources.length, 3);
  assert.equal(resources.find(r => r.id === 3).data.toString(), "mp3!");
  assert.equal(resources.find(r => r.id === 3).sound, true);
  assert.equal(resources.find(r => r.id === 4).data.toString(), "test");
  for (const length of [0, 11, 30, bytes.length - 1]) {
    if (length < 4) assert.equal(afterburnerMap(bytes.subarray(0, length)), null);
    else assert.throws(() => afterburnerMap(bytes.subarray(0, length)));
  }
  assert.equal(afterburnerMap(Buffer.from("ordinary movie")), null);
});

function sound() {
  const b = Buffer.alloc(84);
  b.writeUInt16BE(2, 0); b.writeUInt16BE(1, 4); b.writeUInt16BE(0x8051, 6);
  b.writeUInt32BE(14, 10); b.writeUInt32BE(1, 18); b.writeUInt16BE(22050, 22);
  b[34] = 0xff; b[35] = 60; b.writeUInt32BE(3, 36); b.writeUInt16BE(16, 62);
  b.writeUInt32BE(2, 78); b[82] = 255; b[83] = 251;
  return b;
}
test("SWA retains the header and exact declared PCM after priming", () => {
  const input = sound();
  const pcm = Buffer.from([0, 1, 0, 2, 0, 3, 0, 4, 0, 5]);
  const out = decodeSound(input, (bytes, format) => {
    assert.deepEqual(format, {bits: 16, rate: 22050, channels: 1});
    assert.deepEqual(bytes, Buffer.from([255, 251]));
    return pcm;
  });
  assert.deepEqual(out.subarray(0, 78), input.subarray(0, 78));
  assert.deepEqual(out.subarray(78), pcm.subarray(4));
  assert.throws(() => decodeSound(input, () => pcm.subarray(0, 8)), /short SWA decode/);
  assert.throws(() => decodeSound(input.subarray(0, 82), () => pcm), /missing SWA bitstream/);
});

test("Lscr quote recovery preserves literals without altering bytecode", () => {
  const scripts = {casts: [{scripts: [{lingo: 'on f\n return "a"b"\nend', bytecode: "original"}]}]};
  const records = [{fourCC: "Lscr", data: {literals: [{type: 1, value: 'a"b'}]}}];
  const result = quotedLiterals(records, scripts);
  assert.equal(result.casts[0].scripts[0].lingo, 'on f\n return ("a" & QUOTE & "b")\nend');
  assert.equal(result.casts[0].scripts[0].bytecode, "original");
  assert.equal(scripts.casts[0].scripts[0].lingo, 'on f\n return "a"b"\nend');
});

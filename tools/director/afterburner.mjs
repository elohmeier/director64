// Host-side recovery of Shockwave-compressed Mac snd resources. The locked
// parser's minimp3 wrapper rejects valid SWA streams in Löwenzahn 5. Read the
// actual Afterburner map (not KEY*'s stale slots), retain its complete chunk
// inventory, and decode those sounds with the converter's FFmpeg dependency.
import {inflateSync} from "node:zlib";
import {spawnSync} from "node:child_process";

const LIMIT = 256 * 1024 * 1024;
class Reader {
  constructor(bytes, little) { this.bytes = Buffer.from(bytes); this.at = 0; this.little = little; }
  take(n) {
    if (!Number.isSafeInteger(n) || n < 0 || n > this.bytes.length - this.at)
      throw new Error("truncated Afterburner resource");
    const b = this.bytes.subarray(this.at, this.at + n); this.at += n; return b;
  }
  uint16() { const b = this.take(2); return this.little ? b.readUInt16LE() : b.readUInt16BE(); }
  tag() { const b = this.take(4); return (this.little ? Buffer.from(b).reverse() : b).toString("latin1"); }
  varint() {
    let value = 0;
    for (let i = 0; i < 5; i++) {
      const b = this.take(1)[0]; value = value * 128 + (b & 127);
      if (value > 0xffffffff) throw new Error("Afterburner integer overflow");
      if (!(b & 128)) return value;
    }
    throw new Error("unterminated Afterburner integer");
  }
  chunk(tag) {
    if (this.tag() !== tag) throw new Error(`expected Afterburner ${tag}`);
    return this.take(this.varint());
  }
}
function inflate(bytes, length) {
  if (length > LIMIT) throw new Error("Afterburner expansion exceeds conversion limit");
  const out = inflateSync(bytes, {maxOutputLength: length ?? LIMIT});
  if (length !== undefined && out.length !== length) throw new Error("Afterburner expansion size mismatch");
  return out;
}

export function afterburnerMap(bytes) {
  const signature = Buffer.from(bytes.subarray(0, 4)).toString("latin1");
  if (!["XFIR", "RIFX"].includes(signature)) return null;
  const r = new Reader(bytes, signature === "XFIR");
  r.take(8);
  if (!["FGDM", "FGDC"].includes(r.tag())) return null;
  r.chunk("Fver");
  const codecs = new Reader(inflate(r.chunk("Fcdr")), r.little);
  const count = codecs.uint16();
  if (count > 256) throw new Error("excessive Afterburner codecs");
  const soundCodecs = new Set();
  for (let i = 0; i < count; i++) {
    const guid = codecs.take(16);
    const first = r.little ? guid.readUInt32LE(0) : guid.readUInt32BE(0);
    const second = r.little ? guid.readUInt16LE(4) : guid.readUInt16BE(4);
    const third = r.little ? guid.readUInt16LE(6) : guid.readUInt16BE(6);
    if (first === 0x7204a889 && second === 0xafd0 && third === 0x11cf &&
        guid.subarray(8).toString("hex") === "a22200a02453444c") soundCodecs.add(i);
  }
  const packed = new Reader(r.chunk("ABMP"), r.little);
  if (packed.varint() >= count) throw new Error("invalid Afterburner map codec");
  const expanded = packed.varint();
  const map = new Reader(inflate(packed.take(packed.bytes.length - packed.at), expanded), r.little);
  map.varint(); map.varint();
  const resources = new Map();
  const n = map.varint();
  if (n > 1000000) throw new Error("excessive Afterburner resources");
  for (let i = 0; i < n; i++) {
    const id = map.varint(), offset = map.varint() | 0;
    const size = map.varint(), length = map.varint(), codec = map.varint(), fourCC = map.tag();
    if (resources.has(id) || codec >= count || size > LIMIT || length > LIMIT)
      throw new Error("invalid Afterburner resource map");
    resources.set(id, {id, offset, size, length, codec, fourCC});
  }
  if (map.at !== map.bytes.length) throw new Error("trailing Afterburner resource map");
  if (![...resources.values()].some(r => soundCodecs.has(r.codec))) return null;
  if (r.tag() !== "FGEI") throw new Error("missing Afterburner initial segment");
  r.varint();
  const base = r.at;
  const initial = resources.get(2);
  if (!initial) throw new Error("missing Afterburner initial resource");
  const ils = new Reader(inflate(r.take(initial.size), initial.length), r.little);
  while (ils.at < ils.bytes.length) {
    const id = ils.varint(), resource = resources.get(id);
    if (!resource || resource.offset >= 0 || resource.data)
      throw new Error("invalid Afterburner inline resource");
    resource.data = ils.take(resource.size);
  }
  for (const resource of resources.values()) {
    if (resource.offset >= 0) {
      r.at = base + resource.offset; resource.data = r.take(resource.size);
    } else if (!resource.data) {
      if (resource.size === 0) resource.data = Buffer.alloc(0);
      else throw new Error(`missing Afterburner inline resource ${resource.id}`);
    }
    resource.sound = soundCodecs.has(resource.codec);
  }
  return [...resources.values()];
}

export function decodeSound(bytes, decode) {
  const b = Buffer.from(bytes);
  const need = (at, size) => { if (at < 0 || at + size > b.length) throw new Error("truncated SWA sound"); };
  need(0, 6);
  const format = b.readUInt16BE(0);
  let commands;
  if (format === 1) commands = 4 + 6 * b.readUInt16BE(2);
  else if (format === 2) commands = 4;
  else throw new Error("unsupported SWA sound format");
  need(commands, 10);
  if (b.readUInt16BE(commands) !== 1 || ![0x8050, 0x8051].includes(b.readUInt16BE(commands + 2)))
    throw new Error("unsupported SWA sound commands");
  const start = b.readUInt32BE(commands + 6);
  if (start !== commands + 10) throw new Error("invalid SWA header offset");
  need(start, 22);
  const encoding = b[start + 20], rate = b.readUInt16BE(start + 8);
  const param = b.readUInt32BE(start + 4);
  let frames = param, bits = 8, channels = 1, size = 22;
  if ([0xff, 0xfd].includes(encoding)) {
    need(start, 64); size = 64; frames = b.readUInt32BE(start + 22);
    channels = param; bits = b.readUInt16BE(start + 48);
  } else if (encoding !== 0) throw new Error("unsupported SWA header encoding");
  const samples = frames * channels * bits / 8;
  if (![8, 16].includes(bits) || ![1, 2].includes(channels) || !rate || !frames || samples > LIMIT)
    throw new Error("invalid SWA sample bounds");
  const at = start + size; need(at, 4);
  if (at + 4 === b.length) throw new Error("missing SWA bitstream");
  const skip = b.readUInt32BE(at);
  const pcm = decode(b.subarray(at + 4), {rate, bits, channels});
  const trim = skip * channels * bits / 8;
  if (trim + samples > pcm.length) throw new Error(`short SWA decode: need ${trim + samples}, got ${pcm.length}`);
  const header = Buffer.from(b.subarray(0, at));
  if (encoding === 0xfd) header[start + 20] = 0xff;
  return Buffer.concat([header, pcm.subarray(trim, trim + samples)]);
}

function ffmpegSound(mp3, {bits, rate, channels}) {
  const format = bits === 16 ? "s16be" : "u8";
  const result = spawnSync(process.env.FFMPEG || "ffmpeg", ["-v", "error", "-xerror", "-f", "mp3",
    "-i", "pipe:0", "-ar", String(rate), "-ac", String(channels), "-f", format, "pipe:1"],
    {input: mp3, maxBuffer: LIMIT});
  if (result.error || result.status !== 0)
    throw new Error(`SWA decoding failed: ${result.error || result.stderr?.toString()}`);
  return result.stdout;
}

export function afterburnerChunks(bytes, file) {
  const map = afterburnerMap(bytes);
  if (!map?.some(r => r.sound)) return undefined;
  return map.filter(r => r.id !== 0).map(r => {
    if (r.sound) {
      if (r.fourCC !== "snd ") throw new Error(`SWA codec on unexpected ${r.fourCC} resource ${r.id}`);
      let data;
      try { data = decodeSound(r.data, ffmpegSound); }
      catch (error) {
        // A published resource can declare PCM frames but contain no MP3
        // bytes at all. Preserve it verbatim: the native sound converter
        // reports its invalid sample bounds, rather than inventing audio.
        if (error.message !== "missing SWA bitstream")
          throw new Error(`SWA resource ${r.id}: ${error.message}`);
        data = r.data;
      }
      return {fourCC: r.fourCC, id: r.id, data};
    }
    const chunk = file.getChunk(r.fourCC, r.id);
    if (!chunk) throw new Error(`parser omitted ${r.fourCC} resource ${r.id}`);
    return chunk;
  });
}

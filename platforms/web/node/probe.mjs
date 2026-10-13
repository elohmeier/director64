// The native probes' line protocol (games/*/tests/director_probe.c) over the
// browser runtime's Wasm module, so the parity tool and the games' journey
// scripts drive the browser build exactly as they drive the native probe:
//   probe.mjs SITE MOVIE rpc   then one command per line on stdin.
// Only the game's entry movie can start: direct-activity fixtures are a
// feature of the native probe, not of the browser runtime.
import {existsSync, readFileSync} from "node:fs";
import {inflateRawSync} from "node:zlib";
import {createInterface} from "node:readline";
import {join} from "node:path";
import {pathToFileURL} from "node:url";

const [site, movie = "START", mode] = process.argv.slice(2);
if (!site || mode !== "rpc") {
  process.stderr.write("usage: probe.mjs SITE MOVIE rpc\n");
  process.exit(2);
}
const build = JSON.parse(readFileSync(join(site, "build.json"), "utf8"));
if (movie.toUpperCase() !== build.game.entry_movie.toUpperCase()) {
  process.stderr.write(`the browser runtime starts at ${build.game.entry_movie} only\n`);
  process.exit(2);
}
function pack(kind) {
  if (!existsSync(join(site, "assets", `${kind}.json`))) return () => null;
  const index = JSON.parse(readFileSync(join(site, "assets", `${kind}.json`), "utf8"));
  const blob = readFileSync(join(site, "assets", `${kind}.bin`));
  return (name) => {
    const entry = index[name];
    if (!entry) return null;
    const bytes = blob.subarray(entry[0], entry[0] + entry[1]);
    // "D64Z" + raw length + raw deflate, as the player inflates on load
    // (platforms/web/site/player.js); WebP images ("D64W") go to the runtime.
    if (bytes.length >= 8 && bytes[0] === 0x44 && bytes[1] === 0x36 && bytes[2] === 0x34 && bytes[3] === 0x5a)
      return new Uint8Array(inflateRawSync(bytes.subarray(8)));
    return bytes;
  };
}
const images = pack("images");
const data = pack("data");
const {default: createDirector64} = await import(pathToFileURL(join(site, "director64.mjs")).href);
const module = await createDirector64({
  director64Host: {
    asset: images,
    data,
    // The native probe holds a playFile stream's channel for a nominal second.
    stream: (channel, name) => (name ? 1000 : 0),
    // No fonts here: text lays out on a nominal fixed advance, wrapped as the
    // page wraps it, so text hit tests (pointToWord, hyperlinks) can be driven.
    layout: (text, width, height, style) => {
      const advance = style ? Math.max(1, Math.round(style.size * 0.55)) : 7;
      const step = style ? (style.lineHeight > 0 ? style.lineHeight : Math.round(style.size * 1.2)) : 13;
      const boxes = new Int16Array(text.length * 4);
      let x = 0, y = 0;
      for (let i = 0; i < text.length; i++) {
        const c = text[i];
        if (c === "\r" || c === "\n") {
          if (c === "\r" && text[i + 1] === "\n") i++;
          x = 0;
          y += step;
          continue;
        }
        if (c !== " " && x > 0 && style) {
          // Wrap a word that would cross the box edge.
          let end = i;
          while (end < text.length && !" \r\n".includes(text[end])) end++;
          if ((i === 0 || text[i - 1] === " ") && x + (end - i) * advance > width) {
            x = 0;
            y += step;
          }
        }
        boxes.set([x, y, advance, step], i * 4);
        x += advance;
      }
      return boxes;
    },
    trace: (text) => process.stderr.write(`NATIVE_TRACE ${text}\n`),
  },
  print: (text) => process.stderr.write(text + "\n"),
  printErr: (text) => process.stderr.write(text + "\n"),
});
if (build.package) {
  const bytes = readFileSync(join(site, build.package.file));
  const pointer = module._malloc(bytes.length);
  module.HEAPU8.set(bytes, pointer);
  const loaded = module._d64_load_package(pointer, bytes.length);
  module._free(pointer);
  if (!loaded) {
    process.stderr.write(`package rejected: ${module.UTF8ToString(module._d64_package_error())}\n`);
    process.exit(3);
  }
}
// The native probe starts from erased flash; so does a fresh browser profile.
module.HEAPU8.fill(255, module._d64_flash(), module._d64_flash() + module._d64_flash_bytes());
const out = (text) => new Promise((resolve) => process.stdout.write(text, resolve));
await out(module.ccall("d64_boot_rpc", "string", ["number"], [42]));
let status = 0;
for await (const line of createInterface({input: process.stdin, crlfDelay: Infinity})) {
  const reply = module.ccall("d64_rpc", "string", ["string"], [line.trimEnd()]);
  if (reply === "") { status = 2; break; }
  await out(reply);
}
if (!status && module.UTF8ToString(module._d64_error())) status = 1;
process.exit(status);

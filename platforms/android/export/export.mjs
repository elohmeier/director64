// Converts a disc image for the game's Android TV APK and web player
// (docs/android.md): the conversion pipeline (platforms/web/import) under
// Node, with FFmpeg for video and audio. Writes the game's package and asset
// packs, and the manifest last, so a directory without one is incomplete.
//   node export.mjs TOOLS_DIR SOURCE PROFILE_JSON PACKAGE_META MANIFEST_JSON OUT_DIR
import {readFileSync, writeFileSync, openSync, readSync, fstatSync, mkdirSync, rmSync} from "node:fs";
import {createRequire} from "node:module";
import {createHash} from "node:crypto";
import {join} from "node:path";
import {pathToFileURL} from "node:url";
import {availableParallelism} from "node:os";
import {Worker} from "node:worker_threads";
import {encodeAudio, encodeVideo, encodeMpeg, mpegAudio} from "./video.mjs";
import {encodeImage} from "./webp.mjs";

const [tools, sourcePath, profilePath, metaPath, manifestPath, out] = process.argv.slice(2);
if (!out) {
  console.error("usage: export.mjs TOOLS_DIR SOURCE PROFILE_JSON PACKAGE_META MANIFEST_JSON OUT_DIR");
  process.exit(2);
}
const root = new URL("../../../", import.meta.url).pathname;
// The image packer (webp.mjs) loads from the tools, here and in the workers.
process.env.D64_TOOLS = tools;
const {importGame} = await import(pathToFileURL(join(root, "platforms/web/import/pipeline.mjs")).href);
const {loadConverter} = await import(pathToFileURL(join(root, "platforms/web/import/convert.mjs")).href);
const {DirectorFile} = await import(pathToFileURL(join(root, "node_modules/projectorrays/dist/pkg/node.es.js")).href);
const {Pool} = await import(pathToFileURL(join(root, "platforms/web/import/pool.mjs")).href);
const converterModule = await WebAssembly.compile(readFileSync(join(tools, "convert.wasm")));
const converter = await loadConverter(converterModule);
const poolSize = Math.max(1, Math.min(availableParallelism() - 1, 12));
const pool = new Pool(Array.from({length: poolSize}, () => {
  const worker = new Worker(new URL("./pool-worker.mjs", import.meta.url));
  return {
    receive: (handler) => worker.on("message", handler),
    send: (message, transfer) => worker.postMessage(message, transfer ?? []),
    close: () => worker.terminate(),
  };
}), {type: "init", converter: converterModule, parser: null});
const handle = openSync(sourcePath, "r");
const size = fstatSync(handle).size;
const readAt = (offset, length) => {
  const bytes = new Uint8Array(length);
  readSync(handle, bytes, 0, length, offset);
  return bytes;
};
const started = performance.now();
const profile = JSON.parse(readFileSync(profilePath, "utf8"));
const toolManifest = JSON.parse(readFileSync(join(tools, "manifest.json"), "utf8"));
const systemFonts = Object.fromEntries(
  [...new Set((profile.policy.system_font_resources ?? []).map((r) => r.sha256))].map((hash) => {
    const name = toolManifest.files[`systemFont${hash}`];
    if (!name) throw new Error(`Missing original system font installer ${hash}`);
    return [hash, readFileSync(join(tools, name))];
  }),
);
let shown = "";
const result = await importGame({
  source: {size, readAt},
  profile,
  onProgress: ({stage}) => {
    if (stage !== shown) process.stderr.write(`  ${(shown = stage)}\n`);
  },
  tools: {
    converter, DirectorFile, pool, encodeAudio, encodeImage, encodeVideo, systemFonts,
    encodeMpeg: (bytes) => encodeMpeg(bytes, converter.mpegInfo(bytes)),
    mpegAudio: (bytes) => mpegAudio(bytes, converter.mpegInfo(bytes)),
    parserWasm: readFileSync(join(root, "node_modules/projectorrays/dist/projectorrays.wasm")),
    names: readFileSync(join(root, "runtime/lingo/names.txt"), "utf8"),
    bytecodeHeader: readFileSync(join(root, "runtime/lingo/lingo_bytecode.h")),
    meta: readFileSync(metaPath, "utf8"),
    fonts: {
      createMkfont: createRequire(import.meta.url)(join(tools, "mkfont.js")),
      createMksprite: createRequire(import.meta.url)(join(tools, "mksprite.js")),
      mkfontWasm: readFileSync(join(tools, "mkfont.wasm")),
      mkspriteWasm: readFileSync(join(tools, "mksprite.wasm")),
    },
    substituteFont: readFileSync(join(tools, "droid-sans.ttf")),
  },
});
pool.close();

const PACKS = ["images", "audio", "fonts", "streams", "data", "video", "print"];
rmSync(out, {recursive: true, force: true});
mkdirSync(out, {recursive: true});
const bytes = {}, hashes = {};
const write = (name, data) => {
  writeFileSync(join(out, name), data);
  bytes[name] = data.length;
  hashes[name] = createHash("sha256").update(data).digest("hex");
};
write("game.d64p", result.package);
for (const kind of PACKS) {
  const {blob, index} = result[kind] ?? {blob: new Uint8Array(), index: {}};
  write(`${kind}.bin`, blob);
  write(`${kind}.json`, new TextEncoder().encode(JSON.stringify(index)));
}
const manifest = {
  ...JSON.parse(readFileSync(manifestPath, "utf8")),
  created: new Date().toISOString(),
  bytes,
  sha256: hashes,
  report: result.report,
};
writeFileSync(join(out, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
// The scene model and program beside the game, for inspection; not shipped.
writeFileSync(join(out, "..", "model.json"), result.model);
writeFileSync(join(out, "..", "program.json"), result.program);
const total = Object.values(bytes).reduce((a, b) => a + b, 0);
console.log(JSON.stringify({seconds: Number(((performance.now() - started) / 1000).toFixed(1)),
  bytes: total, sounds: result.report.sounds, images: result.report.images, videos: result.report.videos}));

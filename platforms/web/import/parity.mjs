// Import parity: the browser importer's pipeline (the built bundle, over its
// in-memory file system and adapters) against the native pipeline's outputs
// for the same disc. Local-media gate; `director64 web --check-import` runs it.
//   node parity.mjs TOOLS_DIR ISO WORK_DIR PROFILE_JSON PACKAGE_META
// TOOLS_DIR holds the build's pipeline.mjs and convert.wasm;
// WORK_DIR is build/<game>/<source>. Prints a JSON summary; exits 1 on any
// difference.
import {readFileSync, writeFileSync, openSync, readSync, fstatSync, existsSync} from "node:fs";
import {createRequire} from "node:module";
import {isDeepStrictEqual} from "node:util";
import {dirname, join} from "node:path";
import {pathToFileURL} from "node:url";
import {availableParallelism} from "node:os";
import {Worker} from "node:worker_threads";

const [tools, iso, work, profilePath, metaPath] = process.argv.slice(2);
if (!metaPath) {
  console.error("usage: parity.mjs TOOLS_DIR ISO WORK_DIR PROFILE_JSON PACKAGE_META");
  process.exit(2);
}
const root = new URL("../../../", import.meta.url).pathname;
const {importGame} = await import(pathToFileURL(join(tools, "pipeline.mjs")).href);
const {loadConverter} = await import(pathToFileURL(join(root, "platforms/web/import/convert.mjs")).href);
const {DirectorFile} = await import(pathToFileURL(join(root, "node_modules/projectorrays/dist/pkg/node.es.js")).href);
const converterModule = await WebAssembly.compile(readFileSync(join(tools, "convert.wasm")));
const converter = await loadConverter(converterModule);
// The importer's worker pool, as the browser runs it (D64_POOL=0: in-thread).
const {Pool} = await import(pathToFileURL(join(root, "platforms/web/import/pool.mjs")).href);
const poolSize = process.env.D64_POOL === "0" ? 0 : Math.max(1, Math.min(availableParallelism() - 1, 12));
const pool = poolSize ? new Pool(Array.from({length: poolSize}, () => {
  const worker = new Worker(join(root, "platforms/web/node/pool-worker.mjs"));
  return {
    receive: (handler) => worker.on("message", handler),
    send: (message, transfer) => worker.postMessage(message, transfer ?? []),
    close: () => worker.terminate(),
  };
}), {type: "init", converter: converterModule, parser: null}) : undefined;
const handle = openSync(iso, "r");
const size = fstatSync(handle).size;
const readAt = (offset, length) => {
  const bytes = new Uint8Array(length);
  readSync(handle, bytes, 0, length, offset);
  return bytes;
};
const started = performance.now();
const result = await importGame({
  source: {size, readAt},
  profile: JSON.parse(readFileSync(profilePath, "utf8")),
  tools: {
    converter, DirectorFile, pool,
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
pool?.close();
// The program the console runs: with the port's fixes and launcher when it
// has them. A port's model is rewritten by its host post-processor, so the
// two sides compare as JSON.
const nativeProgram = existsSync(join(work, "aot/native-program.json"))
  ? join(work, "aot/native-program.json") : join(work, "aot/program.json");
const sameJson = (text, path) => isDeepStrictEqual(JSON.parse(text), JSON.parse(readFileSync(path, "utf8")));
const same = (a, b) => Buffer.compare(Buffer.from(a), b) === 0;
const summary = {
  seconds: Number(((performance.now() - started) / 1000).toFixed(1)),
  pool: poolSize,
  stages_ms: result.report.stages,
  convert_ms: result.report.convert,
  memory_bytes: result.report.memory_bytes,
  program: sameJson(result.program, nativeProgram),
  model: sameJson(result.model, join(work, "director/model.json")),
  package: same(result.package, readFileSync(join(work, "web/site/game.d64p"))),
  images: 0, images_identical: 0, sounds: 0, sounds_identical: 0,
};
for (const [name, [at, length]] of Object.entries(result.images.index)) {
  summary.images++;
  const native = join(work, "director/images", name);
  // The importer keeps images deflated; the console's files are plain.
  if (existsSync(native) && same(converter.inflate(result.images.blob.subarray(at, at + length)), readFileSync(native)))
    summary.images_identical++;
}
for (const [name, [at, length]] of Object.entries(result.audio.index)) {
  summary.sounds++;
  const native = join(work, "director/wav", name.replace(/\.wav64$/, ".wav"));
  if (existsSync(native) && same(result.audio.blob.subarray(at, at + length), readFileSync(native)))
    summary.sounds_identical++;
}
// The playFile streams and shipped data files, against the local build's
// packs (`director stream-pack` and the extracted folders).
for (const kind of ["streams", "data", "print"]) {
  const index = join(work, "web/site/assets", `${kind}.json`);
  if (!existsSync(index)) continue;
  summary[kind] = Object.keys(result[kind].index).length;
  summary[`${kind}_identical`] =
    isDeepStrictEqual(result[kind].index, JSON.parse(readFileSync(index, "utf8"))) &&
    same(result[kind].blob, readFileSync(join(work, "web/site/assets", `${kind}.bin`)));
}
// The importer's side of a JSON that differs, beside the profile, to diff.
for (const kind of ["program", "model"])
  if (!summary[kind]) writeFileSync(join(dirname(profilePath), `import-${kind}.json`), result[kind]);
console.log(JSON.stringify(summary));
const ok = summary.program && summary.model && summary.package &&
  summary.streams_identical !== false && summary.data_identical !== false &&
  summary.print_identical !== false &&
  summary.images === summary.images_identical && summary.sounds === summary.sounds_identical;
process.exit(ok ? 0 : 1);

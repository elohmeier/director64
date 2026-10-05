// Bundles the browser importer: the pipeline, the dump format and the
// ProjectorRays web entry. Nothing in it uses Node's APIs; the parser's
// Node-only branch keeps its dynamic imports, which a browser never runs.
//   node platforms/web/import/build.mjs OUT_DIR
// writes OUT_DIR/importer.js (a classic worker: the parser's glue loads with
// importScripts), OUT_DIR/pool.js (its pool's workers, also classic) and
// OUT_DIR/pipeline.mjs (the same code as an ES module, which the Node parity
// harness drives).
import path from "node:path";
import {fileURLToPath} from "node:url";
import * as esbuild from "esbuild";

const here = path.dirname(fileURLToPath(import.meta.url));
const common = {
  bundle: true,
  external: ["node:module", "node:url"],
  define: {"import.meta.url": '"file:///importer/importer.js"'},
  target: "es2022",
  legalComments: "eof",
  logLevel: "warning",
};
const out = process.argv[2];
if (!out) throw new Error("usage: build.mjs OUT_DIR");
await esbuild.build({...common, entryPoints: [path.join(here, "worker.mjs")], format: "iife",
  platform: "browser", outfile: path.join(out, "importer.js")});
await esbuild.build({...common, entryPoints: [path.join(here, "pool-worker.mjs")], format: "iife",
  platform: "browser", outfile: path.join(out, "pool.js")});
await esbuild.build({...common, entryPoints: [path.join(here, "pipeline.mjs")], format: "esm",
  platform: "neutral", outfile: path.join(out, "pipeline.mjs")});

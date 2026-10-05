#!/usr/bin/env node
// ProjectorRays is the one converter stage that is not Rust: this parses each
// Director file of a conversion plan (director64-aot director plan) and writes
// what the parser saw — chunks, parsed records, decompiled scripts — as a dump
// the Rust stages read (compiler/src/convert/dump.rs).
//   node tools/director/dump.mjs PLAN.json
import fs from "node:fs/promises";
import path from "node:path";
import {createHash} from "node:crypto";
import {isMain} from "./cli.mjs";
import {dumpDirector, PARSER_WASM_SHA256} from "./dump-format.mjs";

export {dumpDirector, PARSER_WASM_SHA256};

// Parses every entry of a plan; `read`/`write` give the embedder's file access.
export async function dumpPlan(plan, DirectorFile, read, write) {
  for (const entry of plan.dumps) {
    const file = await DirectorFile.read(await read(entry.input));
    try {
      await write(entry.output, dumpDirector(file));
    } finally {
      file.destroy();
    }
  }
}

async function main() {
  const [planPath] = process.argv.slice(2);
  if (!planPath) throw new Error("usage: dump.mjs PLAN.json");
  const wasm = await fs.readFile("node_modules/projectorrays/dist/projectorrays.wasm");
  if (createHash("sha256").update(wasm).digest("hex") !== PARSER_WASM_SHA256)
    throw new Error("unsupported ProjectorRays WASM; restore the locked npm dependency");
  const {DirectorFile} = await import("../../node_modules/projectorrays/dist/pkg/node.es.js");
  const plan = JSON.parse(await fs.readFile(planPath, "utf8"));
  await dumpPlan(plan, DirectorFile, (p) => fs.readFile(p), async (p, data) => {
    await fs.mkdir(path.dirname(p), {recursive: true});
    await fs.writeFile(p, data);
  });
  console.log(`Parsed ${plan.dumps.length} Director files`);
}

if (isMain(import.meta.url))
  main().catch((error) => {
    console.error(error);
    process.exit(1);
  });

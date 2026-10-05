// The importer's worker (bundled to importer.js by build.mjs). The page posts
// one import; this reads the chosen disc image through FileReaderSync, so
// every read is bounded and synchronous, and posts progress, then the game
// package and asset packs, or an error. Cancelling terminates the worker:
// nothing is stored until the page receives a finished result.
import {identify, importGame, ImportError} from "./pipeline.mjs";
import {loadConverter} from "./convert.mjs";
import {encodeAudio} from "./opus.mjs";
import {encodeVideo} from "./vp8.mjs";
import {Pool} from "./pool.mjs";
import {DirectorFile} from "projectorrays/web";

// The pool's workers (pool.js): one per core the page can spare, at most
// twelve; each holds its own converter and parser instance.
function startPool(url, converterModule, parser) {
  if (typeof Worker !== "function") return null;
  const size = Math.max(1, Math.min((self.navigator?.hardwareConcurrency ?? 4) - 1, 12));
  const endpoints = Array.from({length: size}, () => {
    const worker = new Worker(url);
    return {
      receive: (handler) => { worker.onmessage = ({data}) => handler(data); },
      send: (message, transfer) => worker.postMessage(message, transfer ?? []),
      close: () => worker.terminate(),
    };
  });
  return new Pool(endpoints, {type: "init", converter: converterModule, parser});
}

async function fetchBytes(url) {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
  return new Uint8Array(await response.arrayBuffer());
}
async function fetchText(url) {
  return new TextDecoder().decode(await fetchBytes(url));
}

let pool = null;

self.onmessage = async ({data}) => {
  if (data.type !== "import") return;
  const post = (message, transfer) => self.postMessage(message, transfer ?? []);
  try {
    const {file, profiles, tools: urls, meta} = data;
    post({type: "progress", stage: "tools", index: 0, stages: 1, fraction: 0});
    const [converterWasm, parserWasm, names, bytecodeHeader, mkfontWasm, mkspriteWasm, substituteFont] =
      await Promise.all([
        fetchBytes(urls.converter), fetchBytes(urls.parserWasm), fetchText(urls.names),
        fetchBytes(urls.bytecodeHeader), fetchBytes(urls.mkfontWasm), fetchBytes(urls.mkspriteWasm),
        fetchBytes(urls.substituteFont),
      ]);
    // libdragon's font tools, as classic scripts defining their factories.
    importScripts(urls.mkfont, urls.mksprite);
    const converterModule = await WebAssembly.compile(converterWasm);
    const converter = await loadConverter(converterModule);
    // A classic worker runs the parser glue with importScripts, which the
    // loader chooses only for its script-tag mode.
    await DirectorFile.loadModule({glueUrl: urls.parserGlue, wasmBinary: parserWasm, useScriptTag: true});
    const reader = new FileReaderSync();
    const source = {
      size: file.size,
      readAt: (offset, length) => new Uint8Array(reader.readAsArrayBuffer(file.slice(offset, offset + length))),
    };
    const progress = (update) => post({type: "progress", ...update});
    const {profile, digest} = identify(source, profiles, converter,
      (fraction) => progress({stage: "verify", index: 0, stages: 10, fraction}));
    post({type: "identified", slug: profile.slug, digest});
    pool = startPool(urls.pool, converterModule, {glueUrl: urls.parserGlue, wasmBinary: parserWasm});
    const result = await importGame({
      source, profile, verified: true, onProgress: progress,
      tools: {converter, DirectorFile, parserWasm, names, bytecodeHeader, substituteFont, encodeVideo, pool,
        encodeAudio: (wav) => encodeAudio(converter, wav),
        fonts: {createMkfont: self.createMkfont, createMksprite: self.createMksprite, mkfontWasm, mkspriteWasm},
        meta: JSON.stringify({...meta, game: profile.slug, source_sha256: profile.source.sha256,
          director_version: profile.port.director_version}, Object.keys({
          converter: 0, director_version: 0, game: 0, schema_version: 0, source_sha256: 0}).sort()) + "\n"},
    });
    const packs = {images: result.images, audio: result.audio, fonts: result.fonts,
      streams: result.streams, data: result.data, video: result.video, print: result.print};
    pool?.close();
    post({type: "done", slug: profile.slug, package: result.package, packs, report: result.report},
      [result.package.buffer, ...Object.values(packs).map((pack) => pack.blob.buffer)]);
  } catch (error) {
    pool?.close();
    post({type: "error", message: String(error?.message ?? error),
      kind: error instanceof ImportError ? "unsupported" : error?.name ?? "error"});
  }
};

// The dump format the Rust converter stages read (compiler/src/convert/
// dump.rs): what ProjectorRays saw of one Director file. No Node APIs, so the
// browser importer bundles it as is.
export const PARSER_WASM_SHA256 = "e4d7ab15a9cb20586d0e5afc54ad4d48412254d98b78a0c834b5adc8e2e3983e";
const encoder = new TextEncoder();

// One parsed file as the dump format: "D64DUMP1", u32le header length, the
// JSON header, then every chunk's bytes in header order.
export function dumpDirector(file) {
  const chunks = file.dumpChunks();
  const json = file.dumpJSON();
  const scripts = file.dumpScripts();
  // Decompiling a protected movie rewrites its parsed config (the version
  // placeholder and protection field); keep both views when they differ, so
  // each stage can read the one the JS stages read.
  const scripted = JSON.stringify(file.dumpJSON());
  const header = encoder.encode(JSON.stringify({
    json,
    scripts,
    ...(scripted !== JSON.stringify(json) ? {json_after_scripts: JSON.parse(scripted)} : {}),
    chunks: chunks.map((chunk) => [chunk.fourCC, chunk.id, chunk.data.length]),
  }));
  const total = 12 + header.length + chunks.reduce((n, chunk) => n + chunk.data.length, 0);
  const out = new Uint8Array(total);
  out.set(encoder.encode("D64DUMP1"), 0);
  new DataView(out.buffer).setUint32(8, header.length, true);
  out.set(header, 12);
  let at = 12 + header.length;
  for (const chunk of chunks) {
    out.set(chunk.data, at);
    at += chunk.data.length;
  }
  return out;
}

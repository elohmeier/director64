// The dump format the Rust converter stages read (compiler/src/convert/
// dump.rs): what ProjectorRays saw of one Director file. No Node APIs, so the
// browser importer bundles it as is.
export const PARSER_WASM_SHA256 = "e4d7ab15a9cb20586d0e5afc54ad4d48412254d98b78a0c834b5adc8e2e3983e";
const encoder = new TextEncoder();

// The parser prints embedded quote characters verbatim in string constants.
// Re-express only exact Lscr literals, using Lingo's QUOTE constant, so the
// recovered program keeps the original value and remains parseable.
export function quotedLiterals(records, scripts) {
  const literals = [...new Set(records.filter(c => c.fourCC === "Lscr")
    .flatMap(c => c.data?.literals ?? [])
    .filter(l => l.type === 1 && typeof l.value === "string" && l.value.includes('"') && !/[\r\n]/.test(l.value))
    .map(l => l.value))].sort((a, b) => b.length - a.length);
  if (!literals.length) return scripts;
  const result = structuredClone(scripts);
  for (const cast of result.casts ?? []) for (const script of cast.scripts ?? []) {
    for (const literal of literals) {
      const source = `"${literal}"`;
      const expression = `(${literal.split('"').map(part => `"${part}"`).join(" & QUOTE & ")})`;
      script.lingo = script.lingo.split(source).join(expression);
    }
  }
  return result;
}

// One parsed file as the dump format: "D64DUMP1", u32le header length, the
// JSON header, then every chunk's bytes in header order.
export function dumpDirector(file, chunks = file.dumpChunks()) {
  const json = file.dumpJSON();
  const scripts = quotedLiterals(json, file.dumpScripts());
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

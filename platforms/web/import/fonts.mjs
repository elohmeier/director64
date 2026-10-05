// The importer's font stage: every font variant the model manifests, packed
// into a font64 by libdragon's own mkfont (built as WebAssembly by
// platforms/web/tools/build-fonttools.sh), as `director64 assets` packs them
// for the console. Glyph metrics, ranges and kerning match the console's
// files; only where mkfont places glyphs in its atlases may differ.
//
// tools: {createMkfont, createMksprite, mkfontWasm, mkspriteWasm}, the two
// modules' factories (Node `require` or a worker's importScripts) and bytes.
export async function fontTools({createMkfont, createMksprite, mkfontWasm, mkspriteWasm}) {
  const quiet = {print: () => {}, printErr: () => {}};
  const mksprite = await createMksprite({...quiet, wasmBinary: mkspriteWasm});
  mksprite.FS.mkdir("/in");
  mksprite.FS.mkdir("/out");
  // mkfont's atlas subprocess (subprocess_shim.h): the PNG it wrote to
  // mksprite's stdin, packed with the arguments it passed.
  const runMksprite = (args, png) => {
    mksprite.FS.writeFile("/in/atlas.png", png);
    const status = mksprite.callMain([...args, "-o", "/out", "/in/atlas.png"]) ?? 0;
    const output = status ? new Uint8Array() : mksprite.FS.readFile("/out/atlas.sprite");
    if (!status)
      mksprite.FS.unlink("/out/atlas.sprite");
    return {status, output};
  };
  // One mkfont run per variant, in a fresh instance: mkfont keeps its
  // options and font state in globals.
  return async function mkfont(source, name, args) {
    const errors = [];
    const m = await createMkfont({
      wasmBinary: mkfontWasm, print: () => {}, printErr: (text) => errors.push(text),
      d64RunMksprite: runMksprite, preRun: [(module) => { module.ENV.N64_INST = "/n64"; }],
    });
    m.FS.mkdir("/work");
    m.FS.mkdir("/work/out");
    m.FS.writeFile(`/work/${name}`, source);
    const status = m.callMain([...args, "-o", "/work/out", `/work/${name}`]) ?? 0;
    const out = `/work/out/${name.replace(/\.[^.]+$/, "")}.font64`;
    if (status || !m.FS.analyzePath(out).exists)
      throw new Error(`mkfont ${name}: ${errors.slice(-3).join(" | ") || `status ${status}`}`);
    return m.FS.readFile(out);
  };
}

// Packs every variant of every font in `model` (director/model.json);
// read(asset) gives a converted font's bytes. Returns {"fonts/fN-S.font64": bytes}.
export async function packFonts(model, read, tools, onProgress = () => {}) {
  const mkfont = await fontTools(tools);
  const variants = (model.fonts ?? []).flatMap((font) => font.variants.map((v) => ({font, v})));
  const out = {};
  let done = 0;
  for (const {font, v} of variants) {
    // The console packs each size from a staging copy named after the output.
    const stem = v.asset.split("/").pop().replace(/\.font64$/, "");
    const extension = font.asset.slice(font.asset.lastIndexOf("."));
    out[v.asset] = await mkfont(read(font.asset), stem + extension, ["--size", String(v.size), "--range", "all"]);
    onProgress(++done / variants.length);
  }
  return out;
}

// The importer's image packer (platforms/web/tools/webp_tool.c, built by
// `director64 web` into the tools directory D64_TOOLS names): a stored image
// as lossless WebP in a "D64W" container, or null where that would not give
// back exactly the stored bytes.
import {readFileSync} from "node:fs";
import {createRequire} from "node:module";
import {join} from "node:path";

// libwebp lossless method 2 at quality 50: within 5% of its smallest output
// at a fiftieth of the time.
const METHOD = 2, QUALITY = 50;
let loading = null;
function load() {
  loading ??= (async () => {
    const tools = process.env.D64_TOOLS;
    const createWebp = createRequire(import.meta.url)(join(tools, "webp.js"));
    return createWebp({wasmBinary: readFileSync(join(tools, "webp.wasm"))});
  })();
  return loading;
}

export async function encodeImage(stored) {
  const module = await load();
  const input = module._malloc(stored.length), size = module._malloc(4);
  module.HEAPU8.set(stored, input);
  const out = module._d64w_encode(input, stored.length, METHOD, QUALITY, size);
  const length = module.HEAPU32[size >> 2];
  module._free(input);
  module._free(size);
  if (!out) return null;
  const packed = module.HEAPU8.slice(out, out + length);
  module._d64w_free(out);
  return packed;
}

export function unpackAsset(module, bytes) {
  if (bytes.length < 8 || String.fromCharCode(...bytes.subarray(0, 4)) !== "D64Z") return bytes;
  const length = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(4);
  if (!length || length > 64 * 1024 * 1024) throw new Error("invalid packed asset size");
  const input = module._malloc(bytes.length), output = module._malloc(length);
  try {
    if (!input || !output) throw new Error("cannot allocate packed asset");
    module.HEAPU8.set(bytes, input);
    if (module._d64_unpack_asset(input, bytes.length, output, length) !== length)
      throw new Error("invalid packed asset");
    return module.HEAPU8.slice(output, output + length);
  } finally {
    module._free(output); module._free(input);
  }
}

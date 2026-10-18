#!/usr/bin/env python3
"""Create ignored engine copies with synthetic brush resources for an oracle.

No executable code is changed. Run pattern-grid.lingo in a fresh original
engine process for each generated DLL, then restore the original DLL.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def bitmap(raw, wanted):
    def u16(p): return struct.unpack_from("<H", raw, p)[0]
    def u32(p): return struct.unpack_from("<I", raw, p)[0]
    pe = u32(0x3c)
    if raw[:2] != b"MZ" or raw[pe:pe + 4] != b"PE\0\0" or u16(pe + 24) != 0x10b:
        raise ValueError("expected PE32 engine DLL")
    sections = pe + 24 + u16(pe + 20)

    def offset(rva, length):
        for i in range(u16(pe + 6)):
            p = sections + 40 * i
            at, size = u32(p + 12), u32(p + 16)
            if at <= rva and rva - at + length <= size:
                result = u32(p + 20) + rva - at
                if result + length <= len(raw): return result
        raise ValueError("resource outside PE sections")

    root = offset(u32(pe + 24 + 96 + 16), u32(pe + 24 + 96 + 20))

    def entries(at):
        p = root + at
        return [(u32(p + 16 + 8 * i), u32(p + 20 + 8 * i))
                for i in range(u16(p + 12) + u16(p + 14))]

    def child(at, key):
        matches = [v for k, v in entries(at) if k == key and v & 0x80000000]
        if len(matches) != 1: raise ValueError("missing or ambiguous bitmap directory")
        return matches[0] & 0x7fffffff

    leaves = entries(child(child(0, 2), wanted))
    if len(leaves) != 1 or leaves[0][1] & 0x80000000:
        raise ValueError("expected one bitmap language")
    leaf = root + leaves[0][1]
    return offset(u32(leaf), u32(leaf + 4)), u32(leaf + 4)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--unaligned", action="store_true", help="2048 independent RGB samples instead of RGB5 census")
    args = parser.parse_args()
    original = args.engine.read_bytes()
    if hashlib.sha256(original).hexdigest() != args.sha256:
        raise SystemExit("selected original engine SHA-256 differs")
    args.output.mkdir(parents=True, exist_ok=True)
    records = []
    for batch in range(1 if args.unaligned else 16):
        raw = bytearray(original)
        inputs = []
        for resource in range(8):
            at, length = bitmap(raw, 20 + resource)
            header, width, height, planes, depth, compression = struct.unpack_from("<IiiHHI", raw, at)
            count = struct.unpack_from("<I", raw, at + 32)[0] or 256
            if (header, planes, depth, compression, count) != (40, 1, 8, 0, 256) or width < 16 or abs(height) < 16 or length < 1320:
                raise ValueError("probe requires an uncompressed indexed brush with room for16x16")
            struct.pack_into("<ii", raw, at + 4, 16, 16)
            struct.pack_into("<I", raw, at + 20, 256)
            for index in range(256):
                n = batch * 2048 + resource * 256 + index
                if args.unaligned:
                    color = list(hashlib.sha256(b"Director64 brush probe" + n.to_bytes(4, "little")).digest()[:3])
                else:
                    color = [(n >> 10) * 8, ((n >> 5) & 31) * 8, (n & 31) * 8]
                inputs.append(color)
                r, g, b = color
                raw[at + 40 + index * 4:at + 44 + index * 4] = bytes([b, g, r, 0])
                y, x = divmod(index, 16)
                row = 15 - y
                raw[at + 1064 + row * 16 + x] = index
        name = f"brush-{'unaligned' if args.unaligned else 'rgb5'}-{batch:02}.dll"
        target = args.output / name
        if target.resolve() == args.engine.resolve(): raise ValueError("output must not replace original")
        target.write_bytes(raw)
        records.append({"file": name, "sha256": hashlib.sha256(raw).hexdigest(), "input_rgb": inputs})
    (args.output / "inputs.json").write_text(json.dumps({"original_sha256": args.sha256, "batches": records}, indent=2) + "\n")


if __name__ == "__main__":
    main()

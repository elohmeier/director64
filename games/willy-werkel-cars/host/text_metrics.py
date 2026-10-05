"""Measure controller text with the pinned mkfont renderer, including kerning."""

import hashlib
import json
import os
import struct
import subprocess

from director64.toolchain import container_engine, inspect_image


def decode(data):
    if len(data) < 100 or data[:4] != b"FNT\x0b":
        raise ValueError("unsupported font64 metrics header")
    def u32(at):
        return struct.unpack_from(">I", data, at)[0]
    ranges, glyphs, kranges, kern = (u32(i) for i in (72, 80, 84, 92))
    indices = {}
    for i in range(u32(36)):
        first, count, glyph = struct.unpack_from(">IIi", data, ranges + i * 12)
        if glyph < 0:
            raise ValueError("sparse controller font range")
        for char in range(max(32, first), min(127, first + count)):
            indices[char] = glyph + char - first
    if set(indices) != set(range(32, 127)):
        raise ValueError("controller font lacks printable ASCII")
    advances = [data[glyphs + indices[c] * 8] for c in range(32, 127)]
    reverse = {g: c for c, g in indices.items()}
    pairs = []
    if kranges:
        for char, glyph in indices.items():
            lo, hi = struct.unpack_from(">HH", data, kranges + glyph * 4)
            if not lo:
                continue
            for i in range(lo, hi + 1):
                second, amount = struct.unpack_from(">hb", data, kern + i * 3)
                if second in reverse:
                    pairs.append([char, reverse[second], amount])
    return {"advances": advances, "kerning": pairs, "size": u32(8)}


def measure(game, font, size):
    sdk = inspect_image(
        game.root, os.environ.get("DIRECTOR64_TOOLCHAIN_IMAGE", "director64-toolchain:local")
    )
    key = hashlib.sha256(font.read_bytes() + sdk["Id"].encode() + str(size).encode()).hexdigest()
    output = game.work / "director/font-metrics" / key
    output.mkdir(parents=True, exist_ok=True)
    packed = output / (font.stem + ".font64")
    if not packed.exists():
        subprocess.run([
            container_engine(), "run", "--rm", "--network=none", "-v", f"{game.root}:/workdir",
            "-w", "/workdir", sdk["Id"], "mkfont", "--size", str(size),
            "--range", "0x20-0x7e", "-c", "0", "-o", str(output.relative_to(game.root)),
            str(font.relative_to(game.root)),
        ], check=True, stdout=subprocess.DEVNULL)
    result = decode(packed.read_bytes())
    (output / "metrics.json").write_text(json.dumps(result, indent=2) + "\n")
    return result

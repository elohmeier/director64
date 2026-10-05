"""Measure per-variant glyph metrics with the pinned mkfont renderer.

The runtime lays exercise text out in authored space: charPosOf highlight
boxes and the Flash prompt overlays both walk a per-style advance table.
The table comes from the same mkfont build that renders the glyphs, so
layout and rendering agree by construction. Coverage is windows-1252 text
stored as UTF-8: codepoints 32..255, intersected with the recovered font's
own codepoint set (mkfont fails closed on absent glyphs).
"""

import hashlib
import json
import os
import struct
import subprocess

from director64.toolchain import container_engine, inspect_image


def decode(data, first=32, last=255):
    """Advances, kerning and vertical metrics from a font64 build."""
    if len(data) < 100 or data[:4] != b"FNT\x0b":
        raise ValueError("unsupported font64 metrics header")

    def u32(at):
        return struct.unpack_from(">I", data, at)[0]

    def i32(at):
        return struct.unpack_from(">i", data, at)[0]

    ranges, glyphs, kranges, kern = (u32(i) for i in (72, 80, 84, 92))
    indices = {}
    for i in range(u32(36)):
        start, count, glyph = struct.unpack_from(">IIi", data, ranges + i * 12)
        if glyph < 0:
            # A sparse (hashed) range would need the displacement table;
            # the measurement build requests dense ranges only.
            raise ValueError("sparse metrics font range")
        for char in range(max(first, start), min(last + 1, start + count)):
            indices[char] = glyph + char - start
    if not all(c in indices for c in range(0x20, 0x7F)):
        raise ValueError("metrics font lacks printable ASCII")
    advances = [
        data[glyphs + indices[c] * 8] if c in indices else 0
        for c in range(first, last + 1)
    ]
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
    return {
        "advances": advances,
        "kerning": pairs,
        "size": u32(8),
        "ascent": i32(12),
        "descent": -i32(16),
    }


def _ranges(codepoints, first=32, last=255):
    """Contiguous --range arguments covering the font's own codepoints."""
    covered = sorted(c for c in codepoints if first <= c <= last)
    if not covered:
        raise ValueError("font covers no measurable codepoints")
    runs, start, previous = [], covered[0], covered[0]
    for c in covered[1:]:
        if c != previous + 1:
            runs.append((start, previous))
            start = c
        previous = c
    runs.append((start, previous))
    return [f"0x{a:X}-0x{b:X}" for a, b in runs]


def measure(game, font, codepoints, size):
    sdk = inspect_image(
        game.root, os.environ.get("DIRECTOR64_TOOLCHAIN_IMAGE", "director64-toolchain:local")
    )
    ranges = _ranges(codepoints)
    key = hashlib.sha256(
        font.read_bytes() + sdk["Id"].encode() + str(size).encode()
        + ",".join(ranges).encode()
    ).hexdigest()
    output = game.work / "director/font-metrics" / key
    output.mkdir(parents=True, exist_ok=True)
    packed = output / (font.stem + ".font64")
    if not packed.exists():
        subprocess.run(
            [
                container_engine(), "run", "--rm", "--network=none",
                "-v", f"{game.root}:/workdir", "-w", "/workdir", sdk["Id"],
                "mkfont", "--size", str(size),
                *(part for r in ranges for part in ("--range", r)),
                "-c", "0", "-o", str(output.relative_to(game.root)),
                str(font.relative_to(game.root)),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )
    result = decode(packed.read_bytes())
    (output / "metrics.json").write_text(json.dumps(result, indent=2) + "\n")
    return result

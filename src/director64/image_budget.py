"""Spend spare ROM on faster image codecs where the scenes draw.

The codec tiers pick a compression level by decompressed size: level 3
(Shrinkler) is the smallest and decodes at about 1 MB/s on the console,
level 2 at about 4, level 1 (LZ4) at about 7, for 1.26 and 1.63 times the
bytes. A port's `image_pack_budget_mib` says how much ROM the image pack
may take; whatever the tiers leave of it goes to promoting the images the
recorded working sets draw, in the order that saves the most decoding time
per ROM byte: an image the stage draws in several scenes and that packs
well at level 3 is promoted first. Images no scene draws keep their tier.
"""

from __future__ import annotations

# Decompression rates in bytes per second (the middle of the measured
# ranges, docs/roadmap.md and the codec-tier notes) and compressed size
# relative to level 3.
RATE = {1: 7_000_000.0, 2: 4_300_000.0, 3: 1_000_000.0}
RATIO = {1: 1.63, 2: 1.26, 3: 1.0}


def estimated_bytes(bytes_at_level: int, level: int, target: int) -> int:
    """The compressed size an image packed at `level` would take at `target`."""
    return int(round(bytes_at_level * RATIO[target] / RATIO[level]))


def seconds_saved(length: int, level: int, target: int) -> float:
    """Decoding time one load saves by moving from `level` to `target`."""
    return length / RATE[level] - length / RATE[target]


def solve(images: dict[str, dict], budget: int) -> dict[str, int]:
    """Promotions within a ROM budget. `images` maps a name to its `level`,
    compressed `bytes` at that level, decompressed `length` and `weight`,
    the number of scenes whose walk drew it; `budget` is the bytes the whole
    image set may take. Returns the images to repack and their new level.
    Greedy by seconds saved per byte added, one level at a time: every
    level-3 image is considered for level 2 before any level-2 image is
    considered for level 1, since the second step buys a tenth of the
    speed for more than twice the bytes."""
    total = sum(image["bytes"] for image in images.values())
    promoted: dict[str, int] = {}
    for level, target in ((3, 2), (2, 1)):
        candidates = []
        for name, image in images.items():
            current = promoted.get(name, image["level"])
            if current != level or not image["weight"]:
                continue
            bytes_now = estimated_bytes(image["bytes"], image["level"], current)
            cost = estimated_bytes(image["bytes"], image["level"], target) - bytes_now
            gain = image["weight"] * seconds_saved(image["length"], level, target)
            if cost > 0 and gain > 0:
                candidates.append((gain / cost, name, cost))
        for _, name, cost in sorted(candidates, key=lambda c: (-c[0], c[1])):
            if total + cost > budget:
                continue
            total += cost
            promoted[name] = target
    return promoted

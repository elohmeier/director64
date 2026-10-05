"""One image pack per port: every packed image once, and per scene the
directory of the images the scene draws, in priority order.

The console used to open one DragonFS file per image, decompress it into
the malloc arena or a reserved region, and evict by least recent use when
a scene drew more than the cache held. The pack replaces the files: at a
scene entry the console reads the scene's directory, takes one block for
as much of it as the heap allows, and decompresses each image from its
offset the first time the scene draws it, applying the sprite's ink to the
decompressed plane as before.

Layout, big-endian:

    header (32 bytes)  "D64P", u32 version, u32 index_count, u32 scene_count,
                       u32 index_offset, u32 scene_table_offset,
                       u32 entries_offset, u32 blobs_offset
    scene table        scene_count x (u32 first, u32 count), by movie id - 1
    index              index_count x entry, sorted by key: every blob once,
                       keyed by (asset, AUTHORED)
    entries            per scene, priority order, keyed by (asset, ink)
    blobs              the compressed assets, 16-byte aligned
    entry (28 bytes)   u32 key, u32 offset, u32 compressed, u32 allocation,
                       u32 length, u16 width, u16 height, u16 share,
                       u16 flags

`key` is the console's image_key(asset, ink): FNV-1a over the asset name,
then the ink, or'ed with one; the index uses the ink value AUTHORED (255),
which no sprite ink takes. `allocation` is what the SDK's decompressor
needs for the blob (its in-place margin included), `length` the
decompressed size, and the dimensions those of the stored plane. `share`
is per mille of the scene's most-drawn image's ticks: the console pins the
rows drawn at least half the time when the whole directory does not fit.
"""

from __future__ import annotations

import struct
from pathlib import Path

MAGIC = b"D64P"
VERSION = 1
ENTRY = struct.Struct(">IIIIIHHHH")
HEADER = struct.Struct(">4sIIIIIII")
SCENE = struct.Struct(">II")
BLOB_ALIGN = 16

# The ink value of the index entries: no sprite ink takes it.
AUTHORED = 255
# Entry flags: the blob's coverage already carries its mask member's cut.
MASK_BAKED = 1


def image_key(asset: str, ink: int) -> int:
    """Mirror of platforms/n64/director_main.c image_key()."""
    h = 2166136261
    for byte in asset.encode():
        h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
    h = ((h ^ ink) * 16777619) & 0xFFFFFFFF
    return h | 1


def read_varint(data: bytes, at: int) -> tuple[int, int]:
    value, shift = 0, 0
    while True:
        byte = data[at]
        at += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, at
        shift += 7


def asset_header(blob: bytes) -> dict:
    """The SDK's compressed-asset header: sizes the console needs to place
    the blob before it reads it (third_party/libdragon/src/asset_internal.h)."""
    if blob[:3] != b"DCA":
        raise ValueError("not a compressed asset")
    algo = (blob[4] >> 4) & 3
    if not 1 <= algo <= 3:
        raise ValueError("unsupported compression algorithm")
    at = 5
    cmp_size, at = read_varint(blob, at)
    orig_size, at = read_varint(blob, at)
    margin, at = read_varint(blob, at)
    header_size = at + (at & 1)
    if header_size + cmp_size != len(blob):
        raise ValueError("compressed asset size disagrees with its header")
    return {
        "algorithm": algo,
        "compressed": len(blob),
        "length": orig_size,
        "allocation": allocation(orig_size, cmp_size, margin),
    }


def allocation(length: int, compressed: int, margin: int) -> int:
    """asset_buf_size(): the buffer the in-place decompressor needs."""
    margin += 8
    size = length + margin
    offset = size - compressed
    while offset & 3:
        offset += 1
        size += 1
    if size & 15:
        size += 16 - (size & 15)
    return size


def apply_mask(raw: bytes, mask: bytes) -> bytes:
    """Director's mask ink, baked: the console's load-time pass
    (platforms/n64/director_main.c) over a stored FDI1 or FDIA image and its
    mask member's FDI1 image. Dark mask pixels keep the image and light ones
    (luminance at or above 3968 of 5-bit channels weighted 77, 151, 28) cut
    it; the mask is sampled with clamped coordinates over the overlap, as
    authored masks can differ by a pixel from their image. An FDI1 image
    gets its coverage bit set or cleared per pixel, an FDIA image only
    loses coverage where the mask is light."""
    if mask[:4] != b"FDI1" or raw[:4] not in (b"FDI1", b"FDIA"):
        raise ValueError("mask ink needs an FDI1 mask over an FDI1 or FDIA image")
    width, height, _ = image_dims(raw)
    mask_width, mask_height, _ = image_dims(mask)
    if not (width and height and mask_width and mask_height):
        raise ValueError("mask ink over an empty image")
    mask_count = mask_width * mask_height
    mask_words = struct.unpack(f">{mask_count}H", mask[32 : 32 + mask_count * 2])
    light = [
        ((w >> 11) & 31) * 77 + ((w >> 6) & 31) * 151 + ((w >> 1) & 31) * 28 >= 3968
        for w in mask_words
    ]
    rows = []
    for y in range(height):
        mask_y = min(y, mask_height - 1)
        row = light[mask_y * mask_width : (mask_y + 1) * mask_width]
        rows.append([row[min(x, mask_width - 1)] for x in range(width)])
    if raw[:4] == b"FDIA":
        alpha_offset = int.from_bytes(raw[20:24], "big")
        alpha = bytearray(raw[alpha_offset : alpha_offset + width * height])
        for y in range(height):
            for x in range(width):
                if rows[y][x]:
                    alpha[y * width + x] = 0
        return raw[:alpha_offset] + bytes(alpha) + raw[alpha_offset + width * height :]
    count = width * height
    words = list(struct.unpack(f">{count}H", raw[32 : 32 + count * 2]))
    for y in range(height):
        for x in range(width):
            i = y * width + x
            words[i] = (words[i] & 0xFFFE) if rows[y][x] else (words[i] | 1)
    return raw[:32] + struct.pack(f">{count}H", *words) + raw[32 + count * 2 :]


def image_dims(raw: bytes) -> tuple[int, int, bool]:
    """Stored width, height and the FollowAlpha flag of an FDI header."""
    if len(raw) < 32 or raw[:3] != b"FDI":
        raise ValueError("not an FDI image")
    return int.from_bytes(raw[8:10], "big"), int.from_bytes(raw[10:12], "big"), bool(raw[16] & 1)


def write_pack(
    path: Path,
    blobs: dict[str, bytes],
    index: dict[int, str],
    dims: dict[str, tuple[int, int]],
    scenes: dict[int, list[tuple[int, str, int]]],
    flags: dict[str, int] | None = None,
) -> dict:
    """Write the pack. `blobs` maps a blob name to its compressed bytes,
    `index` an authored key to its blob name, `dims` a blob name to its
    stored width and height, `scenes` a movie id to its (key, blob, share)
    rows, best first, and `flags` a blob name to its entry flags. Returns
    the per-scene sizes for the manifest."""
    flags = flags or {}
    order = sorted(blobs)
    offsets, headers = {}, {}
    scene_count = max(scenes, default=0)
    entry_total = sum(len(rows) for rows in scenes.values())
    index_offset = HEADER.size + scene_count * SCENE.size
    entries_offset = index_offset + len(index) * ENTRY.size
    blobs_offset = (entries_offset + entry_total * ENTRY.size + BLOB_ALIGN - 1) & ~(BLOB_ALIGN - 1)
    at = blobs_offset
    for name in order:
        headers[name] = asset_header(blobs[name])
        offsets[name] = at
        at += (len(blobs[name]) + BLOB_ALIGN - 1) & ~(BLOB_ALIGN - 1)

    def entry(key: int, name: str, share: int = 0) -> bytes:
        h = headers[name]
        width, height = dims[name]
        return ENTRY.pack(
            key, offsets[name], h["compressed"], h["allocation"], h["length"], width, height,
            share, flags.get(name, 0),
        )

    out = bytearray()
    out += HEADER.pack(
        MAGIC, VERSION, len(index), scene_count, index_offset, HEADER.size, entries_offset,
        blobs_offset,
    )
    first = 0
    report = {}
    for movie in range(1, scene_count + 1):
        rows = scenes.get(movie, [])
        out += SCENE.pack(first, len(rows))
        first += len(rows)
        if rows:
            report[movie] = {
                "entries": len(rows),
                "allocation": sum(headers[name]["allocation"] for _, name, _ in rows),
            }
    for key in sorted(index):
        out += entry(key, index[key])
    for movie in range(1, scene_count + 1):
        for key, name, share in scenes.get(movie, []):
            out += entry(key, name, share)
    out += bytes(blobs_offset - len(out))
    for name in order:
        out += blobs[name]
        out += bytes(-len(blobs[name]) % BLOB_ALIGN)
    path.write_bytes(out)
    return {"bytes": len(out), "blobs": len(order), "scenes": report}


def read_pack(data: bytes) -> dict:
    """Parse a pack back (tests and reports)."""
    magic, version, index_count, scene_count, index_offset, scene_table, entries_offset, blobs = (
        HEADER.unpack_from(data, 0)
    )
    if magic != MAGIC or version != VERSION:
        raise ValueError("not an image pack")

    def entries(at: int, count: int) -> list[dict]:
        rows = []
        for i in range(count):
            fields = ENTRY.unpack_from(data, at + i * ENTRY.size)
            rows.append(
                dict(
                    zip(
                        ("key", "offset", "compressed", "allocation", "length", "width",
                         "height", "share", "flags"),
                        fields,
                        strict=True,
                    )
                )
            )
        return rows

    scenes = {}
    for movie in range(scene_count):
        first, count = SCENE.unpack_from(data, scene_table + movie * SCENE.size)
        if count:
            scenes[movie + 1] = entries(entries_offset + first * ENTRY.size, count)
    return {"index": entries(index_offset, index_count), "scenes": scenes, "blobs_offset": blobs}

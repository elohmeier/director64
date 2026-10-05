import pytest

from director64 import image_pack

# A real level-2 blob from the Mucklas build: 6,700 bytes decompressed into
# a 6,720-byte buffer, as the console's NATIVE_CACHE largest_load reported
# for images of that size.
REAL_BLOB = bytes.fromhex(
    "444341352223ac340200461c444943381c1a2c1c5a1c941c2d3ee618040641021c0101"
    "38fffe46eaabe3110000"
)


def test_image_key_matches_the_console():
    # Vectors pinned against platforms/n64/director_main.c image_key() by
    # tests/native/test_image_alpha.c.
    assert image_pack.image_key("2044807783f1979d6c83bbd5.fdi", 36) == 0xEC740A63
    assert image_pack.image_key("a.fdi", 255) == 0x1B712721
    assert image_pack.image_key("a.fdi", 0) & 1


def test_asset_header_reads_the_sdk_sizes():
    header = image_pack.asset_header(REAL_BLOB)
    assert header == {"algorithm": 2, "compressed": 45, "length": 6700, "allocation": 6720}
    with pytest.raises(ValueError):
        image_pack.asset_header(b"FDI1" + REAL_BLOB[4:])


def test_allocation_mirrors_asset_buf_size():
    # margin + 8, then the compressed offset aligned to 4, then 16-byte rounding.
    assert image_pack.allocation(6700, 24, 4) == 6720
    assert image_pack.allocation(100, 100, 0) == 112
    assert image_pack.allocation(101, 100, 0) == 112


def blob(payload: bytes) -> bytes:
    def varint(n):
        out = bytearray()
        while True:
            byte = n & 0x7F
            n >>= 7
            out.append(byte | (0x80 if n else 0))
            if not n:
                return bytes(out)

    header = b"DCA5" + bytes([0x22]) + varint(len(payload)) + varint(len(payload)) + varint(0)
    if len(header) & 1:
        header += b"\0"
    return header + payload


def test_pack_round_trip_orders_scenes_and_shares_blobs(tmp_path):
    blobs = {"a.fdi": blob(b"A" * 50), "b.fdi": blob(b"B" * 70)}
    key_a, key_b = image_pack.image_key("a.fdi", 255), image_pack.image_key("b.fdi", 255)
    key_a36, key_b0 = image_pack.image_key("a.fdi", 36), image_pack.image_key("b.fdi", 0)
    index = {key_a: "a.fdi", key_b: "b.fdi"}
    dims = {"a.fdi": (5, 5), "b.fdi": (7, 5)}
    scenes = {1: [(key_b0, "b.fdi", 1000), (key_a36, "a.fdi", 400)], 3: [(key_a36, "a.fdi", 0)]}
    report = image_pack.write_pack(tmp_path / "images.pack", blobs, index, dims, scenes)
    data = (tmp_path / "images.pack").read_bytes()
    pack = image_pack.read_pack(data)
    assert [e["key"] for e in pack["index"]] == sorted(index)
    assert [e["key"] for e in pack["scenes"][1]] == [key_b0, key_a36]
    assert [e["key"] for e in pack["scenes"][3]] == [key_a36]
    assert [e["share"] for e in pack["scenes"][1]] == [1000, 400]
    assert 2 not in pack["scenes"]
    by_key = {e["key"]: e for e in pack["index"]}
    assert pack["scenes"][1][1]["offset"] == by_key[key_a]["offset"]
    assert by_key[key_a]["width"] == 5 and by_key[key_b]["width"] == 7
    for e in pack["index"]:
        assert e["offset"] % 16 == 0
        assert data[e["offset"] : e["offset"] + e["compressed"]] == blobs[index[e["key"]]]
        assert e["length"] in (50, 70)
    assert report["blobs"] == 2 and report["scenes"][1]["entries"] == 2
    expected = by_key[key_b]["allocation"] + by_key[key_a]["allocation"]
    assert report["scenes"][1]["allocation"] == expected


def fdi1(width, height, words):
    header = bytearray(32)
    header[0:4] = b"FDI1"
    header[4:8] = (32 + len(words) * 2).to_bytes(4, "big")
    header[8:10] = width.to_bytes(2, "big")
    header[10:12] = height.to_bytes(2, "big")
    return bytes(header) + b"".join(w.to_bytes(2, "big") for w in words)


def test_mask_ink_bakes_light_mask_pixels_as_cut_and_dark_as_kept():
    import struct

    # A 3x2 image, all pixels with coverage; a 2x2 mask (a pixel narrower
    # and sampled clamped): white cuts, black keeps, mid grey keeps.
    image = fdi1(3, 2, [0x0841, 0x0840, 0xFFFF, 0x1235, 0x1234, 0x0001])
    mask = fdi1(2, 2, [0xFFFF, 0x0001, 0x4210, 0xFFFF])
    baked = image_pack.apply_mask(image, mask)
    words = struct.unpack(">6H", baked[32:44])
    # row 0: mask white, black, black(clamped) -> cut, keep, keep
    assert words[0:3] == (0x0840, 0x0841, 0xFFFF)
    # row 1: mask grey, white, white(clamped) -> keep, cut, cut
    assert words[3:6] == (0x1235, 0x1234, 0x0000)
    assert baked[:32] == image[:32]


def test_mask_ink_over_an_alpha_plane_only_clears_coverage():
    header = bytearray(32)
    header[0:4] = b"FDIA"
    header[4:8] = (40 + 2).to_bytes(4, "big")
    header[8:10] = (2).to_bytes(2, "big")
    header[10:12] = (1).to_bytes(2, "big")
    header[16] = 1
    header[20:24] = (40).to_bytes(4, "big")
    image = bytes(header) + b"\x08\x40\x08\x40" + b"\0\0\0\0" + b"\xff\x80"
    mask = fdi1(2, 1, [0xFFFF, 0x0001])
    assert image_pack.apply_mask(image, mask)[40:42] == b"\x00\x80"
    with pytest.raises(ValueError):
        image_pack.apply_mask(image, image)

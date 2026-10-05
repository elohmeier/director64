import json
import subprocess

import pytest

from director64 import full_assets
from director64.project import work_dir

FONT_ASSET = "fonts/f1-12.font64"
FONT_FLAGS = ["--size", "12", "--range", "all"]


@pytest.fixture
def font_pack(tmp_path, monkeypatch):
    directory = tmp_path / f"{work_dir()}/director"
    source = directory / "fonts/font.otf"
    source.parent.mkdir(parents=True)

    def write_source(data):
        """A recovered original font is manifested by its own bytes."""
        source.write_bytes(data)
        (directory / "model.json").write_text(
            json.dumps(
                {
                    "problems": [],
                    "movies": [],
                    "fonts": [
                        {
                            "number": 1,
                            "asset": "fonts/font.otf",
                            "sha256": full_assets.digest(source),
                            "variants": [{"size": 12, "asset": FONT_ASSET}],
                        }
                    ],
                }
            )
        )

    write_source(b"test font source")
    monkeypatch.setattr(
        full_assets,
        "inspect_image",
        lambda *args: {
            "Id": "sha256:test-sdk",
            "Config": {"Labels": {"org.director64.libdragon-commit": "test-revision"}},
        },
    )
    conversions = []

    def convert(cmd, **kwargs):
        if "mkfont" not in cmd:
            return
        conversions.append(cmd)
        output = tmp_path / cmd[cmd.index("-o") + 1] / "f1-12.font64"
        output.write_bytes(b"converted font: " + source.read_bytes() + repr(cmd[10:]).encode())

    monkeypatch.setattr(full_assets.subprocess, "run", convert)
    return tmp_path, source, conversions, write_source


def manifest(root):
    return json.loads((root / f"{work_dir()}/director/packed.json").read_text())


def test_font_manifest_and_cached_output(font_pack):
    root, source, conversions, _ = font_pack
    full_assets.pack(root, "test")
    packed = root / f"{work_dir()}/filesystem/{FONT_ASSET}"
    entry = manifest(root)["files"][FONT_ASSET]
    assert entry["source_sha256"] == full_assets.digest(source)
    assert entry["sha256"] == full_assets.digest(packed)
    assert entry["bytes"] == packed.stat().st_size
    assert entry["conversion"] == {"tool": "mkfont", "args": FONT_FLAGS}
    full_assets.pack(root, "test")
    assert len(conversions) == 1
    assert manifest(root)["files"][FONT_ASSET] == entry


@pytest.mark.parametrize("change", ["source", "flags", "output", "missing"])
def test_font_rebuilds_stale_inputs(font_pack, change):
    root, source, conversions, write_source = font_pack
    full_assets.pack(root, "test")
    packed = root / f"{work_dir()}/filesystem/{FONT_ASSET}"
    if change == "source":
        write_source(b"changed font source")
    elif change == "flags":
        # A previous pack converted the same bytes with different mkfont
        # options; conversion identity, not source identity, is stale here.
        recorded = manifest(root)
        recorded["files"][FONT_ASSET]["conversion"]["args"] = ["--size", "40", "--range", "all"]
        (root / f"{work_dir()}/director/packed.json").write_text(json.dumps(recorded, indent=2))
    elif change == "output":
        packed.write_bytes(b"damaged font")
    else:
        packed.unlink()
    (packed.parent / "stale.font64").write_bytes(b"obsolete font")
    full_assets.pack(root, "test")
    assert len(conversions) == 2
    assert set(packed.parent.iterdir()) == {packed}
    entry = manifest(root)["files"][FONT_ASSET]
    assert entry["source_sha256"] == full_assets.digest(source)
    assert entry["sha256"] == full_assets.digest(packed)
    assert entry["conversion"]["args"] == FONT_FLAGS


def test_failed_font_conversion_preserves_previous_assets(font_pack, monkeypatch):
    root, _, _, write_source = font_pack
    full_assets.pack(root, "test")
    packed = root / f"{work_dir()}/filesystem/{FONT_ASSET}"
    before = packed.read_bytes(), manifest(root)
    write_source(b"updated font source")

    def fail(cmd, **kwargs):
        if "mkfont" in cmd:
            raise subprocess.CalledProcessError(1, cmd)

    monkeypatch.setattr(full_assets.subprocess, "run", fail)
    with pytest.raises(subprocess.CalledProcessError):
        full_assets.pack(root, "test")
    assert (packed.read_bytes(), manifest(root)) == before


TIERS = [
    {"min_decompressed_bytes": 262144, "level": 1},
    {"min_decompressed_bytes": 65536, "level": 2},
]


@pytest.mark.parametrize(
    "raw_bytes,level",
    [(1 << 20, 1), (262144, 1), (262143, 2), (65536, 2), (65535, 3), (0, 3)],
)
def test_image_codec_tier_selects_by_decompressed_size(raw_bytes, level):
    assert full_assets.image_level(raw_bytes, TIERS) == level


def test_image_codec_tiers_default_to_maximum_compression():
    assert full_assets.image_level(1 << 30, []) == full_assets.DEFAULT_IMAGE_LEVEL == 3


def test_image_codec_tiers_reject_unusable_policies(tmp_path, monkeypatch):
    policy = tmp_path / "host/source-policy.json"
    policy.parent.mkdir(parents=True)

    class Game:
        directory = tmp_path

    monkeypatch.setattr("director64.project.selected_game", lambda: Game())
    policy.write_text(json.dumps({"image_codec_tiers": list(reversed(TIERS))}))
    with pytest.raises(ValueError, match="descend"):
        full_assets.image_codec_tiers(tmp_path)
    policy.write_text(
        json.dumps({"image_codec_tiers": [{"min_decompressed_bytes": 1, "level": 4}]})
    )
    with pytest.raises(ValueError, match="invalid image codec tier"):
        full_assets.image_codec_tiers(tmp_path)
    policy.write_text(json.dumps({"image_codec_tiers": TIERS}))
    assert full_assets.image_codec_tiers(tmp_path) == TIERS


def fdi(pixels, magic=b"FDI1", trailer=b""):
    header = bytearray(32)
    header[0:4] = magic
    header[8:10] = len(pixels).to_bytes(2, "big")
    header[10:12] = (1).to_bytes(2, "big")
    return bytes(header) + b"".join(p.to_bytes(2, "big") for p in pixels) + trailer


def test_quantized_colors_keep_the_endpoints_and_the_alpha_bit():
    """Matte ink compares against pure white and coverage lives in bit 0, so a
    channel that no longer reaches 31, or a dropped alpha bit, would change
    what the runtime draws rather than only how it looks."""
    white, black = 0xFFFF, 0x0001
    out = full_assets.quantize_colors(fdi([white, black, 0x0000]))
    assert out[32:38].hex() == "ffff00010000"
    # Every channel keeps its own 4 bits and neighbouring pixels never bleed.
    # The dropped bit is refilled from the top one, so the 16 levels are
    # 0,2..14 below the midpoint and 17,19..31 above it.
    packed = [(15 << 11) | (16 << 6) | (3 << 1) | 1, (1 << 11) | (30 << 6) | (0 << 1)]
    out = full_assets.quantize_colors(fdi(packed))
    got = [int.from_bytes(out[32 + i * 2 : 34 + i * 2], "big") for i in range(2)]
    assert [((v >> 11) & 31, (v >> 6) & 31, (v >> 1) & 31, v & 1) for v in got] == [
        (14, 17, 2, 1),
        (0, 31, 0, 0),
    ]


def test_quantized_colors_leave_everything_but_the_colour_plane_alone():
    """FDIA carries its coverage in a separate 8-bit plane after the colours,
    and FDI2 is not a 5551 image at all."""
    trailer = bytes(range(8))
    out = full_assets.quantize_colors(fdi([0xFFFF], magic=b"FDIA", trailer=trailer))
    assert out[:32] == fdi([0xFFFF], magic=b"FDIA")[:32] and out[34:] == trailer
    untouched = fdi([0x1234], magic=b"FDI2")
    assert full_assets.quantize_colors(untouched) == untouched


def test_source_policy_knobs_reject_unusable_values(tmp_path, monkeypatch):
    policy = tmp_path / "host/source-policy.json"
    policy.parent.mkdir(parents=True)

    class Game:
        directory = tmp_path

    monkeypatch.setattr("director64.project.selected_game", lambda: Game())
    policy.write_text(json.dumps({}))
    assert (full_assets.image_color_bits(tmp_path), full_assets.wav_resample_hz(tmp_path)) == (5, 0)
    policy.write_text(json.dumps({"image_color_bits": 4, "wav_resample_hz": 16000}))
    assert (full_assets.image_color_bits(tmp_path), full_assets.wav_resample_hz(tmp_path)) == (
        4,
        16000,
    )
    policy.write_text(json.dumps({"image_color_bits": 3}))
    with pytest.raises(ValueError, match="image_color_bits"):
        full_assets.image_color_bits(tmp_path)
    policy.write_text(json.dumps({"wav_resample_hz": 4000}))
    with pytest.raises(ValueError, match="wav_resample_hz"):
        full_assets.wav_resample_hz(tmp_path)


def fdic_words(data: bytes) -> tuple[int, int, bytes, bytes]:
    """(bits, colours, palette words, index plane) of an FDIC image."""
    colors = int.from_bytes(data[20:22], "big")
    offset = (32 + colors * 2 + 7) & ~7
    return data[18], colors, data[32 : 32 + colors * 2], data[offset:]


def test_indexed_colors_use_four_bits_up_to_sixteen_colours_and_eight_beyond():
    # 3x2, three colours: a nibble per pixel, rows of two bytes, the odd row
    # end a zero nibble; the palette keeps first-seen order and the alpha bit,
    # so opaque white and transparent white are two entries.
    white, black, clear_white = 0xFFFF, 0x0001, 0xFFFE
    image = fdi([white, black, clear_white, black, white, white])
    image = image[:8] + (3).to_bytes(2, "big") + (2).to_bytes(2, "big") + image[12:]
    out = full_assets.index_colors(image)
    assert out[:4] == b"FDIC" and int.from_bytes(out[4:8], "big") == len(out) == 44
    bits, colors, palette, plane = fdic_words(out)
    assert (bits, colors) == (4, 3)
    assert palette.hex() == "ffff0001fffe" and plane.hex() == "01201000"
    # Seventeen colours: a byte per pixel.
    pixels = list(range(17))
    out = full_assets.index_colors(fdi(pixels))
    bits, colors, palette, plane = fdic_words(out)
    assert (bits, colors) == (8, 17) and plane == bytes(range(17))
    assert int.from_bytes(out[4:8], "big") == len(out) == (32 + 34 + 7) // 8 * 8 + 17
    # The header's other fields survive, and the registration point with them.
    assert out[8:18] == fdi(pixels)[8:18]


def test_indexed_colors_leave_rich_and_soft_images_alone():
    many = fdi(list(range(257)))
    assert full_assets.index_colors(many) == many
    soft = fdi([0xFFFF], magic=b"FDIA", trailer=bytes(8))
    assert full_assets.index_colors(soft) == soft


def test_indexed_colors_keep_the_tile_layout_of_a_wide_image():
    # 1056x1: 33 tiles of 32x32, zero-padded, so the stored plane is 33,792
    # pixels and CI4 rows are the tiles' 32 pixels: 16 bytes each.
    width, height = 1056, 1
    stored = full_assets.plane_pixels(width, height)
    assert stored == 33 * 1024
    header = bytearray(32)
    header[0:4] = b"FDI1"
    header[8:10] = width.to_bytes(2, "big")
    header[10:12] = height.to_bytes(2, "big")
    words = [((i % 3) << 11) | 1 for i in range(stored)]
    raw = bytes(header) + b"".join(w.to_bytes(2, "big") for w in words)
    out = full_assets.index_colors(raw)
    bits, colors, palette, plane = fdic_words(out)
    assert (bits, colors, len(plane)) == (4, 3, 33 * 512)
    # Image pixel (33, 0) is stored pixel 1025 (tile 1, row 0, column 1): the
    # low nibble of byte 512, behind stored pixel 1024 in the high nibble.
    assert plane[512] == ((words[1024] >> 11) << 4) | (words[1025] >> 11)


def test_source_policy_indexed_knob_must_be_boolean(tmp_path, monkeypatch):
    policy = tmp_path / "host/source-policy.json"
    policy.parent.mkdir(parents=True)
    monkeypatch.setattr(full_assets, "source_policy", lambda root: json.loads(policy.read_text()))
    policy.write_text(json.dumps({"image_indexed": "yes"}))
    with pytest.raises(ValueError, match="image_indexed"):
        full_assets.image_indexed(tmp_path)
    policy.write_text(json.dumps({"image_indexed": True}))
    assert full_assets.image_indexed(tmp_path) is True
    policy.write_text(json.dumps({}))
    assert full_assets.image_indexed(tmp_path) is False


def fdi1_words(width, height, words):
    header = bytearray(32)
    header[0:4] = b"FDI1"
    header[4:8] = (32 + len(words) * 2).to_bytes(4, "big")
    header[8:10] = width.to_bytes(2, "big")
    header[10:12] = height.to_bytes(2, "big")
    return bytes(header) + b"".join(w.to_bytes(2, "big") for w in words)


def fake_blob(payload: bytes) -> bytes:
    """A compressed asset as mkasset would write it, holding the input."""

    def varint(n):
        out = bytearray()
        while True:
            out.append((n & 0x7F) | (0x80 if n >> 7 else 0))
            n >>= 7
            if not n:
                return bytes(out)

    header = b"DCA5" + bytes([0x22]) + varint(len(payload)) * 2 + varint(0)
    return header + (b"\0" if len(header) & 1 else b"") + payload


@pytest.fixture
def image_pack_root(tmp_path, monkeypatch):
    """Two movies over three images, with a recorded working set: the first
    movie draws one image with matte ink and another opaque, the second
    only references the third from its score."""
    directory = tmp_path / f"{work_dir()}/director"
    (directory / "images").mkdir(parents=True)
    raws = {
        "a": fdi1_words(2, 1, [0xFFFF, 0x0840]),
        "b": fdi1_words(1, 1, [0x1234]),
        "c": fdi1_words(1, 2, [0xFFFF, 0xFFFF]),
    }
    names = {}
    for stem, raw in raws.items():
        name = full_assets.hashlib.sha256(raw).hexdigest()[:24] + ".fdi"
        (directory / "images" / name).write_bytes(raw)
        names[stem] = name
    model = {
        "problems": [],
        "fonts": [],
        "movies": [
            {
                "id": 1,
                "name": "ONE.DXR",
                "members": [
                    {"number": 1, "cast": 1, "type": 1, "asset": names["a"]},
                    {"number": 2, "cast": 1, "type": 1, "asset": names["b"]},
                ],
            },
            {
                "id": 2,
                "name": "TWO.DXR",
                "members": [{"number": 1, "cast": 1, "type": 1, "asset": names["c"]}],
            },
        ],
    }
    (directory / "model.json").write_text(json.dumps(model))
    sets = {
        "ONE.DXR": [
            {"asset": names["a"], "ink": 36, "ticks": 500, "frames": 1},
            {"asset": names["b"], "ink": 0, "ticks": 900, "frames": 0},
            {"asset": names["a"], "ink": 8, "ticks": 20, "frames": 0},
        ],
        "TWO.DXR": [{"asset": names["c"], "ink": 36, "ticks": 0, "frames": 3}],
    }
    monkeypatch.setattr(full_assets, "working_sets", lambda root: sets)
    monkeypatch.setattr(full_assets, "source_policy", lambda root: {})
    monkeypatch.setattr(
        full_assets,
        "inspect_image",
        lambda *args: {
            "Id": "sha256:test-sdk",
            "Config": {"Labels": {"org.director64.libdragon-commit": "test-revision"}},
        },
    )
    conversions = []

    def convert(cmd, **kwargs):
        if "sh" not in cmd:
            return
        shell = cmd[cmd.index("-c") + 1]
        for part in shell.split(" && "):
            words = part.split()
            if "mkasset" not in words:
                continue
            listing = tmp_path / words[words.index("-a") + 1]
            output = tmp_path / words[words.index("-o") + 1]
            level = words[words.index("-c") + 1]
            output.mkdir(parents=True, exist_ok=True)
            for source in listing.read_bytes().split(b"\0"):
                if source:
                    src = tmp_path / source.decode()
                    conversions.append(src.name)
                    (output / src.name).write_bytes(
                        fake_blob(src.read_bytes() + b"L" + level.encode())
                    )

    monkeypatch.setattr(full_assets.subprocess, "run", convert)
    return tmp_path, names, conversions


def test_images_pack_into_one_file_with_scene_directories(image_pack_root):
    from director64 import image_pack

    root, names, conversions = image_pack_root
    full_assets.pack(root, "test")
    filesystem = root / f"{work_dir()}/filesystem"
    assert (filesystem / "images.pack").is_file()
    assert not (filesystem / "images").exists()
    cache = root / f"{work_dir()}/director/packed/images"
    assert sorted(p.name for p in cache.iterdir()) == sorted(names.values())
    pack = image_pack.read_pack((filesystem / "images.pack").read_bytes())
    # Every image once, keyed by its authored name.
    assert {e["key"] for e in pack["index"]} == {
        image_pack.image_key(name, image_pack.AUTHORED) for name in names.values()
    }
    by_key = {e["key"]: e for e in pack["index"]}
    # Scene one lists what it draws, most drawn first, keyed by asset and
    # ink; two inks on one asset are two entries over the same blob.
    assert [e["key"] for e in pack["scenes"][1]] == [
        image_pack.image_key(names["b"], 0),
        image_pack.image_key(names["a"], 36),
        image_pack.image_key(names["a"], 8),
    ]
    assert pack["scenes"][1][1]["offset"] == pack["scenes"][1][2]["offset"]
    assert pack["scenes"][1][1]["offset"] == by_key[image_pack.image_key(names["a"], 255)]["offset"]
    assert [e["key"] for e in pack["scenes"][2]] == [image_pack.image_key(names["c"], 36)]
    entry = pack["scenes"][1][0]
    assert (entry["width"], entry["height"], entry["length"]) == (1, 1, 36)
    assert [e["share"] for e in pack["scenes"][1]] == [1000, 555, 22]
    assert [e["share"] for e in pack["scenes"][2]] == [0]
    report = manifest(root)["pack"]
    assert report["scenes"]["ONE.DXR"]["entries"] == 3
    assert report["scenes"]["ONE.DXR"]["allocation"] == sum(
        e["allocation"] for e in pack["scenes"][1]
    )
    assert report["unknown_assets"] == []
    # A second pack converts nothing and writes the same pack.
    before = (filesystem / "images.pack").read_bytes()
    conversions.clear()
    full_assets.pack(root, "test")
    assert conversions == []
    assert (filesystem / "images.pack").read_bytes() == before


def test_codec_budget_promotes_drawn_images_within_the_pack_budget(image_pack_root, monkeypatch):
    from director64 import image_pack

    root, names, conversions = image_pack_root
    # Every image is below the tier threshold, so the tiers pack at level 3;
    # a budget of the baseline plus a little promotes the most drawn image.
    monkeypatch.setattr(
        full_assets,
        "source_policy",
        lambda root: {"image_pack_budget_mib": 0.001},
    )
    full_assets.pack(root, "test")
    report = manifest(root)["pack"]
    assert report["budget"] == 1048
    assert report["promoted"]["images"] >= 1
    promoted = manifest(root)["promoted"]
    assert all(entry["level"] in (1, 2) for entry in promoted.values())
    # The pack carries the promoted blob, whose fake payload names its level.
    pack = image_pack.read_pack((root / f"{work_dir()}/filesystem/images.pack").read_bytes())
    data = (root / f"{work_dir()}/filesystem/images.pack").read_bytes()
    for name, entry in promoted.items():
        key = image_pack.image_key(name[len("images/") :], image_pack.AUTHORED)
        row = next(e for e in pack["index"] if e["key"] == key)
        blob = data[row["offset"] : row["offset"] + row["compressed"]]
        assert blob.endswith(b"L%d" % entry["level"])
    cache = root / f"{work_dir()}/director/packed/promoted"
    assert sorted(p.name for p in cache.iterdir()) == sorted(
        n[len("images/") :] for n in promoted
    )
    # A second pack reuses the promoted blobs and converts nothing.
    conversions.clear()
    full_assets.pack(root, "test")
    assert conversions == []


def test_codec_budget_knob_must_be_a_number(tmp_path, monkeypatch):
    monkeypatch.setattr(full_assets, "source_policy", lambda root: {"image_pack_budget_mib": "1"})
    with pytest.raises(ValueError):
        full_assets.image_pack_budget(tmp_path)
    monkeypatch.setattr(full_assets, "source_policy", lambda root: {})
    assert full_assets.image_pack_budget(tmp_path) == 0


def test_codec_budget_promotes_cached_baseline_images(image_pack_root, monkeypatch):
    root, names, conversions = image_pack_root
    # A first pack without a budget fills the baseline cache; a second one
    # with a budget must stage the cached sources again for the promotion.
    full_assets.pack(root, "test")
    monkeypatch.setattr(
        full_assets, "source_policy", lambda root: {"image_pack_budget_mib": 0.001}
    )
    conversions.clear()
    full_assets.pack(root, "test")
    promoted = manifest(root)["promoted"]
    assert promoted
    assert sorted(conversions) == sorted(n[len("images/") :] for n in promoted)


def test_mask_ink_is_baked_for_images_every_scene_draws_masked(image_pack_root, monkeypatch):
    from director64 import image_pack

    root, names, conversions = image_pack_root
    directory = root / f"{work_dir()}/director"
    model = json.loads((directory / "model.json").read_text())
    # "a" gains a mask member right after it; "c" too, but "c" is also drawn
    # with matte ink in the recorded sets, so it keeps the console's pass.
    mask = fdi1_words(2, 1, [0xFFFF, 0x0001])
    mask_name = full_assets.hashlib.sha256(mask).hexdigest()[:24] + ".fdi"
    (directory / "images" / mask_name).write_bytes(mask)
    model["movies"][0]["members"] = [
        {"number": 1, "cast": 1, "type": 1, "name": "a", "asset": names["a"]},
        {"number": 2, "cast": 1, "type": 1, "name": "a_mask", "asset": mask_name},
        {"number": 3, "cast": 1, "type": 1, "name": "b", "asset": names["b"]},
    ]
    model["movies"][1]["members"] = [
        {"number": 1, "cast": 1, "type": 1, "name": "c", "asset": names["c"]},
        {"number": 2, "cast": 1, "type": 1, "name": "c_mask", "asset": mask_name},
    ]
    (directory / "model.json").write_text(json.dumps(model))
    sets = {
        "ONE.DXR": [{"asset": names["a"], "ink": 9, "ticks": 5, "frames": 0}],
        "TWO.DXR": [
            {"asset": names["c"], "ink": 9, "ticks": 5, "frames": 0},
            {"asset": names["c"], "ink": 36, "ticks": 1, "frames": 0},
        ],
    }
    monkeypatch.setattr(full_assets, "working_sets", lambda root: sets)
    full_assets.pack(root, "test")
    report = manifest(root)["pack"]
    assert report["masks_baked"] == 1
    files = manifest(root)["files"]
    assert files[f"images/{names['a']}"]["conversion"]["mask"] == full_assets.digest(
        directory / "images" / mask_name
    )
    assert "mask" not in files[f"images/{names['c']}"]["conversion"]
    pack = image_pack.read_pack((root / f"{work_dir()}/filesystem/images.pack").read_bytes())
    by_key = {e["key"]: e for e in pack["index"]}
    assert by_key[image_pack.image_key(names["a"], 255)]["flags"] == image_pack.MASK_BAKED
    assert by_key[image_pack.image_key(names["c"], 255)]["flags"] == 0
    assert [e["flags"] for e in pack["scenes"][1]] == [image_pack.MASK_BAKED]
    # The baked blob's pixels: the white mask pixel cut the first pixel.
    cache = root / f"{work_dir()}/director/packed/images"
    baked = (cache / names["a"]).read_bytes()
    assert baked.endswith(b"\xff\xfe\x08\x41L3")

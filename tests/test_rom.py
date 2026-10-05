import io
import struct
import zipfile

import pytest

from director64.rom import (
    ROM_LIMIT,
    ROM_TARGET,
    RomError,
    dfs_files,
    validate_header,
    validate_size,
    validate_symbols,
)


def test_valid_header(rom_factory):
    validate_header(rom_factory(save_type=0), title="Findus Workshop")
    fields = validate_header(rom_factory(), title="Findus Workshop", save_type=0x50)
    assert fields["num-players"] == "1"
    with pytest.raises(RomError):
        validate_header(rom_factory(save_type=0), title="Findus Workshop", save_type=0x50)


def test_default_and_configured_rom_size_boundaries():
    validate_size(ROM_TARGET)
    with pytest.raises(RomError, match="release 56 MiB"):
        validate_size(ROM_TARGET + 1)
    validate_size(24 * 1024 * 1024, release_budget_mib=24)
    with pytest.raises(RomError, match="release 24 MiB"):
        validate_size(24 * 1024 * 1024 + 1, release_budget_mib=24)
    with pytest.raises(RomError, match="hard 64 MiB"):
        validate_size(ROM_LIMIT + 1, release_budget_mib=56)


@pytest.mark.parametrize("budget", [None, False, True, 0, -1, 57, 65, 1000, 56.0, "56"])
def test_release_budgets_cannot_be_raised_past_the_debug_window(rom_factory, budget):
    """56 MiB is where the SDK's USB debug scratch starts overwriting the
    cartridge, so a game may only ask for less than the default, never more."""
    with pytest.raises(RomError, match="integer from 1 to 56 MiB"):
        validate_header(rom_factory(), title="Findus Workshop", release_budget_mib=budget)
    with pytest.raises(RomError, match="hard 64 MiB"):
        validate_size(ROM_LIMIT + 1, release_budget_mib=budget)


def test_oversized_rom_is_rejected_before_its_header_is_trusted(rom_factory):
    data = rom_factory(title="Findus Mucklas", code_bytes=60 * 1024 * 1024 - 16384)
    assert len(data) == 60 * 1024 * 1024
    with pytest.raises(RomError, match="release 56 MiB"):
        validate_header(data, title="Findus Mucklas", save_type=0x50)
    smaller = rom_factory(title="Findus Mucklas", code_bytes=32 * 1024 * 1024 - 16384)
    assert validate_header(smaller, title="Findus Mucklas", save_type=0x50)["name"] == (
        "Findus Mucklas"
    )
    with pytest.raises(RomError, match="unexpected ROM title"):
        validate_header(smaller, title="Wrong game", save_type=0x50)
    with pytest.raises(RomError, match="differs from"):
        validate_header(
            smaller,
            title="Findus Mucklas",
            save_type=0x50,
            expected_metadata=b"stale",
        )


@pytest.mark.parametrize(
    "offset,value",
    [
        (0, 0),
        (0x20, 0),
        (0x3C, 0),
        (0x3F, 0x02),
        (0x3F, 0x50),
        (0x3F, 0x53),
        (0x3F, 0x56),
        (0x3F, 0x5A),
        (0x34, 0x80),
        (0x35, 0),
        (0x36, 0x80),
        (0x37, 0),
        (0x38, 0),
        (0x38, 3),
    ],
)
def test_metadata_mismatch(rom_factory, offset, value):
    data = bytearray(rom_factory())
    data[offset] = value
    with pytest.raises(RomError):
        validate_header(data, title="Findus Workshop", save_type=0x50)


def test_incomplete_header(rom_factory):
    with pytest.raises(RomError):
        validate_header(rom_factory()[:100], title="Findus Workshop", save_type=0x50)


def test_metadata_flag_without_zip_is_rejected(rom_factory):
    with pytest.raises(RomError, match="metadata ZIP"):
        validate_header(rom_factory()[:16384], title="Findus Workshop", save_type=0x50)


def test_corrupt_metadata_crc_is_rejected(rom_factory):
    data = bytearray(rom_factory())
    data[data.index(b"Test fixture")] ^= 1
    with pytest.raises(RomError, match="CRC"):
        validate_header(data, title="Findus Workshop", save_type=0x50)


@pytest.mark.parametrize(
    "metadata",
    [
        b"[meta]\nname=Wrong game\n",
        b"\xff",
        b"x" * 65537,
    ],
)
def test_invalid_metadata_text_is_rejected(rom_factory, metadata):
    with pytest.raises(RomError, match="metadata.ini"):
        validate_header(rom_factory(metadata=metadata), title="Findus Workshop", save_type=0x50)


@pytest.mark.parametrize(
    "old,new",
    [
        (b"name=Findus Workshop", b"name=Other game"),
        (b"num-players=1", b"num-players=4"),
        (b"short-desc=", b"Short-Desc="),
        (b"2026-09-06", b"20260906"),
        (b"v0.1.0+fixture - Requires 8 MiB RAM.", "ä".encode() * 61),
        (b"author=Test fixture", b"author=" + b"a" * 256),
    ],
)
def test_menu_field_limits_and_identity(rom_factory, old, new):
    with zipfile.ZipFile(io.BytesIO(rom_factory())) as archive:
        metadata = archive.read("metadata.ini").replace(old, new)
    with pytest.raises(RomError, match="metadata.ini"):
        validate_header(rom_factory(metadata=metadata), title="Findus Workshop", save_type=0x50)


@pytest.mark.parametrize(
    "entries",
    [
        [],
        [("nested/metadata.ini", b"x")],
        [("metadata.ini", b"x"), ("../escape", b"x")],
        [("metadata.ini", b"x"), ("big.txt", bytes(4 * 1024 * 1024))],
    ],
)
def test_archive_structure_is_checked(rom_factory, entries):
    with pytest.raises(RomError, match="metadata ZIP"):
        validate_header(rom_factory(entries=entries), title="Findus Workshop", save_type=0x50)


def test_duplicate_metadata_entries_are_rejected(rom_factory):
    with pytest.warns(UserWarning, match="Duplicate"):
        data = rom_factory(entries=[("metadata.ini", b"x"), ("metadata.ini", b"y")])
    with pytest.raises(RomError, match="unique"):
        validate_header(data, title="Findus Workshop", save_type=0x50)


def test_final_metadata_must_match_build_input(rom_factory):
    data = rom_factory()
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        metadata = archive.read("metadata.ini")
    validate_header(data, title="Findus Workshop", save_type=0x50, expected_metadata=metadata)
    with pytest.raises(RomError, match="differs"):
        validate_header(data, title="Findus Workshop", save_type=0x50, expected_metadata=b"stale")


def test_production_replay_rejected():
    validate_symbols("80001000 T engine_tick\n", "release")
    for name in ("demo_replay_init", "update_autoplay", "REPLAY_CARD_ORDER"):
        with pytest.raises(RomError):
            validate_symbols(f"80001000 T {name}\n", "release")
    validate_symbols("80001000 T demo_replay_init\n80002000 T demo_replay_sample\n", "probe")
    with pytest.raises(RomError):
        validate_symbols("80001000 T engine_tick\n", "probe")


def test_dfs_bounds_and_cycle():
    data = bytearray(80)
    data[:8] = bytes.fromhex("deadbeefffffffff")
    data[12:25] = b"DragonFS 2.1\0"
    struct.pack_into(">III", data, 26, 0, 4, 76)
    data[38:42] = b"abc\0"
    data[76:80] = b"test"
    assert dfs_files(data) == {"abc": b"test"}
    struct.pack_into(">I", data, 26, 26)
    with pytest.raises(RomError, match="cyclic"):
        dfs_files(data)
    struct.pack_into(">I", data, 26, 0)
    struct.pack_into(">I", data, 34, 0xFFFFFFFF)
    with pytest.raises(RomError, match="bounds"):
        dfs_files(data)


def test_two_controller_metadata(rom_factory):
    with zipfile.ZipFile(io.BytesIO(rom_factory())) as archive:
        metadata = archive.read("metadata.ini").replace(b"num-players=1", b"num-players=2")
    data = bytearray(rom_factory(metadata=metadata))
    data[0x34:0x38] = b"\x00\x00\xff\xff"
    assert (
        validate_header(bytes(data), title="Findus Workshop", save_type=0x50, controller_count=2)[
            "num-players"
        ]
        == "2"
    )
    with pytest.raises(RomError, match="controller metadata"):
        validate_header(bytes(data), title="Findus Workshop", save_type=0x50)
    data[0x34:0x38] = b"\x00\xff\xff\xff"
    with pytest.raises(RomError, match="controller metadata"):
        validate_header(bytes(data), title="Findus Workshop", save_type=0x50, controller_count=2)


def test_packed_dfs_names_replace_images_with_the_pack():
    from director64.rom import packed_dfs_names

    packed = {"files": {"images/a.fdi": {}, "images/b.fdi": {}, "audio/x.wav64": {}}}
    assert packed_dfs_names(packed) == {"audio/x.wav64", "images.pack"}
    assert packed_dfs_names({"files": {"fonts/f.font64": {}}}) == {"fonts/f.font64"}


def test_validate_image_pack_checks_digest_and_index(tmp_path):
    import hashlib

    import pytest

    from director64 import image_pack
    from director64.rom import RomError, validate_image_pack

    def blob(payload):
        header = b"DCA5" + bytes([0x22]) + bytes([len(payload)]) * 2 + b"\0"
        return header + (b"\0" if len(header) & 1 else b"") + payload

    blobs = {"a.fdi": blob(b"A" * 20)}
    index = {image_pack.image_key("a.fdi", image_pack.AUTHORED): "a.fdi"}
    image_pack.write_pack(tmp_path / "images.pack", blobs, index, {"a.fdi": (1, 1)}, {})
    data = (tmp_path / "images.pack").read_bytes()
    packed = {"files": {"images/a.fdi": {}}, "pack": {"sha256": hashlib.sha256(data).hexdigest()}}
    validate_image_pack({"images.pack": data}, packed)
    with pytest.raises(RomError):
        validate_image_pack({}, packed)
    more = {"files": {"images/a.fdi": {}, "images/b.fdi": {}}, "pack": packed["pack"]}
    with pytest.raises(RomError):
        validate_image_pack({"images.pack": data}, more)
    with pytest.raises(RomError):
        validate_image_pack({"images.pack": data + b"x"}, packed)

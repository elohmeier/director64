from __future__ import annotations

import io
from pathlib import Path

import pytest

from director64 import iso


def dual(value, width):
    return value.to_bytes(width, "little") + value.to_bytes(width, "big")


def record(name: bytes, sector: int, length: int, flags=0):
    data = bytearray(33 + len(name) + (len(name) % 2 == 0))
    data[0] = len(data)
    data[2:10] = dual(sector, 4)
    data[10:18] = dual(length, 4)
    data[25] = flags
    data[28:32] = dual(1, 2)
    data[32] = len(name)
    data[33 : 33 + len(name)] = name
    return data


def image(name=b"TEST.TXT;1"):
    data = bytearray(24 * iso.SECTOR)
    pvd = 16 * iso.SECTOR
    data[pvd : pvd + 7] = b"\x01CD001\x01"
    data[pvd + 80 : pvd + 88] = dual(24, 4)
    data[pvd + 128 : pvd + 132] = dual(iso.SECTOR, 2)
    root = record(b"\0", 20, iso.SECTOR, 2)
    data[pvd + 156 : pvd + 156 + len(root)] = root
    data[17 * iso.SECTOR : 17 * iso.SECTOR + 7] = b"\xffCD001\x01"
    directory = root + record(b"\x01", 20, iso.SECTOR, 2) + record(name, 21, 4)
    data[20 * iso.SECTOR : 20 * iso.SECTOR + len(directory)] = directory
    data[21 * iso.SECTOR : 21 * iso.SECTOR + 4] = b"test"
    return data


def test_iso_inventory():
    assert iso.inventory(io.BytesIO(image())) == [iso.Entry("TEST.TXT", 21 * iso.SECTOR, 4)]


@pytest.mark.parametrize("name", [b"../TEST;1", b"..;1", b"C:TEST;1", b"\\TEST;1", b"\x80;1"])
def test_unsafe_names(name):
    with pytest.raises(iso.IsoError):
        iso.inventory(io.BytesIO(image(name)))


def test_mismatched_endian_size():
    data = image()
    data[16 * iso.SECTOR + 87] ^= 1
    with pytest.raises(iso.IsoError, match="endian"):
        iso.inventory(io.BytesIO(data))


def test_directory_cycle():
    data = image()
    cyclic = record(b"LOOP", 20, iso.SECTOR, 2)
    data[20 * iso.SECTOR + 68 : 21 * iso.SECTOR] = bytes(iso.SECTOR - 68)
    data[20 * iso.SECTOR + 68 : 20 * iso.SECTOR + 68 + len(cyclic)] = cyclic
    with pytest.raises(iso.IsoError, match="cyclic"):
        iso.inventory(io.BytesIO(data))


def test_truncation():
    with pytest.raises(iso.IsoError):
        iso.inventory(io.BytesIO(image()[: 17 * iso.SECTOR + 10]))


def test_cached_file_revalidated(tmp_path: Path, monkeypatch):
    path = tmp_path / "fixture.iso"
    path.write_bytes(image())
    output = tmp_path / "extracted"
    iso.extract(path, output, expected_sha256=iso.file_sha256(path))
    iso.extract(path, output, expected_sha256=iso.file_sha256(path))
    (output / "TEST.TXT").write_bytes(b"tampered")
    with pytest.raises(iso.IsoError, match="differs"):
        iso.extract(path, output, expected_sha256=iso.file_sha256(path))
    assert (output / "TEST.TXT").read_bytes() == b"tampered"


def test_extraction_rejects_symlinks(tmp_path: Path, monkeypatch):
    path = tmp_path / "fixture.iso"
    path.write_bytes(image())
    target = tmp_path / "original"
    iso.extract(path, target, expected_sha256=iso.file_sha256(path))
    link = tmp_path / "link"
    link.symlink_to(target)
    with pytest.raises(iso.IsoError, match="symlink"):
        iso.extract(path, link, expected_sha256=iso.file_sha256(path))


def test_wrong_disc_fails_before_creating_output(tmp_path: Path):
    path = tmp_path / "fixture.iso"
    path.write_bytes(image())
    with pytest.raises(iso.IsoError, match="unsupported ISO"):
        iso.extract(path, tmp_path / "out", expected_sha256="0" * 64)
    assert not (tmp_path / "out").exists()


def test_cached_extraction_rejects_extra_files(tmp_path):
    path = tmp_path / "fixture.iso"
    path.write_bytes(image())
    output = tmp_path / "extracted"
    iso.extract(path, output, expected_sha256=iso.file_sha256(path))
    (output / "UNMANIFESTED.DXR").write_bytes(b"extra")
    with pytest.raises(iso.IsoError, match="extra files"):
        iso.extract(path, output, expected_sha256=iso.file_sha256(path))


def raw(cooked: bytes, mode=1):
    """A cooked image framed as a raw 2352-byte rip (CloneCD .img, .bin)
    with an audio track after the data."""
    data = 16 if mode == 1 else 24
    frames = []
    for n in range(0, len(cooked), iso.SECTOR):
        frame = bytearray(iso.RAW_SECTOR)
        frame[:12] = iso.SYNC
        frame[15] = mode
        frame[data : data + iso.SECTOR] = cooked[n : n + iso.SECTOR]
        frames.append(bytes(frame))
    return b"".join(frames) + b"\x55" * 3 * iso.RAW_SECTOR


@pytest.mark.parametrize(("mode", "data"), [(1, 16), (2, 24)])
def test_raw_rip_extracts_like_the_iso(tmp_path: Path, mode, data):
    path = tmp_path / "CD01.img"
    path.write_bytes(raw(image(), mode))
    layout, entries = iso.layout_inventory(io.BytesIO(path.read_bytes()))
    assert layout == iso.Layout(iso.RAW_SECTOR, data)
    assert entries == iso.inventory(io.BytesIO(image()))
    output = tmp_path / "extracted"
    iso.extract(path, output, expected_sha256=iso.file_sha256(path))
    assert (output / "TEST.TXT").read_bytes() == b"test"
    iso.extract(path, output, expected_sha256=iso.file_sha256(path))


def test_raw_rip_copies_damaged_file_sectors(tmp_path: Path):
    data = bytearray(raw(image(), 1))
    data[21 * iso.RAW_SECTOR + 15] = 0x62  # a protection decoy's damaged header
    path = tmp_path / "CD01.img"
    path.write_bytes(data)
    iso.extract(path, tmp_path / "out", expected_sha256=iso.file_sha256(path))
    assert (tmp_path / "out/TEST.TXT").read_bytes() == b"test"
    data[20 * iso.RAW_SECTOR + 15] = 0x62  # the root directory's
    path.write_bytes(data)
    with pytest.raises(iso.IsoError):
        iso.extract(path, tmp_path / "out2", expected_sha256=iso.file_sha256(path))


def test_overstated_volume_reads_past_the_image_as_zeros(tmp_path: Path):
    data = image()
    data[16 * iso.SECTOR + 80 : 16 * iso.SECTOR + 88] = dual(40, 4)
    start = 20 * iso.SECTOR + 68
    decoy = record(b"TEST.TXT;1", 23, 2 * iso.SECTOR)  # runs a sector past the end
    data[start : start + len(decoy)] = decoy
    data[23 * iso.SECTOR - 1 :] = b""
    data += b"x" * (iso.SECTOR + 1)
    path = tmp_path / "overstated.iso"
    path.write_bytes(data)
    iso.extract(path, tmp_path / "out", expected_sha256=iso.file_sha256(path))
    assert (tmp_path / "out/TEST.TXT").read_bytes() == b"x" * iso.SECTOR + bytes(iso.SECTOR)

import hashlib
import json
import struct
import zlib
from types import SimpleNamespace

import pytest
from director64_findus_workshop.full_validation import inspect_archive, validate_capture

from director64.captures import CaptureError


def block(generation=1, files=None, version=1, legacy=0):
    files = files or [b"0\r0\r[]\r[]\r"] * 7 + [b"", b"music\r"]
    data = bytearray(65536)
    struct.pack_into(
        ">4s7I", data, 0, b"F64D", version, generation, sum(map(len, files)), 0, 0x434F4D54, 9, 0
    )
    struct.pack_into(">9I", data, 32, *map(len, files))
    struct.pack_into(">I", data, 68, legacy)
    position = 128
    for file in files:
        data[position : position + len(file)] = file
        position += len(file)
    struct.pack_into(">I", data, 16, zlib.crc32(data))
    return data


def test_full_archive_independent_reader():
    current = block(2)
    result = inspect_archive(block() + current)
    assert result["slot"] == 1 and result["generation"] == 2
    assert result["files"][8] == {"bytes": 6, "sha256": hashlib.sha256(b"music\r").hexdigest()}
    current[130] ^= 1
    assert inspect_archive(block() + current)["generation"] == 1
    with pytest.raises(CaptureError):
        inspect_archive(current + current)
    with pytest.raises(CaptureError):
        inspect_archive(bytes(65536))


def test_full_archive_canonical_ranges():
    for offset, value in ((28, 1), (68, 1), (65535, 1), (128, 0)):
        data = block()
        data[offset] = value
        struct.pack_into(">I", data, 16, 0)
        struct.pack_into(">I", data, 16, zlib.crc32(data))
        with pytest.raises(CaptureError):
            inspect_archive(data + bytes(65536))


def test_archive_versions_and_reserved_header_word():
    old = block()
    # V2 reserves a 0/1 word at 68 that a removed setting once wrote.
    current = block(2, version=2, legacy=1)
    result = inspect_archive(old + current)
    assert result["version"] == 2 and result["generation"] == 2
    assert result["files"] == inspect_archive(old + bytes(65536))["files"]
    for invalid in (
        block(3, version=3),
        block(3, version=2, legacy=2),
        block(3, version=1, legacy=1),
    ):
        assert inspect_archive(current + invalid)["generation"] == 2
    current[72] = 1
    struct.pack_into(">I", current, 16, 0)
    struct.pack_into(">I", current, 16, zlib.crc32(current))
    assert inspect_archive(old + current)["generation"] == 1


def test_capture_accepts_default_without_writing_a_save(tmp_path, monkeypatch):
    (tmp_path / "inputs").mkdir()
    (tmp_path / "inputs/findus-workshop-probe.z64").write_bytes(b"test")
    replay = {
        "target": "default",
        "expected_movie": "GINTRO.DXR",
        "expected_frame": 1,
        "ticks": 60,
        "expected_feathers": 0,
    }
    (tmp_path / "inputs/replay.json").write_text(json.dumps(replay))
    checkpoint = "FULL_REPLAY_COMPLETE id=default movie=GINTRO.DXR frame=1 ticks=60 feathers=0\n"
    log = tmp_path / "gopher64.log"
    log.write_text(checkpoint)
    monkeypatch.setattr(
        "director64_findus_workshop.full_validation.validate_media",
        lambda *args, **kwargs: ({}, {"streams": [{"codec_type": "audio"}]}),
    )
    monkeypatch.setattr(
        "director64_findus_workshop.full_validation.subprocess.run",
        lambda *args, **kwargs: SimpleNamespace(stderr="max_volume: -3.0 dB"),
    )
    assert validate_capture(tmp_path)["journal"] is None


def test_sdk_assertion_cannot_be_passing_capture(tmp_path):
    (tmp_path / "inputs").mkdir()
    (tmp_path / "inputs/replay.json").write_text("{}")
    (tmp_path / "gopher64.log").write_text("ASSERTION FAILED: invalid loop endpoint\n")
    with pytest.raises(CaptureError, match="guest failed"):
        validate_capture(tmp_path)


@pytest.mark.parametrize(
    "expected,peak,error",
    [
        ("audible", "-3.0", None),
        ("audible", "-91.0", "is silent"),
        ("audible", "-inf", "is silent"),
        ("silent", "-91.0", None),
        ("silent", "-inf", None),
        ("silent", "-3.0", "unexpected audio"),
        ("silent", None, "could not be measured"),
        ("ignored", "-3.0", "invalid replay audio expectation"),
    ],
)
def test_capture_checks_both_audible_and_silent_journeys(
    tmp_path,
    monkeypatch,
    expected,
    peak,
    error,
):
    (tmp_path / "inputs").mkdir()
    (tmp_path / "inputs/findus-workshop-probe.z64").write_bytes(b"test")
    replay = {
        "target": "audio",
        "expected_movie": "HALLTYST.DXR",
        "expected_frame": 226,
        "ticks": 60,
        "expected_feathers": 0,
        "expected_audio": expected,
    }
    (tmp_path / "inputs/replay.json").write_text(json.dumps(replay))
    (tmp_path / "gopher64.log").write_text(
        "FULL_REPLAY_COMPLETE id=audio movie=HALLTYST.DXR frame=226 ticks=60 feathers=0\n"
    )
    monkeypatch.setattr(
        "director64_findus_workshop.full_validation.validate_media",
        lambda *args, **kwargs: ({}, {"streams": [{"codec_type": "audio"}]}),
    )
    monkeypatch.setattr(
        "director64_findus_workshop.full_validation.subprocess.run",
        lambda *args, **kwargs: SimpleNamespace(
            stderr=f"max_volume: {peak} dB" if peak is not None else ""
        ),
    )
    if error:
        with pytest.raises(CaptureError, match=error):
            validate_capture(tmp_path)
    else:
        assert validate_capture(tmp_path)["expected_audio"] == expected

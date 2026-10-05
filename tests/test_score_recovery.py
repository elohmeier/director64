"""Source-free fixtures for the native ProjectorRays structural score spike."""

from __future__ import annotations

import json
import os
import struct
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="session")
def score_probe(tmp_path_factory):
    executable = tmp_path_factory.mktemp("score-probe") / "score-probe"
    subprocess.run(
        [
            os.environ.get("CXX", "c++"),
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pedantic",
            str(ROOT / "tools/projectorrays/score_recovery.cpp"),
            str(ROOT / "tools/projectorrays/probe.cpp"),
            "-o",
            str(executable),
        ],
        check=True,
        capture_output=True,
    )
    return executable


def recover(score_probe, tmp_path, kind, data, *, success=True, version=600):
    chunk = tmp_path / "synthetic.bin"
    chunk.write_bytes(data)
    run = subprocess.run(
        [str(score_probe), kind, str(version), str(chunk)], capture_output=True, text=True
    )
    if not success:
        assert run.returncode != 0
        assert not run.stdout
        assert "score-probe:" in run.stderr
        return None
    assert run.returncode == 0, run.stderr
    result = json.loads(run.stdout)
    assert bytes.fromhex(result["raw_hex"]) == data
    assert result["runtime_ir_frozen"] is False
    assert result["semantic_complete"] is False
    assert result["reference_verified"] is False
    return result


def delta(offset, payload):
    return struct.pack(">HH", len(payload), offset) + payload


def frame(*deltas):
    body = b"".join(deltas)
    return struct.pack(">H", len(body) + 2) + body


def score(*frames, declared=None, tail=b"", channels=7, version=600):
    stream = b"".join(frames)
    header = struct.pack(
        ">IIIHHHH",
        20 + len(stream),
        20,
        len(frames) if declared is None else declared,
        11 if version == 600 else 13,
        24 if version == 600 else 48,
        channels,
        0,
    )
    data = header + stream + struct.pack(">I", 0)
    envelope = struct.pack(">IiI", 40 + len(data) + len(tail), -3, 12)
    index = struct.pack(
        ">IIIIIII", 3, 4, len(data) + len(tail), 0, 20 + len(stream), len(data), len(data)
    )
    return envelope + index + data + tail


def test_partial_channel_changes_preserve_known_bytes(score_probe, tmp_path):
    sprite = bytearray(24)
    sprite[0:2] = b"\x01\xa5"
    struct.pack_into(">hHIhhhh", sprite, 4, 1, 42, 3, -2, 250, 100, 200)
    data = score(frame(delta(144, sprite)), frame(delta(159, b"\x07")))
    result = recover(score_probe, tmp_path, "VWSC", data)
    assert result["fields"]["computed_frame_count"] == 2
    first, second = [f["changed_channels"][0] for f in result["frames"]]
    assert first["fields"]["cast_member"] == 42
    assert first["fields"]["loc_v"] == -2
    assert first["fields"]["loc_h"] == 250
    assert second["fields"]["loc_h"] == 7
    assert second["fields"]["cast_member"] == 42
    assert bytes.fromhex(second["known_mask_hex"]) == b"\x01" * 24
    for f in result["frames"]:
        source = f["source"]
        assert (
            bytes.fromhex(source["hex"])
            == data[source["offset"] : source["offset"] + source["length"]]
        )


def test_cross_channel_delta_does_not_invent_defaults(score_probe, tmp_path):
    result = recover(score_probe, tmp_path, "VWSC", score(frame(delta(143, b"\xaa\x01\x08"))))
    palette, sprite = result["frames"][0]["changed_channels"]
    assert [palette["index"], sprite["index"]] == [5, 6]
    assert sprite["fields"] == {"sprite_type": 1, "ink_flags": 8}
    assert bytes.fromhex(sprite["known_mask_hex"]) == b"\x01\x01" + b"\0" * 22


@pytest.mark.parametrize("version", [700, 800, 1000])
def test_d7_d8_d10_extended_channels_partial_rgb_and_signed_rotation(
    score_probe, tmp_path, version
):
    sprite = bytearray(48)
    sprite[0] = 1
    struct.pack_into(">hHIhhhh", sprite, 4, 3, 77, 2, -12, 300, 60, 90)
    sprite[24:28] = bytes([10, 20, 30, 40])
    struct.pack_into(">ii", sprite, 28, -9000, 1500)
    offset = 999 * 48
    data = score(
        frame(delta(offset, sprite)),
        frame(delta(offset + 26, b"\x63")),
        channels=1006,
        version=version,
    )
    result = recover(score_probe, tmp_path, "VWSC", data, version=version)
    first, second = [f["changed_channels"][0] for f in result["frames"]]
    assert first["index"] == 999
    assert first["fields"]["cast_member"] == 77
    assert first["fields"]["loc_v"] == -12
    assert first["fields"]["rotation_raw"] == -9000
    assert second["fields"]["foreground_blue_raw"] == 99
    assert second["fields"]["rotation_raw"] == -9000
    assert bytes.fromhex(second["known_mask_hex"]) == b"\x01" * 48
    recover(score_probe, tmp_path, "VWSC", data, version=600, success=False)


@pytest.mark.parametrize("version", [700, 800, 1000])
def test_d7_d8_d10_cross_channel_and_all_truncations(score_probe, tmp_path, version):
    data = score(frame(delta(287, b"\xaa\x01\x08")), channels=1006, version=version)
    result = recover(score_probe, tmp_path, "VWSC", data, version=version)
    palette, sprite = result["frames"][0]["changed_channels"]
    assert [palette["index"], sprite["index"]] == [5, 6]
    assert bytes.fromhex(sprite["known_mask_hex"]) == b"\x01\x01" + b"\0" * 46
    for end in range(len(data)):
        recover(score_probe, tmp_path, "VWSC", data[:end], version=version, success=False)


@pytest.mark.parametrize("version", [700, 800, 1000])
def test_d7_d8_d10_rejects_d6_layout_and_out_of_range_delta(score_probe, tmp_path, version):
    recover(score_probe, tmp_path, "VWSC", score(frame()), version=version, success=False)
    data = score(frame(delta(7 * 48, b"\x01")), version=version)
    recover(score_probe, tmp_path, "VWSC", data, version=version, success=False)


def test_score_counts_and_unindexed_tail_remain_unresolved(score_probe, tmp_path):
    data = score(frame(), frame(), declared=0, tail=b"unclassified")
    result = recover(score_probe, tmp_path, "VWSC", data)
    assert result["fields"]["computed_frame_count"] == 2
    assert result["fields"]["declared_frame_count"] == 0
    assert result["fields"]["unindexed_tail_length"] == len(b"unclassified")
    assert "declared-frame-count-disagrees" in result["unresolved"]
    assert "unindexed-score-tail" in result["unresolved"]


@pytest.mark.parametrize(
    "mutate",
    [
        lambda b: struct.pack_into(">I", b, 0, len(b) + 1),
        lambda b: struct.pack_into(">i", b, 4, -2),
        lambda b: struct.pack_into(">I", b, 8, 0xFFFFFFFF),
        lambda b: struct.pack_into(">I", b, 12, 0xFFFFFFFF),
        lambda b: struct.pack_into(">I", b, 16, 1),
        lambda b: struct.pack_into(">I", b, 20, 0xFFFFFFFF),
        lambda b: struct.pack_into(">I", b, 24, 100),
        lambda b: struct.pack_into(">I", b, 28, 0xFFFFFFFF),
        lambda b: struct.pack_into(">I", b, 44, 19),
        lambda b: struct.pack_into(">H", b, 52, 12),
        lambda b: struct.pack_into(">H", b, 54, 48),
        lambda b: struct.pack_into(">H", b, 56, 65535),
        lambda b: struct.pack_into(">H", b, 60, 0),
        lambda b: struct.pack_into(">H", b, 60, 65535),
        lambda b: struct.pack_into(">H", b, 62, 65535),
        lambda b: struct.pack_into(">H", b, 64, 65535),
    ],
)
def test_score_rejects_malformed_ranges(score_probe, tmp_path, mutate):
    data = bytearray(score(frame(delta(144, b"\x01"))))
    mutate(data)
    recover(score_probe, tmp_path, "VWSC", data, success=False)


def test_every_score_truncation_fails(score_probe, tmp_path):
    data = score(frame(delta(144, b"\x01")))
    for length in range(len(data)):
        recover(score_probe, tmp_path, "VWSC", data[:length], success=False)


def test_labels_preserve_encoding_comments_and_sentinel(score_probe, tmp_path):
    first, second = b"Start\rcomment\r", b"\x94End"
    data = struct.pack(">HHHHHHH", 2, 1, 0, 12, len(first), 0, len(first + second)) + first + second
    result = recover(score_probe, tmp_path, "VWLB", data)
    assert [label["frame"] for label in result["labels"]] == [1, 12]
    assert bytes.fromhex(result["labels"][0]["text"]["hex"]) == b"Start"
    assert bytes.fromhex(result["labels"][0]["comment"]["hex"]) == b"comment\r"
    assert bytes.fromhex(result["labels"][1]["text"]["hex"]) == second
    assert "source-text-encoding" in result["unresolved"]
    for length in range(len(data)):
        recover(score_probe, tmp_path, "VWLB", data[:length], success=False)
    invalid = bytearray(data)
    struct.pack_into(">H", invalid, 8, 65535)
    recover(score_probe, tmp_path, "VWLB", invalid, success=False)


def test_file_info_index_preserves_unknown_entries(score_probe, tmp_path):
    values = [b"", b"\x03Ada", b"\x03Ben", b"\x02C:", b"\0\x02", b"unknown"]
    offsets = [0]
    for value in values:
        offsets.append(offsets[-1] + len(value))
    data = struct.pack(">IIIII", 32, 11, 22, 0x80000, 7) + b"\0" * 12
    data += (
        struct.pack(">H", len(values))
        + struct.pack(">" + "I" * len(offsets), *offsets)
        + b"".join(values)
    )
    result = recover(score_probe, tmp_path, "VWFI", data)
    assert result["fields"]["preload_raw"] == 2
    assert result["fields"]["script_id"] == 7
    assert [bytes.fromhex(r["hex"]) for r in result["records"]] == values
    invalid = bytearray(data)
    struct.pack_into(">I", invalid, 38, 65535)
    recover(score_probe, tmp_path, "VWFI", invalid, success=False)


def test_unknown_version_type_and_empty_input_fail(score_probe, tmp_path):
    data = score(frame())
    recover(score_probe, tmp_path, "VWSC", data, success=False, version=700)
    recover(score_probe, tmp_path, "VWSC", data, success=False, version=900)
    recover(score_probe, tmp_path, "VWSC", data, success=False, version=1100)
    recover(score_probe, tmp_path, "NOPE", data, success=False)
    recover(score_probe, tmp_path, "VWFI", b"", success=False)


def test_identical_conversion_is_byte_deterministic(score_probe, tmp_path):
    data = score(frame(delta(144, b"\x01\x02")), frame())
    first = recover(score_probe, tmp_path, "VWSC", data)
    second = recover(score_probe, tmp_path, "VWSC", data)
    assert first == second


def d5_score(*frames, tail=b""):
    stream = b"".join(frames)
    return struct.pack(">IIIHHHH", 20 + len(stream), 20, 0, 7, 24, 50, 256) + stream + tail


def test_d5_main_channels_and_direct_sprite_scripts(score_probe, tmp_path):
    main = bytearray(48)
    struct.pack_into(">HHHHHHHH", main, 0, 1, 7, 2, 42, 1, 9, 2, 61)
    main[21] = 137  # D5 wait for video sprite 2, not a D6 FPS opcode.
    struct.pack_into(">HH", main, 24, 2, 1)
    sprite = bytearray(24)
    struct.pack_into(">BBhHhHBBhhhh", sprite, 0, 16, 36, 2, 3, 1, 19, 255, 0, -2, 320, 240, 320)
    data = d5_score(frame(delta(0, main), delta(48, sprite)), frame(delta(63, b"\x42")))
    result = recover(score_probe, tmp_path, "VWSC", data, version=500)
    fields = result["fields"]
    assert (fields["frames_version"], fields["channel_count"], fields["computed_frame_count"]) == (
        7,
        50,
        2,
    )
    first = result["frames"][0]["changed_channels"]
    assert first[0]["fields"]["tempo_raw"] == 137
    assert first[0]["fields"]["sound1_member"] == 42
    assert first[1]["fields"]["cast_member"] == 1
    assert first[2]["fields"]["script_member"] == 19
    assert first[2]["fields"]["loc_v"] == -2
    assert result["frames"][1]["changed_channels"][0]["fields"]["loc_h"] == 322


def test_d5_preserves_film_tail_and_rejects_profile_substitution(score_probe, tmp_path):
    data = d5_score(frame(delta(49 * 24, b"\x10\x00")), tail=b"authored film tail")
    result = recover(score_probe, tmp_path, "VWSC", data, version=500)
    assert result["fields"]["unindexed_tail_length"] == 18
    assert result["frames"][0]["changed_channels"][0]["index"] == 49
    recover(score_probe, tmp_path, "VWSC", data, version=600, success=False)
    recover(score_probe, tmp_path, "VWSC", score(frame()), version=500, success=False)
    recover(
        score_probe,
        tmp_path,
        "VWSC",
        d5_score(frame(delta(50 * 24, b"x"))),
        version=500,
        success=False,
    )


def test_d5_rejects_every_truncation(score_probe, tmp_path):
    data = d5_score(frame(delta(48, bytes(range(24)))), frame(delta(62, b"xy")))
    for end in range(len(data)):
        recover(score_probe, tmp_path, "VWSC", data[:end], version=500, success=False)

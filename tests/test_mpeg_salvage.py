"""Synthetic MPEG syntax and independent decoder checks; no recovered media."""

import hashlib
import json
import subprocess

import pytest

from director64.mpeg_salvage import _audio, _mux, _probe_video, _stamp, reconstruct

PREFIX = b"\0\0\1"
FIRST = 19800


def bits(value):
    value += "0" * (-len(value) % 8)
    return int(value, 2).to_bytes(len(value) // 8, "big")


def picture(kind, temporal, *, bright=False):
    header = f"{temporal:010b}{kind:03b}" + "1" * 16
    if kind in (2, 3):
        header += "0001"
    if kind == 3:
        header += "0001"
    # One macroblock: qscale8, increment1. Intra DC gives gray128/144;
    # original B pictures use their future reference (unlike the hold output).
    body = "0100001"
    if kind == 1:
        body += "1" + ("111010000" if bright else "100") + "10"
        body += "10010" * 3 + "0010" * 2
    else:
        body += "00111" if kind == 2 else "01011"
    return PREFIX + b"\0" + bits(header + "0") + PREFIX + b"\x01" + bits(body)


def fixture(tmp_path):
    sequence = bits(f"{16:012b}{16:012b}" + "00010011" + f"{1:018b}" + "1" + "0" * 13)
    events = []
    for index in (0, 3, 1, 2, 6, 4, 5, 9, 7, 8):
        kind = 1 if index in (0, 6) else 2 if index % 3 == 0 else 3
        data = picture(kind, index, bright=index == 6)
        if index == 0:
            data = PREFIX + b"\xb3" + sequence + data
        if index == 8:
            data += PREFIX + b"\xb7"
        pts = FIRST + index * 3600
        dts = pts if kind == 3 else pts - (3600 if index == 0 else 10800)
        events.append((dts, pts, 0xE0, data))
    for index in range(17):
        pts = FIRST + index * 2160
        # Distinct ancillary bytes distinguish preserved compressed frames,
        # while zero allocations still decode to exact digital silence.
        data = bytes.fromhex("fffdb400") + bytes(667) + bytes([index])
        events.append((pts, pts, 0xC0, data))
    original = bytearray(_mux(events))
    offset = original.index(PREFIX + b"\xc0", original.index(PREFIX + b"\xc0") + 1) + 11
    original[offset + 100 : offset + 104] = b"lost"
    source = tmp_path / "source.mpg"
    source.write_bytes(original)
    recipe = {
        "version": 1,
        "source_sha256": hashlib.sha256(original).hexdigest(),
        "width": 16,
        "height": 16,
        "fps": 25,
        "reference_stride": 3,
        "first_pts": FIRST,
        "video_frames": 10,
        "audio_frames": 17,
        "audio_header": "fffdb400",
        "audio_frame_bytes": 672,
        "source_damage": [[offset + 100, offset + 104]],
        "video_replacements": [[1, 6]],
        "audio_replacements": [[1, 2]],
        "max_output_bytes": 20000,
    }
    return source, recipe


def decode(path, audio=False):
    command = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-xerror",
        "-err_detect",
        "explode",
        "-i",
        path,
    ]
    if audio:
        command += ["-map", "0:a:0", "-f", "s16le", "-acodec", "pcm_s16le", "-"]
    else:
        command += ["-map", "0:v:0", "-vsync", "0", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-"]
    result = subprocess.run(command, capture_output=True, check=True)
    assert not result.stderr
    return result.stdout


def test_reconstruction_preserves_timeline_intact_pictures_and_audio(tmp_path):
    source, recipe = fixture(tmp_path)
    before = source.read_bytes()
    output, report = tmp_path / "fixed.mpg", tmp_path / "receipt.json"
    receipt = reconstruct(source, output, recipe, report)
    assert source.read_bytes() == before
    assert receipt == json.loads(report.read_text())
    assert receipt["restoration_claim"] is False
    assert (receipt["video_frames"], receipt["audio_frames"]) == (10, 17)
    assert receipt["video_seconds"] == 0.4
    assert receipt["audio_samples"] == 17 * 1152
    old = _probe_video(source, FIRST, 3)
    new = _probe_video(output, FIRST, 3)
    assert set(old) == set(new) == set(range(10))
    assert all(old[i] == new[i] for i in (0, 6, 7, 8, 9))
    a, _ = _audio(before, recipe)
    b, _ = _audio(output.read_bytes(), {**recipe, "source_damage": []})
    assert set(a) == set(range(17)) - {1}
    assert set(b) == set(range(17))
    assert all(a[i] == b[i] for i in a)
    pixels = decode(output)
    assert len(pixels) == 10 * 384
    frames = [pixels[i : i + 384] for i in range(0, len(pixels), 384)]
    assert all(frame == frames[0] for frame in frames[:6])
    assert all(frame == frames[6] for frame in frames[6:])
    assert frames[0] != frames[6]
    pcm = decode(output, audio=True)
    assert len(pcm) == 17 * 1152 * 2 * 2 and not any(pcm)


def test_split_audio_uses_pes_timestamp_not_demux_parser_guesses():
    frames = [bytes.fromhex("fffdb400") + bytes(667) + bytes([i]) for i in range(4)]
    payloads = [
        (0, frames[0][:400]),
        (1, frames[0][400:] + frames[1] + frames[2][:120]),
        (3, frames[2][120:] + frames[3]),
    ]
    source = bytearray()
    for index, data in payloads:
        # Include legal stuffing and an STD buffer field before the first PTS.
        header = b"\xff\x40\x20" + _stamp(FIRST + index * 2160, 2)
        source += PREFIX + b"\xc0" + (len(header) + len(data)).to_bytes(2, "big") + header + data
    recipe = {
        "source_damage": [],
        "first_pts": FIRST,
        "audio_header": "fffdb400",
        "audio_frame_bytes": 672,
    }
    recovered, spans = _audio(bytes(source), recipe)
    assert recovered == dict(enumerate(frames))
    assert len(spans[0]) == len(spans[2]) == 2
    for index, pieces in spans.items():
        assert b"".join(source[start : start + count] for start, count in pieces) == frames[index]
    # One overwritten fragment invalidates exactly its owning MP2 frame.
    start, _ = spans[2][1]
    damaged, _ = _audio(bytes(source), {**recipe, "source_damage": [[start, start + 1]]})
    assert set(damaged) == {0, 1, 3}


@pytest.mark.parametrize(
    ("change", "message"),
    [
        ({"source_sha256": "0" * 64}, "does not match"),
        ({"video_replacements": [[3, 4]]}, "retained B picture depends"),
        ({"video_replacements": [[1, 4]]}, "retained B picture depends"),
        ({"audio_replacements": []}, "exactly match"),
        ({"audio_replacements": [[1, 3]]}, "exactly match"),
        ({"max_output_bytes": 10}, "exceeds available"),
        ({"fps": 30}, "unqualified"),
        ({"video_frames": 11}, "end cadence"),
        ({"video_replacements": [[0, 6]]}, "initial reference"),
    ],
)
def test_unapproved_or_incomplete_reconstruction_fails_before_output(tmp_path, change, message):
    source, recipe = fixture(tmp_path)
    output, report = tmp_path / "fixed.mpg", tmp_path / "receipt.json"
    with pytest.raises(ValueError, match=message):
        reconstruct(source, output, {**recipe, **change}, report)
    assert not output.exists() and not report.exists()
    assert hashlib.sha256(source.read_bytes()).hexdigest() == recipe["source_sha256"]


def test_output_cannot_overwrite_source_even_through_hard_link(tmp_path):
    source, recipe = fixture(tmp_path)
    with pytest.raises(ValueError, match="must not overwrite"):
        reconstruct(source, source, recipe, tmp_path / "receipt.json")
    output = tmp_path / "linked-output.mpg"
    output.hardlink_to(source)
    reconstruct(source, output, recipe, tmp_path / "receipt.json")
    assert hashlib.sha256(source.read_bytes()).hexdigest() == recipe["source_sha256"]
    assert output.read_bytes() != source.read_bytes()

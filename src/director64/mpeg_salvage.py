"""Explicit, hash-pinned MPEG-1 timeline reconstruction.

Only an audited recipe may select missing/damaged pictures and audio frames.
Unselected pictures and MP2 frames retain their original compressed bytes.
Replacement pictures predict the preceding reference with zero motion/residual;
replacement MP2 frames allocate no subbands (silence). This is concealment, not
recovery of the missing original content. Program-stream packetization changes.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path

_PREFIX = b"\0\0\1"


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _bits(value: str) -> bytes:
    value += "0" * (-len(value) % 8)
    return int(value, 2).to_bytes(len(value) // 8, "big")


def _ranges(indices) -> list[list[int]]:
    result = []
    for index in sorted(indices):
        if result and result[-1][1] == index:
            result[-1][1] += 1
        else:
            result.append([index, index + 1])
    return result


def _selected(ranges: list, count: int) -> set[int]:
    result: set[int] = set()
    previous = 0
    for start, end in ranges:
        _require(
            isinstance(start, int) and isinstance(end, int) and previous <= start < end <= count,
            "invalid reconstruction intervals",
        )
        result.update(range(start, end))
        previous = end
    return result


def _timestamp(data: bytes) -> int:
    _require(len(data) == 5 and all(data[i] & 1 for i in (0, 2, 4)), "invalid MPEG timestamp")
    return (
        (data[0] >> 1 & 7) << 30
        | data[1] << 22
        | (data[2] >> 1) << 15
        | data[3] << 7
        | data[4] >> 1
    )


def _stamp(value: int, marker: int) -> bytes:
    _require(0 <= value < 1 << 33, "MPEG timestamp overflow")
    return _bits(f"{marker:04b}{value >> 30:03b}1{value >> 15 & 32767:015b}1{value & 32767:015b}1")


def _picture(data: bytes) -> tuple[int, int]:
    at = data.find(_PREFIX + b"\0")
    _require(at >= 0 and at + 8 <= len(data), "missing MPEG picture header")
    value = int.from_bytes(data[at + 4 : at + 8], "big")
    return value >> 22, value >> 19 & 7


def _hold(width: int, height: int, temporal: int, kind: int) -> bytes:
    _require(kind in (2, 3) and 0 <= temporal < 1024, "invalid hold picture")
    header = f"{temporal:010b}{kind:03b}{65535:016b}0001"
    if kind == 3:
        header += "0001"
    header += "0"  # extra_bit_picture
    # increment1, forward-only prediction, no residual, zero motion in both axes.
    macroblock = "100111" if kind == 2 else "1001011"
    row = _bits("010000" + macroblock * (width // 16))
    return (
        _PREFIX
        + b"\0"
        + _bits(header)
        + b"".join(_PREFIX + bytes([y]) + row for y in range(1, height // 16 + 1))
    )


def _probe_video(source: Path, first_pts: int, stride: int) -> dict[int, bytes]:
    command = [
        "ffprobe",
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_packets",
        "-show_data",
        "-show_entries",
        "packet=pts,dts,size,data",
        "-of",
        "json",
        source,
    ]
    checked = subprocess.run(command, capture_output=True, check=True)
    packets = json.loads(checked.stdout)["packets"]
    result = {}
    for packet in packets:
        data = b"".join(
            bytes.fromhex(line.split(":", 1)[1].split("  ")[0].strip())
            for line in packet["data"].splitlines()
            if ":" in line
        )
        _require(len(data) == int(packet["size"]), "ffprobe packet data/size differs")
        _, kind = _picture(data)
        _require(kind in (1, 2, 3), "unsupported recovered MPEG picture type")
        dts = packet["dts"]
        # Some original PES packets omit PTS. This recipe supports a fixed I/P
        # reference cadence; the decoder reordering delay is proven by every
        # intact packet with both timestamps, including the source-free tests.
        pts = packet.get("pts", dts if kind == 3 else dts + stride * 3600)
        _require((pts - first_pts) % 3600 == 0, "recovered picture leaves 25-Hz PTS grid")
        index = (pts - first_pts) // 3600
        expected_dts = pts if kind == 3 else pts - (3600 if index == 0 else stride * 3600)
        _require(dts == expected_dts, "recovered picture has unexpected reorder delay")
        _require(index not in result and index >= 0, "duplicate/negative recovered video PTS")
        result[index] = data
    return result


def _audio(source: bytes, recipe: dict) -> tuple[dict[int, bytes], dict[int, list]]:
    """Recover complete MP2 frames from intact PES payloads and their own PTS.

    ffprobe can combine post-gap junk with a valid frame and label later frames
    one slot early. Locate frame starts using each source PES's timestamp;
    join a split frame only when the next timestamp and exact byte count agree.
    """
    damage = recipe["source_damage"]
    header = bytes.fromhex(recipe["audio_header"])
    size, first_pts, step = recipe["audio_frame_bytes"], recipe["first_pts"], 2160

    def clean(start: int, end: int) -> bool:
        return not any(start < last and end > first for first, last in damage)

    frames, spans, packets = {}, {}, []
    for match in re.finditer(_PREFIX + b"\xc0", source):
        start = match.start()
        end = start + 6 + int.from_bytes(source[start + 4 : start + 6], "big")
        at = start + 6
        if end > len(source) or not clean(start, at + 1):
            continue
        while at < end and source[at] == 255:
            at += 1
        if at < end and source[at] & 0xC0 == 0x40:
            at += 2
        if at + 5 > end or source[at] >> 4 not in (2, 3):
            continue
        pts = _timestamp(source[at : at + 5])
        at += 10 if source[at] >> 4 == 3 else 5
        if not clean(start, at):
            continue
        _require((pts - first_pts) % step == 0, "MP2 PES leaves 48-kHz frame grid")
        index = (pts - first_pts) // step
        # The first header wholly beginning in this PES has the PES timestamp.
        head = source.find(header, at, end)
        if head < 0:
            continue
        _require(head - at < size, "first MP2 header lost: PES timestamp cannot be reassigned")
        packets.append((head, end, index, at))
        for offset in range(head, end - size + 1, size):
            slot = index + (offset - head) // size
            if clean(offset, offset + size):
                _require(source[offset : offset + 4] == header, "MP2 frame alignment changed")
                _require(slot not in frames, "duplicate recovered MP2 slot")
                frames[slot] = source[offset : offset + size]
                spans[slot] = [[offset, size]]
    for (head, end, index, _), (next_head, _, next_index, next_at) in zip(
        packets,
        packets[1:],
        strict=False,
    ):
        at = head + (end - head) // size * size
        slot, remaining = index + (at - head) // size, end - at
        if (
            remaining
            and next_index == slot + 1
            and next_head - next_at == size - remaining
            and clean(at, end)
            and clean(next_at, next_head)
        ):
            data = source[at:end] + source[next_at:next_head]
            _require(len(data) == size and data[:4] == header, "invalid split MP2 frame")
            _require(slot not in frames, "duplicate split MP2 slot")
            frames[slot] = data
            spans[slot] = [[at, remaining], [next_at, next_head - next_at]]
    return frames, spans


def _mux(events: list[tuple[int, int, int, bytes]]) -> bytes:
    result = bytearray()
    for dts, pts, stream, data in sorted(events, key=lambda item: item[0]):
        scr = max(0, dts - 9000)
        result += _PREFIX + b"\xba" + _stamp(scr, 2) + ((50000 << 1) | 1 << 23 | 1).to_bytes(3)
        stamp = _stamp(pts, 2) if pts == dts else _stamp(pts, 3) + _stamp(dts, 1)
        for offset in range(0, len(data), 65000):
            payload = (stamp if offset == 0 else b"\x0f") + data[offset : offset + 65000]
            result += _PREFIX + bytes([stream]) + len(payload).to_bytes(2) + payload
    return bytes(result) + _PREFIX + b"\xb9"


def reconstruct(source: Path, output: Path, recipe: dict, report: Path) -> dict:
    """Write an explicitly reconstructed, unpadded program stream and receipt.

    Recipe intervals are zero-based, half-open display/audio frame indices.
    Currently qualified for 25-Hz, macroblock-aligned MPEG-1 and unprotected,
    stereo 224-kbit/s 48-kHz MP2 with a regular three-picture reference cadence.
    The caller must additionally run the normal strict importer/decoders and
    verify its pinned output hash before accepting an image repair.
    """
    source, output, report = Path(source), Path(output), Path(report)
    _require(
        len({p.resolve() for p in (source, output, report)}) == 3,
        "reconstruction must not overwrite source or report",
    )
    raw = source.read_bytes()
    _require(
        recipe.get("version") == 1 and _sha(raw) == recipe.get("source_sha256"),
        "source does not match the audited MPEG reconstruction recipe",
    )
    width, height = recipe["width"], recipe["height"]
    _require(
        0 < width < 4096 and 0 < height <= 175 * 16 and width % 16 == height % 16 == 0,
        "invalid reconstruction geometry",
    )
    _require(
        recipe["fps"] == 25
        and recipe["reference_stride"] == 3
        and recipe["audio_header"] == "fffdb400"
        and recipe["audio_frame_bytes"] == 672,
        "unqualified MPEG reconstruction profile",
    )
    first, count = recipe["first_pts"], recipe["video_frames"]
    _require(
        first >= 3600 and 0 < count <= 100000 and count % 3 == 1,
        "unsupported video timeline/end cadence",
    )
    _require(0 < recipe["audio_frames"] <= 200000, "invalid audio timeline")
    _selected(recipe["source_damage"], len(raw))
    video_cut = _selected(recipe["video_replacements"], count)
    audio_cut = _selected(recipe["audio_replacements"], recipe["audio_frames"])
    _require(0 not in video_cut, "initial reference picture must survive")
    pictures = _probe_video(source, first, 3)
    audio, audio_spans = _audio(raw, recipe)
    _require(
        0 in pictures and _picture(pictures[0]) == (0, 1) and _PREFIX + b"\xb3" in pictures[0],
        "video must begin with a complete temporal-zero I picture and sequence header",
    )
    _require(all(index < count for index in pictures), "video exceeds declared timeline")
    _require(
        all(0 <= index < recipe["audio_frames"] for index in audio),
        "audio exceeds declared timeline",
    )
    _require(
        set(range(count)) - pictures.keys() <= video_cut,
        "missing video outside approved replacement intervals",
    )
    for index, data in pictures.items():
        if index in video_cut:
            continue
        _, kind = _picture(data)
        if kind == 2:
            _require(
                index - 3 not in video_cut, "retained P picture depends on a replaced reference"
            )
        elif kind == 3:
            past = index // 3 * 3
            _require(
                past not in video_cut and past + 3 not in video_cut,
                "retained B picture depends on a replaced reference",
            )
    _require(
        set(range(recipe["audio_frames"])) - audio.keys() == audio_cut,
        "audio replacement intervals do not exactly match unavailable source frames",
    )
    events, retained_video, gop_base = [], [], 0
    order = [0]
    for reference in range(3, count, 3):
        order.extend((reference, reference - 2, reference - 1))
    holds = []
    for index in order:
        kind = 3 if index % 3 else 2
        if index in video_cut:
            data = _hold(width, height, index - gop_base, kind)
            holds.append({"frame": index, "reference": (index - 1) // 3 * 3})
        else:
            data = pictures[index]
            temporal, original_kind = _picture(data)
            _require(
                original_kind == kind or original_kind == 1 and index % 3 == 0,
                "original reference cadence differs from reconstruction recipe",
            )
            if original_kind == 1:
                gop_base = index - temporal
            _require(temporal == index - gop_base, "original GOP temporal reference differs")
            sequence = data.find(_PREFIX + b"\xb3")
            if sequence >= 0:
                value = int.from_bytes(data[sequence + 4 : sequence + 8], "big")
                _require(
                    (value >> 20, value >> 8 & 4095, value & 15) == (width, height, 3),
                    "original sequence geometry/rate differs from recipe",
                )
            retained_video.append({"frame": index, "sha256": _sha(data)})
        pts = first + index * 3600
        dts = pts if kind == 3 else pts - (3600 if index == 0 else 10800)
        events.append((dts, pts, 0xE0, data))
    silence = bytes.fromhex(recipe["audio_header"]) + bytes(668)
    retained_audio = []
    for index in range(recipe["audio_frames"]):
        data = silence if index in audio_cut else audio[index]
        if index not in audio_cut:
            retained_audio.append(
                {"frame": index, "sha256": _sha(data), "source_spans": audio_spans[index]}
            )
        pts = first + index * 2160
        events.append((pts, pts, 0xC0, data))
    # Retained last video packet may already contain sequence_end; no extra
    # picture/header is introduced and every program stream gets a program_end.
    result = _mux(events)
    _require(len(result) <= recipe["max_output_bytes"], "reconstruction exceeds available extent")
    receipt = {
        "version": 1,
        "restoration_claim": False,
        "source_sha256": _sha(raw),
        "output_sha256": _sha(result),
        "output_bytes": len(result),
        "video_frames": count,
        "video_seconds": count / 25,
        "audio_frames": recipe["audio_frames"],
        "audio_samples": recipe["audio_frames"] * 1152,
        "audio_seconds": recipe["audio_frames"] * 1152 / 48000,
        "first_pts_seconds": first / 90000,
        "video_replacements": _ranges(video_cut),
        "audio_replacements": _ranges(audio_cut),
        "video_replacement_seconds": [[a / 25, b / 25] for a, b in _ranges(video_cut)],
        "audio_replacement_seconds": [[a * 0.024, b * 0.024] for a, b in _ranges(audio_cut)],
        "hold_references": sorted(holds, key=lambda item: item["frame"]),
        "retained_video": retained_video,
        "retained_audio": retained_audio,
        "audio_boundary_qualification": "Original MP2 bytes survive outside replacements; "
        "synthesis/filter rounding state can differ at and after each splice.",
        "requires_strict_decode": True,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    report.parent.mkdir(parents=True, exist_ok=True)
    _require(
        _sha(source.read_bytes()) == recipe["source_sha256"], "source changed during reconstruction"
    )
    # Replacing a temporary file cannot modify the original through an existing
    # hard link, and a failed reconstruction never leaves a partial result.
    for target, data in (
        (output, result),
        (report, (json.dumps(receipt, indent=2) + "\n").encode()),
    ):
        descriptor, name = tempfile.mkstemp(prefix=".mpeg-salvage-", dir=target.parent)
        temporary = Path(name)
        try:
            with os.fdopen(descriptor, "wb") as handle:
                handle.write(data)
            temporary.replace(target)
        finally:
            temporary.unlink(missing_ok=True)
    return receipt

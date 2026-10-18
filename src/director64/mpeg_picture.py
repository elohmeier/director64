"""Explicit, hash-pinned MPEG-1 B-picture reconstruction.

This is concealment, not restoration of unknown pixels. A selected B picture
holds its immediately preceding I/P reference for one display interval. No
other picture, audio payload, packet header, timestamp, or file length changes.
The caller must separately verify its source-file policy and decode the result.
"""

import hashlib
import re
from dataclasses import dataclass
from fractions import Fraction

_PREFIX = b"\0\0\1"
_RATES = {1: (24000, 1001), 2: (24, 1), 3: (25, 1), 4: (30000, 1001),
          5: (30, 1), 6: (50, 1), 7: (60000, 1001), 8: (60, 1)}


@dataclass(frozen=True)
class HoldPicture:
    """All indices are zero-based; rate/scale is the exact frame frequency."""

    coded_index: int
    display_index: int
    temporal_reference: int
    width: int
    height: int
    rate: int
    scale: int = 1


@dataclass
class _Picture:
    start: int
    header_end: int
    end: int
    temporal: int
    kind: int
    slices: list[int]


def _sha(data: bytes | bytearray) -> str:
    return hashlib.sha256(data).hexdigest()


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _pts(data: bytes, at: int, end: int, prefix: int) -> None:
    _require(at + 5 <= end, "truncated MPEG timestamp")
    value = data[at:at + 5]
    _require(value[0] >> 4 == prefix and all(value[i] & 1 for i in (0, 2, 4)),
             "invalid MPEG timestamp markers")


def _payload(data: bytes, at: int, end: int) -> int:
    """Locate a bounded MPEG-1 or MPEG-2 PES payload without changing headers."""
    while at < end and data[at] == 255:
        at += 1
    _require(at < end, "empty MPEG PES packet")
    if data[at] & 0xC0 == 0x80:
        _require(at + 3 <= end, "truncated MPEG-2 PES header")
        header_end = at + 3 + data[at + 2]
        _require(header_end <= end, "truncated MPEG-2 PES optional header")
        flags = data[at + 1] >> 6
        _require(flags != 1, "invalid MPEG-2 PTS/DTS flags")
        if flags in (2, 3):
            _pts(data, at + 3, header_end, flags)
        if flags == 3:
            _pts(data, at + 8, header_end, 1)
        return header_end
    if data[at] & 0xC0 == 0x40:
        at += 2  # MPEG-1 STD buffer field
    _require(at < end, "truncated MPEG-1 PES header")
    marker = data[at] >> 4
    if marker in (2, 3):
        _pts(data, at, end, marker)
        if marker == 3:
            _pts(data, at + 5, end, 1)
        return at + (5 if marker == 2 else 10)
    _require(data[at] == 15, "unsupported MPEG-1 PES header")
    return at + 1


def _demux(data: bytes) -> tuple[bytes, list[tuple[int, int, int]], dict[int, bytes]]:
    _require(data.startswith(_PREFIX + b"\xba"), "expected an MPEG program stream")
    video = bytearray()
    audio: dict[int, bytearray] = {}
    spans: list[tuple[int, int, int]] = []
    video_id = None
    at = 0
    while at < len(data):
        # Zero padding between packs, including a bounded all-zero tail.
        while data[at:at + 3] == b"\0\0\0":
            at += 1
        if len(data) - at < 4 and not any(data[at:]):
            break
        _require(data[at:at + 3] == _PREFIX and at + 4 <= len(data),
                 f"MPEG packet synchronization at {at}")
        code = data[at + 3]
        if code == 0xB9:
            _require(not any(data[at + 4:]), "data after MPEG program end")
            break
        if code == 0xBA:
            _require(at + 5 <= len(data), "truncated MPEG pack header")
            first = data[at + 4]
            if first & 0xF0 == 0x20:
                size = 12
            else:
                _require(first & 0xC0 == 0x40, "invalid MPEG pack header")
                _require(at + 14 <= len(data), "truncated MPEG-2 pack header")
                size = 14 + (data[at + 13] & 7)
            _require(at + size <= len(data), "truncated MPEG pack header")
            at += size
            continue
        _require(code >= 0xBB, f"unexpected MPEG packet code {code:#x}")
        _require(at + 6 <= len(data), "truncated MPEG packet length")
        length = int.from_bytes(data[at + 4:at + 6], "big")
        _require(length > 0, "unbounded/empty MPEG packets are not supported")
        end = at + 6 + length
        _require(end <= len(data), "truncated MPEG packet")
        if 0xC0 <= code <= 0xEF:
            payload = _payload(data, at + 6, end)
            if code >= 0xE0:
                _require(video_id in (None, code), "multiple MPEG video streams")
                video_id = code
                spans.append((len(video), payload, end - payload))
                video.extend(data[payload:end])
            else:
                audio.setdefault(code, bytearray()).extend(data[payload:end])
        at = end
    _require(bool(video), "MPEG program stream has no video")
    return bytes(video), spans, {key: bytes(value) for key, value in audio.items()}


class _Bits:
    def __init__(self, data: bytes):
        self.data = data
        self.at = 0

    def read(self, count: int) -> int:
        _require(self.at + count <= len(self.data) * 8, "truncated MPEG video header")
        result = 0
        for _ in range(count):
            result = (result << 1) | ((self.data[self.at // 8] >> (7 - self.at % 8)) & 1)
            self.at += 1
        return result


def _pictures(video: bytes, spec: HoldPicture) -> tuple[list[_Picture], list[int]]:
    codes = [(match.start(), video[match.start() + 3])
             for match in re.finditer(rb"\x00\x00\x01[\s\S]", video)]
    _require(bool(codes) and codes[0] == (0, 0xB3), "missing MPEG sequence header")
    pictures: list[_Picture] = []
    current = None
    ended = False
    for index, (at, code) in enumerate(codes):
        following = codes[index + 1][0] if index + 1 < len(codes) else len(video)
        _require(not ended, "data after MPEG sequence end")
        if code in (0, 0xB3, 0xB7, 0xB8) and current is not None:
            pictures[current].end = at
            current = None
        bits = _Bits(video[at + 4:following])
        if code == 0xB3:
            width, height = bits.read(12), bits.read(12)
            bits.read(4)  # aspect ratio does not change the coded sample grid
            rate = _RATES.get(bits.read(4))
            _require((width, height) == (spec.width, spec.height),
                     "MPEG dimensions differ from reconstruction specification")
            _require(rate is not None and Fraction(*rate) == Fraction(spec.rate, spec.scale),
                     "MPEG frame rate differs from reconstruction specification")
            bits.read(18)
            _require(bits.read(1) == 1, "invalid MPEG sequence marker")
            bits.read(11)
            if bits.read(1):
                bits.read(512)
            if bits.read(1):
                bits.read(512)
        elif code == 0:
            temporal, kind = bits.read(10), bits.read(3)
            bits.read(16)  # vbv_delay is preserved verbatim
            _require(kind in (1, 2, 3), "unsupported MPEG picture type")
            for _ in range(1 if kind == 2 else 2 if kind == 3 else 0):
                bits.read(1)
                _require(bits.read(3) != 0, "invalid MPEG motion-vector f_code")
            while bits.read(1):
                bits.read(8)
            # Byte alignment may contain only zero stuffing bits.
            while bits.at < len(bits.data) * 8:
                _require(bits.read(1) == 0, "nonzero MPEG picture-header padding")
            pictures.append(_Picture(at, following, len(video), temporal, kind, []))
            current = len(pictures) - 1
        elif 1 <= code <= 0xAF:
            _require(current is not None, "MPEG slice outside a picture")
            pictures[current].slices.append(code)
        elif code == 0xB7:
            ended = True
            _require(not any(video[at + 4:]), "data after MPEG sequence end")
        elif code == 0xB5:
            raise ValueError("MPEG extensions are not supported by this MPEG-1 reconstruction")
        else:
            _require(code in (0xB2, 0xB8), f"unexpected MPEG video start code {code:#x}")
    _require(ended, "missing MPEG sequence end")
    _require(bool(pictures) and pictures[0].kind == 1, "video must begin with an I picture")
    display = []
    pending = None
    references = 0
    for index, picture in enumerate(pictures):
        if picture.kind == 3:
            _require(references >= 2, "open-GOP pictures without both references are unsupported")
            display.append(index)
        else:
            references += 1
            if pending is not None:
                display.append(pending)
            pending = index
    if pending is not None:
        display.append(pending)
    return pictures, display


def reconstruct_hold(
    source: bytes, *, video_sha256: str, picture: HoldPicture,
) -> tuple[bytes, dict]:
    """Return a separately labeled reconstruction and structural receipt.

    The full elementary-video SHA256 must match. The supported picture is an
    MPEG-1 B frame immediately after its forward I/P reference in display order,
    with macroblock-aligned dimensions. The result needs independent strict
    decoding before use; this routine does not certify arbitrary MPEG syntax.
    """
    _require(isinstance(source, bytes), "source must be immutable bytes")
    _require(isinstance(video_sha256, str)
             and bool(re.fullmatch(r"[0-9a-f]{64}", video_sha256)), "invalid video SHA256 pin")
    spec = picture
    _require(all(isinstance(value, int) and not isinstance(value, bool) for value in (
        spec.coded_index, spec.display_index, spec.temporal_reference, spec.width,
        spec.height, spec.rate, spec.scale,
    )), "reconstruction specification values must be integers")
    _require(spec.coded_index >= 0 and spec.display_index > 0
             and 0 <= spec.temporal_reference < 1024, "invalid picture indices/reference")
    _require(0 < spec.width < 4096 and 0 < spec.height <= 175 * 16
             and spec.width % 16 == 0 and spec.height % 16 == 0,
             "reconstruction requires bounded macroblock-aligned dimensions")
    _require(spec.rate > 0 and spec.scale > 0, "invalid reconstruction frame rate")
    video, spans, audio = _demux(source)
    _require(_sha(video) == video_sha256, "elementary-video SHA256 does not match pin")
    pictures, display = _pictures(video, spec)
    _require(spec.coded_index < len(pictures) and spec.display_index < len(display),
             "selected picture is outside stream")
    target = pictures[spec.coded_index]
    _require(target.kind == 3, "selected picture is not a non-reference B picture")
    _require(target.temporal == spec.temporal_reference, "picture temporal reference differs")
    _require(display[spec.display_index] == spec.coded_index, "picture display index differs")
    refs = [i for i in range(spec.coded_index) if pictures[i].kind != 3]
    forward = refs[-2]
    _require(display[spec.display_index - 1] == forward,
             "preceding displayed picture is not the selected B picture's forward reference")
    rows = spec.height // 16
    _require(target.slices == list(range(1, rows + 1)),
             "selected picture does not have one complete ordered slice per macroblock row")
    # quantizer_scale=8/extra_bit_slice=0; increment1, forward prediction
    # without residual (0010), and zero horizontal/vertical motion_code (1).
    row_bits = "010000" + "1001011" * (spec.width // 16)
    row_bits += "0" * (-len(row_bits) % 8)
    row = int(row_bits, 2).to_bytes(len(row_bits) // 8, "big")
    slices = b"".join(_PREFIX + bytes([number]) + row for number in range(1, rows + 1))
    seconds = Fraction(spec.display_index * spec.scale, spec.rate)
    duration = Fraction(spec.scale, spec.rate)
    label = (
        f"DIRECTOR64 EXPLICIT RECONSTRUCTION: display frame {spec.display_index} "
        f"at {float(seconds):.3f}s held from frame {spec.display_index - 1} "
        f"for {float(duration * 1000):g}ms; not original pixels. "
    ).encode("ascii")
    padding = target.end - target.header_end - 4 - len(slices)
    _require(padding >= len(label), "selected picture lacks space for labeled reconstruction")
    replacement = (video[target.start:target.header_end] + _PREFIX + b"\xb2" + label
                   + b" " * (padding - len(label)) + slices)
    _require(len(replacement) == target.end - target.start, "reconstruction length mismatch")
    output = bytearray(source)
    touched = []
    for elementary_at, file_at, size in spans:
        begin, end = max(elementary_at, target.start), min(elementary_at + size, target.end)
        if begin < end:
            destination = file_at + begin - elementary_at
            output[destination:destination + end - begin] = replacement[
                begin - target.start:end - target.start]
            touched.append({"offset": destination, "bytes": end - begin})
    result = bytes(output)
    expected = video[:target.start] + replacement + video[target.end:]
    changed_video, changed_spans, changed_audio = _demux(result)
    _require(changed_video == expected and changed_spans == spans and changed_audio == audio,
             "reconstruction unexpectedly changed stream structure or audio")
    previous = 0
    for span in touched:
        start, end = span["offset"], span["offset"] + span["bytes"]
        _require(source[previous:start] == result[previous:start],
                 "reconstruction changed bytes outside selected video payload")
        previous = end
    _require(source[previous:] == result[previous:] and len(result) == len(source),
             "reconstruction changed trailing bytes or file length")
    receipt = {
        "method": "mpeg1-b-picture-hold-v1", "restoration_claim": False,
        "source_sha256": _sha(source), "output_sha256": _sha(result),
        "source_video_sha256": video_sha256, "output_video_sha256": _sha(changed_video),
        "audio_streams": {str(key): {"bytes": len(value), "sha256": _sha(value)}
                          for key, value in audio.items()},
        "source_size": len(source), "output_size": len(result),
        "coded_picture_index": spec.coded_index, "display_frame_index": spec.display_index,
        "forward_reference_coded_index": forward, "temporal_reference": target.temporal,
        "width": spec.width, "height": spec.height, "rate": spec.rate, "scale": spec.scale,
        "relative_video_seconds": float(seconds), "duration_seconds": float(duration),
        "picture_offset": target.start, "picture_bytes": target.end - target.start,
        "changed_bytes": sum(a != b for a, b in zip(source, result, strict=True)),
        "video_payload_ranges": touched, "packet_headers_and_audio_unchanged": True,
        "all_changes_confined_to_replaced_picture": True, "requires_strict_decode": True,
    }
    return result, receipt

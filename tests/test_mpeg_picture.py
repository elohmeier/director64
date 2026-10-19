"""Handwritten MPEG syntax; no recovered media or game-specific pins."""

import hashlib
import subprocess
from dataclasses import replace

import pytest

from director64.aot import repository_root
from director64.mpeg_picture import HoldPicture, reconstruct_hold

PREFIX = b"\0\0\1"
SPEC = HoldPicture(3, 2, 2, 16, 16, 25)


def bits(value):
    value += "0" * (-len(value) % 8)
    return int(value, 2).to_bytes(len(value) // 8, "big")


def picture(kind, temporal, *, bright=False, padding=300):
    header = f"{temporal:010b}{kind:03b}" + "1" * 16
    if kind in (2, 3):
        header += "0001"
    if kind == 3:
        header += "0001"
    header += "0"  # extra_bit_picture
    result = PREFIX + b"\0" + bits(header)
    body = "010000" + "1"  # qscale8, extra_bit_slice0, macroblock increment1
    if kind == 1:
        # First luma block's DC differential16 produces gray144; zero is128.
        body += "1" + ("111010000" if bright else "100") + "10"
        body += "10010" * 3 + "0010" * 2
    elif kind == 2:
        body += "00111"  # forward prediction, zero motion, no residual
    else:
        # B-picture backward prediction copies the FUTURE bright I picture.
        body += "01011"
        result += PREFIX + b"\xb2" + b"fixture " * (padding // 8)
    return result + PREFIX + b"\x01" + bits(body)


def elementary(*, two_b=False, padding=300):
    sequence = (f"{16:012b}{16:012b}" + "00010011" + f"{1:018b}" + "1"
                + "0" * 13)
    value = PREFIX + b"\xb3" + bits(sequence)
    value += picture(1, 0) + picture(2, 1) + picture(1, 4 if two_b else 3, bright=True)
    value += picture(3, 2, padding=padding)
    if two_b:
        value += picture(3, 3, padding=padding)
    return value + PREFIX + b"\xb7"


def timestamp(prefix, value):
    return bytes([(prefix << 4) | (((value >> 30) & 7) << 1) | 1,
                  (value >> 22) & 255, (((value >> 15) & 127) << 1) | 1,
                  (value >> 7) & 255, ((value & 127) << 1) | 1])


def program(video, *, mpeg2=False, split=37):
    pack = (b"\x44\0\x04\0\x04\x01\0\0\x03\xfa\xff\xff" if mpeg2
            else b"\x21\0\x01\0\x01\x80\0\x01")
    value = bytearray(PREFIX + b"\xba" + pack)
    # Opaque system/private records are preserved, not mistaken for video.
    value += PREFIX + b"\xbb\0\x03sys" + PREFIX + b"\xbd\0\x04priv"
    spans, packet_at = [], []
    for at in range(0, len(video), split):
        header = ((b"\x80\xc0\x0a" if mpeg2 else b"\xff\xff\x40\0")
                  + timestamp(3, 90000 + at) + timestamp(1, 86400 + at))
        chunk = video[at:at + split]
        packet_at.append(len(value))
        value += PREFIX + b"\xe0" + (len(header) + len(chunk)).to_bytes(2, "big")
        value += header
        spans.append((len(value), len(chunk)))
        value += chunk
        for stream in (0xC0, 0xC1):
            audio = bytes([stream, at % 256]) + b"unchanged audio"
            audio_header = b"\x80\0\0" if mpeg2 else b"\x0f"
            value += (PREFIX + bytes([stream])
                      + (len(audio_header) + len(audio)).to_bytes(2, "big")
                      + audio_header + audio)
        value += b"\0\0"  # legal padding before next packet
    value += PREFIX + b"\xb9\0\0"
    return bytes(value), spans, packet_at


def run(source, video, spec=SPEC):
    return reconstruct_hold(source, video_sha256=hashlib.sha256(video).hexdigest(), picture=spec)


@pytest.mark.parametrize("mpeg2", [False, True])
@pytest.mark.parametrize("split", [1, 37, 10000])
def test_reconstruction_preserves_packets_audio_and_other_pictures(mpeg2, split):
    video = elementary()
    source, spans, _ = program(video, mpeg2=mpeg2, split=split)
    original = bytes(source)
    output, receipt = run(source, video)
    assert source == original
    assert len(output) == len(source)
    assert output != source
    reconstructed = b"".join(output[at:at + size] for at, size in spans)
    selected = video.index(PREFIX + b"\0", video.index(PREFIX + b"\0", 16) + 4)
    selected = video.index(PREFIX + b"\0", selected + 4)
    assert receipt["picture_offset"] == selected
    assert reconstructed[:selected] == video[:selected]
    assert reconstructed[-4:] == video[-4:]
    assert b"EXPLICIT RECONSTRUCTION" in reconstructed
    assert b"not original pixels" in reconstructed
    mask = bytearray(len(source))
    for at, size in spans:
        mask[at:at + size] = b"\1" * size
    assert all(a == b for a, b, marked in zip(source, output, mask, strict=True) if not marked)
    assert receipt["source_video_sha256"] == hashlib.sha256(video).hexdigest()
    assert receipt["output_video_sha256"] == hashlib.sha256(reconstructed).hexdigest()
    assert receipt["source_sha256"] == hashlib.sha256(source).hexdigest()
    assert receipt["output_sha256"] == hashlib.sha256(output).hexdigest()
    assert set(receipt["audio_streams"]) == {"192", "193"}
    assert receipt["forward_reference_coded_index"] == 1
    assert receipt["relative_video_seconds"] == 0.08
    assert receipt["duration_seconds"] == 0.04
    assert receipt["restoration_claim"] is False
    assert receipt["requires_strict_decode"] is True


def test_current_decoder_proves_held_pixels_and_unaffected_frames(tmp_path):
    video = elementary()
    source, spans, _ = program(video, split=17)
    output, _ = run(source, video)
    fixed = b"".join(output[at:at + size] for at, size in spans)
    (tmp_path / "source.m1v").write_bytes(video)
    (tmp_path / "hold.m1v").write_bytes(fixed)
    harness = tmp_path / "verify.c"
    harness.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tests/native/mpeg_tool.c"
static uint8_t *read_file(const char *path, unsigned *length) {
  FILE *f = fopen(path, "rb"); assert(f);
  assert(fseek(f, 0, SEEK_END) == 0); *length = (unsigned)ftell(f); rewind(f);
  uint8_t *data = malloc(*length); assert(data);
  assert(fread(data, 1, *length, f) == *length); fclose(f); return data;
}
int main(int argc, char **argv) {
  assert(argc == 3); unsigned a_size, b_size;
  uint8_t *a = read_file(argv[1], &a_size), *b = read_file(argv[2], &b_size);
  d64_mpeg_t *x = d64m_open(a, a_size, 0), *y = d64m_open(b, b_size, 0);
  assert(x && y); assert(d64m_width(x) == 16 && d64m_height(x) == 16);
  uint8_t previous[16 * 16 * 4]; unsigned count = 0, differences = 0;
  for (;;) {
    uint8_t *original = d64m_frame(x), *held = d64m_frame(y);
    if (!original || !held) { assert(!original && !held); break; }
    if (memcmp(original, held, sizeof(previous))) { assert(count == 2); differences++; }
    if (count == 2) assert(!memcmp(previous, held, sizeof(previous)));
    memcpy(previous, original, sizeof(previous)); count++;
  }
  assert(count == 4 && differences == 1);
  assert(d64m_corrupt(x) == 0 && d64m_corrupt(y) == 0);
  d64m_close(x); d64m_close(y); free(a); free(b); return 0;
}
''')
    root = repository_root()
    executable = tmp_path / "verify"
    subprocess.run(["cc", "-std=c17", "-Wall", "-Wextra", "-Werror", "-O2", f"-I{root}",
                    f"-I{root / 'third_party/pl_mpeg'}", str(harness), "-lm", "-o", executable],
                   check=True, capture_output=True, text=True)
    subprocess.run([executable, tmp_path / "source.m1v", tmp_path / "hold.m1v"],
                   check=True, capture_output=True, text=True)


def test_pin_is_mandatory_and_reconstruction_cannot_be_reapplied():
    video = elementary()
    source, _, _ = program(video)
    with pytest.raises(ValueError, match="does not match pin"):
        reconstruct_hold(source, video_sha256="0" * 64, picture=SPEC)
    output, _ = run(source, video)
    with pytest.raises(ValueError, match="does not match pin"):
        run(output, video)


@pytest.mark.parametrize(("change", "message"), [
    ({"coded_index": 2, "display_index": 3}, "not a non-reference B"),
    ({"coded_index": 1000}, "outside stream"),
    ({"display_index": 1}, "display index differs"),
    ({"temporal_reference": 3}, "temporal reference differs"),
    ({"width": 32}, "dimensions differ"),
    ({"height": 15}, "macroblock-aligned"),
    ({"rate": 24}, "frame rate differs"),
    ({"scale": 0}, "invalid reconstruction frame rate"),
])
def test_wrong_picture_specification_is_rejected(change, message):
    video = elementary()
    source, _, _ = program(video)
    with pytest.raises(ValueError, match=message):
        run(source, video, replace(SPEC, **change))


def test_b_after_another_b_cannot_claim_to_hold_the_preceding_displayed_frame():
    video = elementary(two_b=True)
    source, _, _ = program(video)
    with pytest.raises(ValueError, match="not the selected B picture's forward reference"):
        run(source, video, replace(SPEC, coded_index=4, display_index=3, temporal_reference=3))


@pytest.mark.parametrize(("mutation", "message"), [
    (lambda v: v[:-1], "missing MPEG sequence end"),
    (lambda v: v[:-4] + PREFIX + b"\xb5\0\0\0" + v[-4:], "MPEG extensions"),
    (lambda v: v[:-4] + PREFIX + b"\x02\0\0" + v[-4:], "complete ordered slice"),
])
def test_unqualified_video_structures_are_rejected_even_with_matching_hash(mutation, message):
    video = mutation(elementary())
    source, _, _ = program(video)
    with pytest.raises(ValueError, match=message):
        run(source, video)


def test_rejects_picture_without_space_for_visible_reconstruction_label():
    video = elementary(padding=0)
    source, _, _ = program(video)
    with pytest.raises(ValueError, match="lacks space"):
        run(source, video)


@pytest.mark.parametrize("mpeg2", [False, True])
def test_packet_truncation_and_invalid_timestamp_fail_before_any_output(mpeg2):
    video = elementary()
    source, _, starts = program(video, mpeg2=mpeg2, split=10000)
    for length in (0, 1, 4, 7, starts[0] + 5, starts[0] + 8, len(source) - 9):
        with pytest.raises(ValueError):
            run(source[:length], video)
    malformed = bytearray(source)
    # PTS follows three MPEG2 header bytes or four MPEG1 stuffing/STD bytes.
    pts = starts[0] + 6 + (3 if mpeg2 else 4)
    malformed[pts] &= ~1
    with pytest.raises(ValueError, match="timestamp markers"):
        run(bytes(malformed), video)


@pytest.mark.parametrize(("delta", "message"), [(0, "unbounded"), (65535, "truncated")])
def test_video_packet_length_cannot_escape_its_container(delta, message):
    video = elementary()
    source, _, starts = program(video, split=10000)
    malformed = bytearray(source)
    malformed[starts[0] + 4:starts[0] + 6] = delta.to_bytes(2, "big")
    with pytest.raises(ValueError, match=message):
        run(bytes(malformed), video)


def test_rejects_multiple_video_streams_and_nonpadding_trailer():
    video = elementary()
    source, _, starts = program(video, split=37)
    malformed = bytearray(source)
    malformed[starts[1] + 3] = 0xE1
    with pytest.raises(ValueError, match="multiple MPEG video"):
        run(bytes(malformed), video)
    with pytest.raises(ValueError, match="data after MPEG program end"):
        run(source + b"unexpected", video)

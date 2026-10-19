"""The conversion driver's encoders and packing, on synthetic media (FFmpeg)."""

from __future__ import annotations

import json
import shutil
import struct
import subprocess
import zlib

import pytest

from director64 import convert

needs_ffmpeg = pytest.mark.skipif(not shutil.which("ffmpeg"), reason="needs ffmpeg")


def container(data: bytes) -> dict:
    length = struct.unpack_from("<I", data)[0]
    return json.loads(data[4:4 + length])


def mpeg(with_audio: bool) -> bytes:
    audio = ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=0.4",
             "-c:a", "mp2", "-b:a", "128k"] if with_audio else []
    return subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "testsrc2=size=32x32:rate=25:duration=0.4", *audio, "-c:v", "mpeg1video", "-bf", "2",
         "-f", "mpeg", "pipe:1"], capture_output=True, check=True, timeout=60,
    ).stdout


@needs_ffmpeg
def test_decoded_frames_encode_at_their_own_times(tmp_path):
    times = [0, 100000, 250000]
    frames = tmp_path / "frames"
    with frames.open("wb") as out:
        for i in range(len(times)):
            pixels = bytearray(16 * 16 * 4)
            for at in range(i % 3, len(pixels), 4):
                pixels[at] = 255
            out.write(pixels)
    data = convert.encode_frames(frames, {"width": 16, "height": 16, "times": times})
    header = container(data)
    assert header["codec"] == convert.VP9_CODEC and header["width"] == 16
    assert [f[0] for f in header["frames"]] == times
    assert header["frames"][0][1] == 1
    offsets = [f[2] for f in header["frames"]]
    assert offsets == sorted(offsets) and offsets[0] == 0


@needs_ffmpeg
def test_mpeg_export_separates_silence_alignment_and_failures():
    silent, voiced = mpeg(False), mpeg(True)
    assert convert.mpeg_audio(silent, {}) is None
    header = container(convert.encode_mpeg(voiced, {}))
    assert len(header["frames"]) == 10
    assert header["frames"][0][0] == 0 and header["frames"][9][0] == 360000
    plain = convert.mpeg_audio(voiced, {})
    early = convert.mpeg_audio(voiced, {"audio_lead_seconds": 0.01})
    late = convert.mpeg_audio(voiced, {"audio_lead_seconds": -0.01})
    assert plain[:4] == b"RIFF" and plain[36:40] == b"data"
    rate, channels = struct.unpack_from("<I", plain, 24)[0], struct.unpack_from("<H", plain, 22)[0]
    step = round(rate / 100) * channels * 2
    assert len(plain) - len(early) == step and len(late) - len(plain) == step
    with pytest.raises(ValueError, match="ffmpeg exited"):
        convert.mpeg_audio(b"\1\2\3", {})
    with pytest.raises(ValueError, match="ffmpeg exited"):
        convert.encode_mpeg(b"\1\2\3", {})


@needs_ffmpeg
def test_only_long_wavs_become_opus():
    def wav(seconds: float) -> bytes:
        rate, frames = 8000, int(8000 * seconds)
        data = bytes(frames * 2)
        return (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt "
                + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
                + b"data" + struct.pack("<I", len(data)) + data)

    short = wav(1)
    assert convert.opus(short) == short
    assert convert.opus(b"ID3 mp3") == b"ID3 mp3"
    assert convert.opus(wav(convert.OPUS_SECONDS))[:4] == b"OggS"


def test_stored_images_deflate_as_the_players_inflate_them():
    raw = b"FDI1" + bytes(200)
    packed = convert.deflated(raw)
    assert packed[:4] == b"D64Z" and struct.unpack_from(">I", packed, 4)[0] == len(raw)
    assert zlib.decompress(packed[8:], -15) == raw


def test_ivf_without_a_keyframe_is_refused():
    header = b"DKIF" + struct.pack("<HHIHHIIII", 0, 32, 0x30395056, 16, 16, 15, 1, 1, 0)
    frame = bytes([0x86, 0, 0])  # profile 0, inter frame
    data = header + struct.pack("<IQ", len(frame), 0) + frame
    with pytest.raises(ValueError, match="keyframe"):
        convert.ivf_frames(data)


def test_host_tools_rebuild_only_when_their_sources_change(tmp_path):
    (tmp_path / "src").mkdir()
    (tmp_path / "src/tool.c").write_text("one")
    builds = []

    def build(folder):
        builds.append(folder)
        (folder / "tool").write_text("built")

    for _ in range(2):
        assert convert.built(tmp_path, "tool", ["src"], build) == tmp_path / convert.TOOLS / "tool"
    assert len(builds) == 1
    (tmp_path / "src/tool.c").write_text("two")
    convert.built(tmp_path, "tool", ["src"], build)
    assert len(builds) == 2


# ---- Reusing a conversion ----


def repair_game(tmp_path):
    import hashlib

    from director64.project import GameSpec

    original, repaired = b"original disc", b"repaired disc"
    source = {
        "id": "sha256-" + hashlib.sha256(original).hexdigest()[:12],
        "file": "original.iso", "sha256": hashlib.sha256(original).hexdigest(),
        "bytes": len(original), "kind": "iso9660",
        "repairs": [{"file": "repaired.iso", "sha256": hashlib.sha256(repaired).hexdigest(),
                     "bytes": len(repaired), "recipe": "host/media-repair.json"}],
    }
    game = GameSpec(tmp_path, "fixture", {"port": {"director_version": 850}}, source)
    game.media.parent.mkdir(parents=True)
    game.media.write_bytes(original)
    return game, game.media.parent / "repaired.iso", repaired


@pytest.fixture
def fake_conversion(monkeypatch):
    """convert.convert around a converter that writes the selected image's
    name into every output, counting its runs; no host tools run."""
    from director64 import web

    runs, during = [], []
    monkeypatch.setattr(convert, "converter_revision", lambda root: "converter")
    monkeypatch.setattr(convert, "profile_sha256", lambda game: json.dumps(profile))
    monkeypatch.setattr(web, "abi_digest", lambda root: b"abi")
    monkeypatch.setattr(web, "validate_package", lambda *args: None)
    profile = {"policy": {}}

    class Converter:
        def __init__(self, game, out, meta):
            self.game, self.out, self.meta = game, out, meta
            self.work = out.parent / "conversion"
            self.report = {"sounds": 0, "images": 0}

        def convert(self):
            image = self.game.image
            assert self.meta["source_sha256"] == self.game.source["sha256"]
            assert self.meta.get("source_image", image)["file"] == image["file"]
            runs.append(image["file"])
            self.out.mkdir(parents=True, exist_ok=True)
            for name in convert.GAME_FILES:
                (self.out / name).write_bytes(image["file"].encode())
            for hook in during:
                hook()

    monkeypatch.setattr(convert, "Converter", Converter)
    return runs, profile, during


def test_a_conversion_is_reused_until_its_inputs_or_outputs_change(tmp_path, fake_conversion):
    from director64 import web

    runs, profile, _ = fake_conversion
    game, _, _ = repair_game(tmp_path)
    out = tmp_path / "converted"
    assert convert.convert(game, out) == out
    assert convert.convert(game, out) == out
    assert len(runs) == 1
    profile["policy"]["native_stage"] = True
    convert.convert(game, out)
    assert len(runs) == 2
    (out / "images.bin").write_bytes(b"tampered")
    convert.convert(game, out)
    assert len(runs) == 3
    convert.convert(game, out, force=True)
    assert len(runs) == 4

    # Matching manifest hashes cannot bypass the runtime's loader.
    def rejected(*args):
        raise ValueError("runtime rejected converted package")

    web.validate_package = rejected
    with pytest.raises(ValueError, match="runtime rejected"):
        convert.convert(game, out)


def test_conversion_tracks_the_selected_image_in_both_directions(tmp_path, fake_conversion):
    runs, _, _ = fake_conversion
    game, repaired, data = repair_game(tmp_path)
    out = tmp_path / "converted"
    convert.convert(game, out)
    convert.convert(game, out)
    repaired.write_bytes(data)
    convert.convert(game, out)
    convert.convert(game, out)
    repaired.unlink()
    convert.convert(game, out)
    assert runs == ["original.iso", "repaired.iso", "original.iso"]
    # A manifest that does not name the image cannot establish its source.
    repaired.write_bytes(data)
    convert.convert(game, out)
    manifest = json.loads((out / "manifest.json").read_text())
    del manifest["source_image"]
    (out / "manifest.json").write_text(json.dumps(manifest))
    convert.convert(game, out)
    assert len(runs) == 5
    repaired.write_bytes(b"modified disc")
    with pytest.raises(ValueError, match="source image differs"):
        convert.convert(game, out)


def test_conversion_rejects_a_source_selection_changed_while_it_ran(tmp_path, fake_conversion):
    _, _, during = fake_conversion
    game, repaired, data = repair_game(tmp_path)
    during.append(lambda: repaired.write_bytes(data))
    with pytest.raises(ValueError, match="selected source image changed"):
        convert.convert(game, tmp_path / "converted")
    assert not (tmp_path / "converted/manifest.json").exists()


def test_source_digest_covers_compiler_tools_schema_and_vendored_libraries(tmp_path):
    files = [
        "compiler/src/synthetic.rs",
        "tools/convert/webp_tool.c",
        "runtime/package/package.h",
        "third_party/libwebp/src/decoder.h",
    ]
    for name in files:
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("original")
    first = convert.source_digest(tmp_path)
    for name in files:
        path = tmp_path / name
        path.write_text("changed")
        assert convert.source_digest(tmp_path) != first
        path.write_text("original")
    generated = tmp_path / "build/unrelated.bin"
    generated.parent.mkdir()
    generated.write_bytes(b"generated output")
    assert convert.source_digest(tmp_path) == first

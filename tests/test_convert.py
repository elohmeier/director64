"""The conversion driver's encoders and packing, on synthetic media (FFmpeg)."""

from __future__ import annotations

import json
import shutil
import struct
import subprocess
import zlib
from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace

import pytest

from director64 import convert
from director64.convert import check_handlers
from director64.lingo import parse_movie

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
def test_every_pcm_clip_becomes_opus_with_source_sample_bounds():
    def wav(seconds: float) -> bytes:
        rate, frames = 8000, int(8000 * seconds)
        data = bytes(frames * 2)
        return (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt "
                + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
                + b"data" + struct.pack("<I", len(data)) + data)

    assert convert.opus(b"ID3 mp3") == b"ID3 mp3"
    for seconds in (0.01, 0.1, 1, 8):
        encoded = convert.opus(wav(seconds))
        assert encoded[:4] == b"OggS"
        assert b"D64_SOURCE_RATE=8000" in encoded
        assert f"D64_SOURCE_FRAMES={int(seconds * 8000)}".encode() in encoded


def test_stored_images_deflate_as_the_players_inflate_them():
    raw = b"FDI1" + bytes(200)
    packed = convert.deflated(raw)
    assert packed[:4] == b"D64Z" and struct.unpack_from(">I", packed, 4)[0] == len(raw)
    assert zlib.decompress(packed[8:], -15) == raw


def test_stream_duplicates_need_an_explicit_preference_and_root_keys_have_no_slash(tmp_path):
    from director64.project import converter

    def wav(sample):
        data = bytes([sample]) * 160
        return (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt "
                + struct.pack("<IHHIIHH", 16, 1, 1, 8000, 16000, 2, 16)
                + b"data" + struct.pack("<I", len(data)) + data)

    folder = tmp_path / "sounds"
    folder.mkdir()
    first, second = wav(1), wav(2)
    # Containers are detected by magic: retail files also use misleading suffixes.
    (folder / "Voice.wav").write_bytes(first)
    (folder / "Voice.aif").write_bytes(second)
    target = tmp_path / "streams.bin"
    command = [*converter(), "director", "stream-pack", str(target)]
    result = subprocess.run([*command, f"={folder}"], capture_output=True, text=True)
    assert result.returncode and "two sound files" in result.stderr
    for flag in ("--prefer-wav", "--prefer-lossless"):
        result = subprocess.run([*command, flag, f"={folder}"], capture_output=True,
                                text=True, check=True)
        assert set(json.loads(result.stdout)) == {"voice"}
        assert target.read_bytes() == first
    (folder / "Voice.wav").unlink()
    (folder / "Voice.mp3").write_bytes(first)
    subprocess.run([*command, "--prefer-lossless", f"={folder}"], check=True,
                   capture_output=True)
    assert target.read_bytes() == second


@pytest.mark.parametrize(("slug", "folder_name", "logical", "short", "stem"), [
    ("loewenzahn-8", "DATA/TAMAGOTC/SOUNDS", "tamagotchi/sounds", "tamagotc/sounds", "S5_K_6"),
    ("loewenzahn-7", "DATA/LEXIKON/SOUNDS/KOMPONIS", "lexikon/sounds/komponisten",
     "lexikon/sounds/komponis", "BACH"),
    ("loewenzahn-advent", "DATA/SOUNDS/SCHORNST", "sounds/schornstein",
     "sounds/schornst", "KILLSTAR"),
])
def test_authored_long_and_disc_short_sound_folders_are_both_packaged(
        tmp_path, slug, folder_name, logical, short, stem):
    from director64.project import GameSpec, converter

    game = GameSpec.load(slug)
    aliases = [s for s in game.data["convert"]["streams"] if s.endswith("=" + folder_name)]
    folder = tmp_path / folder_name
    folder.mkdir(parents=True)
    pcm = bytes([64]) * 100
    fmt = struct.pack("<HHIIHH", 1, 1, 1000, 1000, 1, 8)
    wave = (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt "
            + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(pcm)) + pcm)
    (folder / (stem + ".WAV")).write_bytes(wave)
    output = tmp_path / "streams.bin"
    sources = tmp_path / "sources.json"
    result = subprocess.run([
        *converter(), "director", "stream-pack", output, f"--source-map={sources}",
        *(name.split("=", 1)[0] + "=" + str(tmp_path / name.split("=", 1)[1])
          for name in aliases),
    ], text=True, capture_output=True, check=True)
    index = json.loads(result.stdout)
    assert set(index) == {short + "/" + stem.lower()}
    selected = {k: [str(Path(p).relative_to(tmp_path)) for p in paths]
                for k, paths in json.loads(sources.read_text()).items()}
    source_aliases = {logical + "/" + stem + ".WAV":
                      folder_name.removeprefix("DATA/") + "/" + stem + ".WAV"}
    packs = {k: (wave, value[2:]) for k, value in index.items()}
    names = convert.verified_stream_aliases(
        packs, selected, source_aliases, aliases, game.data["port"]["media_root"])
    convert.write_pack(tmp_path, "aliased-streams", packs, names)
    aliased = json.loads((tmp_path / "aliased-streams.json").read_text())
    assert set(aliased) == {short + "/" + stem.lower(), logical + "/" + stem.lower()}
    assert len({tuple(value) for value in aliased.values()}) == 1
    assert {value[2] for value in aliased.values()} == {100}
    assert (tmp_path / "aliased-streams.bin").read_bytes() == wave


def joliet_stream_disc(short_names, long_names, payload, *,
                       short_folders=None, long_folders=None):
    """Synthetic primary/Joliet trees sharing the same sound extents."""
    from director64.iso import SECTOR

    def dual(n, width):
        return n.to_bytes(width, "little") + n.to_bytes(width, "big")

    def record(name, sector, length, flags=0):
        data = bytearray(33 + len(name) + (len(name) % 2 == 0))
        data[0], data[25], data[32] = len(data), flags, len(name)
        data[2:10], data[10:18] = dual(sector, 4), dual(length, 4)
        data[28:32] = dual(1, 2)
        data[33:33 + len(name)] = name
        return data

    disc = bytearray(128 * SECTOR)
    for descriptor, start, folders, names, unicode in (
        (16, 20, short_folders or ["DATA", "LEXIKON", "SOUNDS"], short_names, False),
        (17, 40, long_folders or ["DATA", "Lexikon", "Sounds"], long_names, True),
    ):
        at = descriptor * SECTOR
        disc[at:at + 7] = bytes([2 if unicode else 1]) + b"CD001\x01"
        disc[at + 80:at + 88] = dual(128, 4)
        disc[at + 128:at + 132] = dual(SECTOR, 2)
        root = record(b"\0", start, SECTOR, 2)
        disc[at + 156:at + 156 + len(root)] = root
        if unicode:
            disc[at + 88:at + 91] = b"%/E"
        for depth in range(len(folders) + 1):
            entries = []
            if depth < len(folders):
                name = folders[depth].encode("utf-16-be" if unicode else "ascii")
                entries.append(record(name, start + depth + 1, SECTOR, 2))
            else:
                for i, filename in enumerate(names):
                    name = filename.encode("utf-16-be" if unicode else "ascii")
                    entries.append(record(name, 100 + i, len(payload)))
            directory = b"".join(entries)
            begin = (start + depth) * SECTOR
            disc[begin:begin + len(directory)] = directory
    disc[18 * SECTOR:18 * SECTOR + 7] = b"\xffCD001\x01"
    for i in range(len(short_names)):
        disc[(100 + i) * SECTOR:(100 + i) * SECTOR + len(payload)] = payload
    return disc


def test_verified_joliet_stream_names_preserve_underscores_and_selected_namespaces(tmp_path):
    from director64 import iso
    from director64.project import GameSpec, converter

    pcm = bytes([32, 224]) * 80
    fmt = struct.pack("<HHIIHH", 1, 1, 1000, 1000, 1, 8)
    wave = (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt "
            + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(pcm)) + pcm)
    disc = tmp_path / "synthetic.iso"
    disc.write_bytes(joliet_stream_disc(
        ["L2_01_S1.WAV;1", "L4_02S15.WAV;1"],
        ["L2_01_S_1.wav", "L4_02_S15.wav"], wave))
    extracted = tmp_path / "extracted"
    iso.extract(disc, extracted, expected_sha256=iso.file_sha256(disc))
    game = GameSpec.load("loewenzahn-8")
    policy = convert.prepared_policy(SimpleNamespace(
        media=disc, source={"kind": "iso9660"}, directory=game.directory,
        data=game.data), tmp_path, extracted)
    aliases = json.loads(policy.read_text())["source_path_aliases"]
    assert aliases["Lexikon/Sounds/L2_01_S_1.wav"] == "LEXIKON/SOUNDS/L2_01_S1.WAV"
    spec = next(s for s in game.data["convert"]["streams"] if s.startswith("LEXIKON/SOUNDS="))
    logical, physical = spec.split("=")
    output, source_map = tmp_path / "input.bin", tmp_path / "sources.json"
    result = subprocess.run([
        *converter(), "director", "stream-pack", output, f"--source-map={source_map}",
        f"{logical}={extracted / physical}",
    ], capture_output=True, text=True, check=True)
    index, data = json.loads(result.stdout), output.read_bytes()
    packs = {name: (data[at:at + size], rest) for name, (at, size, *rest) in index.items()}
    sources = {name: [Path(p).relative_to(extracted).as_posix() for p in paths]
               for name, paths in json.loads(source_map.read_text()).items()}
    names = convert.verified_stream_aliases(packs, sources, aliases, [spec], "DATA")
    convert.write_pack(tmp_path, "streams", packs, names)
    stored = json.loads((tmp_path / "streams.json").read_text())
    assert stored["lexikon/sounds/l2_01_s_1"] == stored["lexikon/sounds/l2_01_s1"]
    assert stored["lexikon/sounds/l4_02_s15"] == stored["lexikon/sounds/l4_02s15"]
    assert (tmp_path / "streams.bin").read_bytes() == data
    assert not convert.verified_stream_aliases(packs, sources, aliases, [], "DATA")
    assert not convert.verified_stream_aliases(packs, {}, aliases, [spec], "DATA")
    assert convert.verified_stream_aliases(packs, sources, aliases, [spec], "/DATA/") == names
    root_game = SimpleNamespace(media=disc, source={"kind": "iso9660"},
        directory=game.directory, data={"port": {"media_root": "."}})
    root_policy = json.loads(convert.prepared_policy(root_game, tmp_path, extracted).read_text())
    assert root_policy["source_path_aliases"]["DATA/Lexikon/Sounds/L2_01_S_1.wav"] == (
        "DATA/LEXIKON/SOUNDS/L2_01_S1.WAV")


def test_stream_aliases_cannot_cross_folders_or_replace_other_sounds():
    streams = {"voices/a": (b"voice-a", [100]), "other/b": (b"voice-b", [100])}
    sources = {"voices/a": ["DATA/VOICES/A.WAV"], "other/b": ["DATA/OTHER/B.WAV"]}
    specs = ["voices=DATA/VOICES", "other=DATA/OTHER"]
    aliases = {"Voices/Long_A.wav": "VOICES/A.WAV", "Other/Long_B.wav": "OTHER/B.WAV"}
    result = convert.verified_stream_aliases(streams, sources, aliases, specs, "DATA")
    assert result == {"voices/long_a": "voices/a", "other/long_b": "other/b"}
    with pytest.raises(ValueError, match="conflicting verified streamed sound alias"):
        convert.verified_stream_aliases(streams, sources,
            {**aliases, "Voices/Long_A.WAV": "OTHER/B.WAV"}, specs, "DATA")
    with pytest.raises(ValueError, match="conflicting verified streamed sound alias"):
        convert.verified_stream_aliases(streams, sources,
            {"voices/a.wav": "OTHER/B.WAV"}, specs, "DATA")


def test_mixed_verified_directory_components_and_filename_share_one_stream(tmp_path):
    from itertools import product

    from director64 import iso
    from director64.project import GameSpec, converter

    pcm = bytes([32, 224]) * 80
    fmt = struct.pack("<HHIIHH", 1, 1, 1000, 1000, 1, 8)
    wave = (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt "
            + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(pcm)) + pcm)
    disc = tmp_path / "synthetic.iso"
    disc.write_bytes(joliet_stream_disc(["S2_INTR1.WAV;1"], ["S2_Intro_Take_1.wav"], wave,
        short_folders=["DATA", "WIMMELBI", "SOUNDS", "INTROOUT"],
        long_folders=["DATA", "Wimmelbild", "sounds", "IntroOutro"]))
    extracted = tmp_path / "extracted"
    iso.extract(disc, extracted, expected_sha256=iso.file_sha256(disc))
    game = GameSpec.load("loewenzahn-8")
    policy = convert.prepared_policy(SimpleNamespace(media=disc,
        source={"kind": "iso9660"}, directory=game.directory, data=game.data), tmp_path, extracted)
    aliases = json.loads(policy.read_text())["source_path_aliases"]
    spec = next(s for s in game.data["convert"]["streams"]
                if s.startswith("WIMMELBI/SOUNDS/INTROOUT="))
    logical, physical = spec.split("=")
    source_map = tmp_path / "sources.json"
    output = tmp_path / "streams.bin"
    result = subprocess.run([*converter(), "director", "stream-pack", output,
        f"--source-map={source_map}", f"{logical}={extracted / physical}"],
        text=True, capture_output=True, check=True)
    index = json.loads(result.stdout)
    sources = {name: [Path(p).relative_to(extracted).as_posix() for p in paths]
               for name, paths in json.loads(source_map.read_text()).items()}
    packs = {name: (wave, entry[2:]) for name, entry in index.items()}
    names = convert.verified_stream_aliases(packs, sources, aliases, [spec], "DATA")
    convert.write_pack(tmp_path, "streams", packs, names)
    stored = json.loads((tmp_path / "streams.json").read_text())
    expected = {"/".join(parts) for parts in product(
        ["wimmelbi", "wimmelbild"], ["sounds"], ["introout", "introoutro"],
        ["s2_intr1", "s2_intro_take_1"])}
    assert set(stored) == expected and len(stored) == 8
    assert len({tuple(entry) for entry in stored.values()}) == 1
    assert (tmp_path / "streams.bin").read_bytes() == wave


def test_mixed_stream_names_are_bounded_after_case_normalization():
    parts = ["folder"] * 12
    short = "/".join(parts + ["VOICE.WAV"])
    long = "/".join([p.upper() for p in parts] + ["voice.wav"])
    physical = "/".join(["DATA", *parts])
    key = "/".join([*parts, "voice"])
    assert not convert.verified_stream_aliases({key: (b"audio", [100])},
        {key: ["DATA/" + short]}, {long: short}, [key.rsplit("/", 1)[0] + "=" + physical], "DATA")
    short = "/".join(["a"] * 9 + ["VOICE.WAV"])
    long = "/".join(["long_a"] * 9 + ["voice.wav"])
    key = short.removesuffix(".WAV").lower()
    with pytest.raises(ValueError, match="more than 256 mixed-name variants"):
        convert.verified_stream_aliases({key: (b"audio", [100])},
            {key: ["DATA/" + short]}, {long: short},
            [key.rsplit("/", 1)[0] + "=DATA/" + short.rsplit("/", 1)[0]], "DATA")


def test_stream_aliases_use_actual_preferred_sources_and_ignore_non_sounds(tmp_path):
    from director64.project import converter

    folder = tmp_path / "sounds"
    folder.mkdir()
    pcm = bytes([32]) * 100
    fmt = struct.pack("<HHIIHH", 1, 1, 1000, 1000, 1, 8)
    wave = (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt "
            + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(pcm)) + pcm)
    (folder / "VOICE.WAV").write_bytes(wave)
    (folder / "VOICE.AIF").write_bytes(wave)
    (folder / "VOICE.BMP").write_bytes(b"bitmap")
    sources = tmp_path / "sources.json"
    subprocess.run([*converter(), "director", "stream-pack", tmp_path / "streams.bin",
                    "--prefer-wav", f"--source-map={sources}", f"voices={folder}"],
                   capture_output=True, text=True, check=True)
    assert json.loads(sources.read_text()) == {"voices/voice": [str(folder / "VOICE.WAV")]}
    result = convert.verified_stream_aliases({"voices/voice": (wave, [100])},
        {"voices/voice": ["DATA/VOICES/VOICE.WAV"]},
        {"Voices/Correct.wav": "VOICES/VOICE.WAV",
         "Voices/Wrong.wav": "VOICES/VOICE.AIF",
         "Voices/Bitmap.wav": "VOICES/VOICE.BMP"}, ["voices=DATA/VOICES"], "DATA")
    assert result == {"voices/correct": "voices/voice"}
    duplicate = tmp_path / "duplicate"
    duplicate.mkdir()
    (duplicate / "VOICE.WAV").write_bytes(wave)
    subprocess.run([*converter(), "director", "stream-pack", tmp_path / "streams.bin",
                    "--prefer-wav", f"--source-map={sources}", f"voices={folder}",
                    f"voices={duplicate}"], capture_output=True, text=True, check=True)
    assert set(json.loads(sources.read_text())["voices/voice"]) == {
        str(folder / "VOICE.WAV"), str(duplicate / "VOICE.WAV")}


@pytest.mark.parametrize("root", ["", ".", "/"])
def test_stream_aliases_support_disc_root_namespaces(root):
    assert convert.verified_stream_aliases({"voice": (b"audio", [100])},
        {"voice": ["VOICE.WAV"]}, {"Long_Voice.wav": "VOICE.WAV"}, ["="], root) == {
            "long_voice": "voice"}


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


# ---- The recovered program ----


def handler_fixture():
    source = {
        "files": [
            {
                "name": "SHARED.CXT",
                "members": [{"id": "member-hash", "library_id": 1024, "member_id": 3}],
                "libraries": [{"library_id": 1024, "name": "External"}],
                "scripts": [{"member_id": "member-hash", "handlers": [{"name": "new"}]}],
            }
        ],
    }
    program = {
        "handlers": parse_movie(
            "-- cast: External; member: 3; type: MovieScript; name: actor\n"
            "property position\non new me\n  position = point(1, 2)\n  return me\nend\n",
            "SHARED.CXT",
        )
    }
    return source, program


def test_script_identity_agreement_accepts_native_object_lowering():
    source, program = handler_fixture()
    check_handlers(source, program)


@pytest.mark.parametrize(
    "field,value",
    [
        ("cast", "Internal"),
        ("member", 4),
        ("name", "start"),
        ("movie", "OTHER.CXT"),
    ],
)
def test_same_handler_count_cannot_hide_changed_bindings(field, value):
    source, program = handler_fixture()
    modified = deepcopy(program)
    modified["handlers"][0][field] = value
    with pytest.raises(ValueError, match="identities"):
        check_handlers(source, modified)


def test_missing_or_duplicated_handlers_are_rejected():
    source, program = handler_fixture()
    for handlers in ([], program["handlers"] * 2):
        with pytest.raises(ValueError, match="identities"):
            check_handlers(source, {"handlers": handlers})


@pytest.mark.parametrize(('kind', 'raw'), [
    ('audio', b'RIFF' + bytes(40)), ('streams', b'RIFF' + bytes(40)),
    ('images', b'FDI1' + bytes(100)), ('video', b'raw pixels'),
])
def test_shipping_packs_refuse_raw_media(kind, raw):
    with pytest.raises(ValueError, match='refusing to ship raw'):
        convert.validate_encoded_media(kind, 'fixture', raw)

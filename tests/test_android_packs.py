"""Android compaction preserves payloads, names, timing and descriptor offsets."""

import json
import shutil
import struct
import subprocess
import zlib

import pytest

from director64.android_packs import payload_digest, stage_packs
from director64.convert import PACKS, deflated, opus


def ogg(serial=1, *, packet=b"encoded audio", granule=960, tag=b"source frames=441"):
    def page(sequence, flags, granule, packet):
        header = struct.pack("<4sBBQIIIB", b"OggS", 0, flags, granule, serial,
                             sequence, 0, 1) + bytes([len(packet)])
        # Ogg's non-reflected CRC32, not ZIP/zlib's reflected CRC32.
        data, crc = bytearray(header + packet), 0
        for byte in data:
            crc ^= byte << 24
            for _ in range(8):
                crc = ((crc << 1) ^ (0x04c11db7 if crc & 0x80000000 else 0)) & 0xffffffff
        struct.pack_into("<I", data, 22, crc)
        return bytes(data)

    head = b"OpusHead" + struct.pack("<BBHIhB", 1, 1, 312, 44100, 0, 0)
    return (page(0, 2, 0, head) + page(1, 0, 0, b"OpusTags" + tag)
            + page(2, 4, granule, packet))


def packs(tmp_path, records):
    source, target = tmp_path / "converted", tmp_path / "assets/game"
    source.mkdir()
    target.mkdir(parents=True)
    for kind in PACKS:
        blob, index = bytearray(), {}
        for name, (data, *extra) in records.get(kind, {}).items():
            index[name] = [len(blob), len(data), *extra]
            blob.extend(data)
        (source / f"{kind}.bin").write_bytes(blob)
        (source / f"{kind}.json").write_text(json.dumps(index))
    return source, target


@pytest.fixture(scope="module")
def pack_tool(tmp_path_factory):
    tool = tmp_path_factory.mktemp("android-packs") / "pack-tool"
    subprocess.run(["cc", "-std=c17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-Itests/native/android_stubs", "tests/native/android_pack_tool.c",
                    "platforms/android/jni/json.c", "-lz", "-o", tool], check=True)
    return tool


def test_shared_audio_and_aliases_retain_names_metadata_and_encoded_bytes(tmp_path, pack_tool):
    first, second = ogg(7), ogg(99)
    image = deflated(b"FDI1" + b"pixels" * 50)
    source, target = packs(tmp_path, {
        "audio": {"member.wav64": [first]},
        "streams": {"voices/one": [second, 10], "voices/alias": [second, 20],
                    "different": [ogg(packet=b"another sound"), 30]},
        "images": {"first": [image], "alias": [image]},
        "video": {"first": [b"encoded video"], "alias": [b"encoded video"]},
    })
    original = {p.name: p.read_bytes() for p in source.iterdir()}
    stats = stage_packs(source, target)
    assert {p.name: p.read_bytes() for p in source.iterdir()} == original
    assert stats["saved_bytes"] == len(second) * 2 + len(image) + len(b"encoded video")
    assert not (target / "streams.bin").exists()
    audio = json.loads((target / "audio.json").read_text())
    streams = json.loads((target / "streams.json").read_text())
    assert audio["blob"] == streams["blob"] == "audio"
    assert streams["entries"]["voices/one"] == [0, len(first), 10]
    assert streams["entries"]["voices/alias"] == [0, len(first), 20]
    assert (target / "audio.bin").read_bytes().startswith(first)
    for kind, names in [("audio", ["member.wav64"]),
                        ("streams", ["voices/one", "voices/alias", "different"]),
                        ("images", ["first", "alias"]), ("data", [])]:
        result = json.loads(subprocess.check_output([pack_tool, target.parent, kind, *names]))
        assert result["count"] == len(json.loads((target / f"{kind}.json").read_text())["entries"])
        if kind == "streams":
            assert result["entries"][0] == [0, len(first), 10, zlib.crc32(first), len(first)]
            assert result["entries"][1][2] == 20
        if kind == "images":
            assert result["entries"][0][4] == len(b"FDI1" + b"pixels" * 50)


def test_ogg_equivalence_keeps_packets_timing_and_source_tags():
    digest = payload_digest(ogg(1), audio=True)
    assert digest == payload_digest(ogg(2), audio=True)
    assert payload_digest(ogg(1), audio=False) != payload_digest(ogg(2), audio=False)
    for changed in [ogg(packet=b"changed audio"), ogg(granule=959), ogg(tag=b"other source")]:
        assert payload_digest(changed, audio=True) != digest
    # Concatenated logical streams cannot discard their stream identities.
    assert payload_digest(ogg(1) + ogg(2), audio=True) != payload_digest(
        ogg(3) + ogg(4), audio=True)
    for truncated in [ogg()[:20], ogg()[:-1]]:
        with pytest.raises(ValueError, match="Ogg"):
            payload_digest(truncated, audio=True)


def test_header_only_empty_exports_keep_their_bytes_and_zero_duration(tmp_path, pack_tool):
    def headers(serial):
        whole = ogg(serial)
        return whole[:whole.rfind(b"OggS")]

    first, second = headers(1), headers(2)
    assert payload_digest(first, audio=True) == payload_digest(first, audio=False)
    assert payload_digest(first, audio=True) != payload_digest(second, audio=True)
    source, target = packs(tmp_path, {"streams": {"empty/one": [first, 0],
                                                "empty/two": [second, 0]}})
    assert stage_packs(source, target)["saved_bytes"] == 0
    assert (target / "audio.bin").read_bytes() == first + second
    result = json.loads(subprocess.check_output(
        [pack_tool, target.parent, "streams", "empty/one", "empty/two"]))
    assert result["entries"][0] == [0, len(first), 0, zlib.crc32(first), len(first)]
    assert result["entries"][1] == [len(first), len(second), 0, zlib.crc32(second), len(second)]


@pytest.mark.parametrize("entry", [[-1, 3], [0, 999], [0, -1], [0, 1.5], [0, 1, -1]])
def test_staging_rejects_invalid_ranges_and_metadata(tmp_path, entry):
    source, target = packs(tmp_path, {"images": {"fixture": [b"pixels"]}})
    (source / "images.json").write_text(json.dumps({"fixture": entry}))
    with pytest.raises(ValueError, match="images/fixture: invalid pack range"):
        stage_packs(source, target)


@pytest.mark.parametrize("change,error", [
    ({"version": 2}, "version 1"), ({"blob": "../audio"}, "version 1"),
    ({"blob": "video"}, "version 1"),
    ({"entries": {"cue": [-1, 3]}}, "version 1"),
    ({"entries": {"cue": [0, 1.5]}}, "version 1"),
    ({"entries": {"cue": [0, 1, -1]}}, "version 1"),
    ({"entries": {"cue": [0, 999]}}, "shorter"),
])
def test_native_loader_rejects_invalid_indexes(tmp_path, pack_tool, change, error):
    source, target = packs(tmp_path, {"audio": {"cue": [ogg()]}})
    stage_packs(source, target)
    path = target / "audio.json"
    index = json.loads(path.read_text()) | change
    path.write_text(json.dumps(index))
    result = subprocess.run([pack_tool, target.parent, "audio"], capture_output=True, text=True)
    assert result.returncode == 2 and error in result.stderr


def test_native_loader_requires_uncompressed_blob(tmp_path, pack_tool):
    source, target = packs(tmp_path, {"streams": {"cue": [ogg(), 10]}})
    stage_packs(source, target)
    (target / "audio.bin.compressed").touch()
    result = subprocess.run([pack_tool, target.parent, "streams"], capture_output=True, text=True)
    assert result.returncode == 2 and "audio.bin is compressed" in result.stderr


@pytest.mark.skipif(not shutil.which("ffmpeg"), reason="needs ffmpeg")
def test_reused_real_opus_decodes_identically(tmp_path):
    samples = struct.pack("<2000h", *(i % 200 * 100 - 10000 for i in range(2000)))
    wav = (b"RIFF" + struct.pack("<I", 36 + len(samples)) + b"WAVEfmt "
           + struct.pack("<IHHIIHH", 16, 1, 1, 22050, 44100, 2, 16)
           + b"data" + struct.pack("<I", len(samples)) + samples)
    first, second = opus(wav), opus(wav)
    assert payload_digest(first, audio=True) == payload_digest(second, audio=True)
    source, target = packs(tmp_path, {"audio": {"cue": [first]},
                                      "streams": {"voice": [second, 91]}})
    stats = stage_packs(source, target)
    assert stats["saved_bytes"] == len(second)
    stored = (target / "audio.bin").read_bytes()
    assert stored == first
    decoded = [subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-f", "ogg", "-i",
                               "pipe:0", "-f", "s16le", "pipe:1"], input=data,
                              capture_output=True, check=True).stdout
               for data in [first, second, stored]]
    assert decoded[0] == decoded[1] == decoded[2] and decoded[0]

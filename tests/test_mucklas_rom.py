import hashlib
import io
import json
import os
import struct
import zipfile
from dataclasses import replace

import pytest

from director64.project import GameSpec
from director64.rom import RomError
from director64.rom_inputs import record as record_rom_inputs


def overlay_dfs():
    """Small real DFS directory containing the validator's 49 required overlays."""
    data = bytearray(44 + 49 * 64)
    data[:8] = bytes.fromhex("deadbeefffffffff")
    data[12:25] = b"DragonFS 2.1\0"
    struct.pack_into(">III", data, 26, 0, 1 << 28, 44)
    data[38:43] = b"code\0"
    files = {}
    for index in range(49):
        name = f"m{index}_dxr.dso"
        payload = f"synthetic overlay {index}".encode()
        offset = 44 + index * 64
        next_offset = offset + 64 if index < 48 else 0
        struct.pack_into(">III", data, offset, next_offset, len(payload), len(data))
        data[offset + 12 : offset + 13 + len(name)] = name.encode() + b"\0"
        data.extend(payload)
        files["code/" + name] = payload
    return bytes(data), files


@pytest.mark.parametrize("profile", ["release", "probe"])
def test_mucklas_rom_profiles_apply_manifest_budget(tmp_path, monkeypatch, rom_factory, profile):
    for key in ("DIRECTOR64_GAME", "DIRECTOR64_SOURCE", "DIRECTOR64_WORK_DIR"):
        monkeypatch.setenv(key, os.environ.get(key, ""))
    selected = GameSpec.load("findus-mucklas")
    validation = selected.host("full_validation")
    game = replace(selected, root=tmp_path)
    monkeypatch.setattr(validation, "selected_game", lambda: game)
    target = game.slug + ("-probe" if profile == "probe" else "")
    build = game.work / "n64" / profile
    build.mkdir(parents=True)
    rom_path = build.parent / f"{target}.z64"
    rom = bytearray(
        rom_factory(title="Findus Mucklas", controller_count=4, code_bytes=52 * 1024 * 1024 - 16384)
    )
    with zipfile.ZipFile(io.BytesIO(rom)) as archive:
        (build / "metadata.ini").write_bytes(archive.read("metadata.ini"))
    dfs, files = overlay_dfs()
    rom[4096 : 4096 + len(dfs)] = dfs
    rom_path.write_bytes(rom)
    record_rom_inputs(game, profile, rom_path)
    (build / f"{target}.dfs").write_bytes(dfs)
    for name, payload in files.items():
        path = game.work / "filesystem" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
    records = {
        "director/packed.json": {
            "toolchain": {"image": "fixture", "revision": "test"},
            "files": {},
        },
        "director/model.json": {},
        "aot/c/manifest.json": {
            "movies": [
                {"movie": f"M{i}.DXR", "native_functions": 0, "handlers": 0} for i in range(49)
            ]
        },
    }
    for name, value in records.items():
        path = game.work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value))
    monkeypatch.setattr(
        validation,
        "inspect_image",
        lambda *_: {
            "Id": "fixture",
            "Config": {"Labels": {"org.director64.libdragon-commit": "test"}},
        },
    )

    def output(command, **_):
        if command[-2] == "mips64-elf-nm":
            return (
                "80001000 T director_replay_sample\n" if profile == "probe" else "80001000 T tick\n"
            )
        assert command[-2] == "mips64-elf-size"
        return "text data bss\n10 20 30\n"

    monkeypatch.setattr(validation.subprocess, "check_output", output)
    report = validation.validate_rom(tmp_path, profile)
    assert report["status"] == "passing"
    assert report["profile"] == profile
    assert report["rom_bytes"] == 52 * 1024 * 1024
    assert report["rom_budget_bytes"] == 56 * 1024 * 1024
    assert report["rom_capacity_bytes"] == 64 * 1024 * 1024
    assert report["rom_sha256"] == hashlib.sha256(rom).hexdigest()
    assert report["overlays"] == 49
    for size, message in [(57, "release 56 MiB"), (65, "hard 64 MiB")]:
        with rom_path.open("r+b") as stream:
            stream.truncate(size * 1024 * 1024)
        # A build records its ROM before validation rejects the size.
        record_rom_inputs(game, profile, rom_path)
        with pytest.raises(RomError, match=message):
            validation.validate_rom(tmp_path, profile)

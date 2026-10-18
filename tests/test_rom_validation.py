"""Synthetic experimental-port ROMs exercise the shared validator end to end."""
import hashlib
import io
import json
import struct
import zipfile
from dataclasses import replace

import pytest

from director64 import cli, rom_validation
from director64.project import GameSpec
from director64.rom import RomError
from director64.rom_inputs import record


def dragonfs(files):
    tree = {}
    for path, contents in files.items():
        parts = path.split("/")
        node = tree
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        node[parts[-1]] = contents
    data = bytearray(26)
    data[:8] = bytes.fromhex("deadbeefffffffff")
    data[12:25] = b"DragonFS 2.1\0"

    def directory(node):
        entries = []
        for name, item in node.items():
            at = len(data)
            encoded = name.encode() + b"\0"
            data.extend(bytes(12) + encoded)
            data.extend(bytes(len(data) % 2))
            entries.append((at, item))
        for n, (at, item) in enumerate(entries):
            pointer = len(data)
            if isinstance(item, dict):
                directory(item)
                flags = 1 << 28
            else:
                data.extend(item)
                data.extend(bytes(len(data) % 2))
                flags = len(item)
            struct.pack_into(">III", data, at, entries[n + 1][0] if n + 1 < len(entries) else 0,
                             flags, pointer)
    directory(tree)
    return bytes(data)


@pytest.fixture
def fixture(tmp_path, monkeypatch, rom_factory):
    game = replace(GameSpec.load("findus-birthday"), root=tmp_path)
    assert "host_package" not in game.data["port"]
    build = game.work / "n64/release"
    build.mkdir(parents=True)
    source = game.work / "n64/findus-birthday.z64"
    (game.work / "build-inputs.json").write_text("{}")
    files = {"code/start_dxr.dso": b"synthetic overlay", "font.bin": b"synthetic font"}
    for name, content in files.items():
        path = game.work / "filesystem" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    packed = {"toolchain": {"image": "sdk-id", "revision": "pinned-commit"},
              "files": {"font.bin": {"sha256": hashlib.sha256(files["font.bin"]).hexdigest()}}}
    aot = {"movies": [{"movie": "START.DXR", "handlers": 1, "native_functions": 0,
                       "bytecode_functions": 1, "code_kind": "bytecode"}]}
    sdk = {"Id": "sdk-id", "Config": {"Labels": {
        "org.director64.libdragon-commit": "pinned-commit"}}}
    reports = {"nm": "80000000 T dg_tick\n00200000 A director64_runtime_bytes\n"
                     "002AC000 A director64_fixed_reserve\n",
               "size": "text data bss dec hex file\n1 2 3 6 6 x\n"}
    monkeypatch.setattr(rom_validation, "inspect_image", lambda *_: sdk)
    monkeypatch.setattr(rom_validation, "container_engine", lambda: "docker")

    def tool(command, *, text):
        assert text and "--network=none" in command and command[-1].endswith(".elf")
        return reports["nm" if command[-2].endswith("-nm") else "size"]
    monkeypatch.setattr(rom_validation.subprocess, "check_output", tool)

    def write_json(name, value):
        path = game.work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value))

    def write_rom(contents=files, copies=1):
        raw = bytearray(rom_factory(title=game.data["port"]["rom_title"], controller_count=4))
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            (build / "metadata.ini").write_bytes(archive.read("metadata.ini"))
        dfs = dragonfs(contents)
        (build / "findus-birthday.dfs").write_bytes(dfs)
        raw[4096:4096 + len(dfs) * copies] = dfs * copies
        source.write_bytes(raw)
        record(game, "release", source)
    write_json("director/packed.json", packed)
    write_json("director/model.json", {"movies": [{"name": "START.DXR"}]})
    write_json("aot/c/manifest.json", aot)
    write_rom()
    return game, files, packed, aot, sdk, reports, write_json, write_rom


def test_experimental_port_checks_without_a_host_adapter(fixture):
    game, *_ = fixture
    result = rom_validation.validate_rom(game, "release")
    assert result["status"] == "passing"
    assert result["overlays"] == 1 and result["native_functions"] == 0
    assert result["bytecode_functions"] == 1 and result["static_bytes"] == 6
    assert not result["hardware_qualified"]
    assert "gameplay and performance are not qualified" in result["validation_scope"]
    assert result["memory_reservation"]["unreserved_bytes"] > 0


@pytest.mark.parametrize("change,message", [
    ("missing", "filesystem differs"), ("extra", "filesystem differs"),
    ("bytes", "embedded resource differs"), ("duplicate", "exactly once"),
    ("hash", "asset hash mismatch"), ("sdk", "pinned SDK"),
    ("replay", "replay symbols"), ("static", "static memory budget"),
    ("size-report", "section-size report"), ("missing-movie", "converted movies"),
    ("collision", "colliding movie"), ("stale-assets", "ROM is stale"),
    ("memory-missing", "lacks N64 memory reservation"),
    ("memory-excess", "reservations exceed RDRAM"),
])
def test_corruption_and_missing_build_contracts_fail_loudly(fixture, change, message):
    game, files, packed, aot, sdk, reports, write_json, write_rom = fixture
    if change == "missing":
        write_rom({"font.bin": files["font.bin"]})
    elif change == "extra":
        write_rom(files | {"unexpected": b"x"})
    elif change == "bytes":
        write_rom(files | {"font.bin": b"wrong"})
    elif change == "duplicate":
        write_rom(copies=2)
    elif change == "hash":
        packed["files"]["font.bin"]["sha256"] = "0" * 64
        write_json("director/packed.json", packed)
    elif change == "sdk":
        sdk["Config"]["Labels"]["org.director64.libdragon-commit"] = "other"
    elif change == "replay":
        reports["nm"] = "80000000 T replay_step\n"
    elif change == "static":
        reports["size"] = "2097152 1 0 0 0 x\n"
    elif change == "memory-missing":
        reports["nm"] = "80000000 T dg_tick\n"
    elif change == "memory-excess":
        reports["nm"] = reports["nm"].replace("00200000 A", "00600000 A")
    elif change == "size-report":
        reports["size"] = "not an ELF size report\n"
    elif change == "missing-movie":
        write_json("director/model.json", {"movies": [{"name": "OTHER.DXR"}]})
    elif change == "collision":
        aot["movies"].append(aot["movies"][0] | {"movie": "start.dxr"})
        write_json("aot/c/manifest.json", aot)
    elif change == "stale-assets":
        (game.work / "build-inputs.json").write_text('{"changed": true}')
    with pytest.raises((RomError, ValueError), match=message):
        rom_validation.validate_rom(game, "release")


def test_cli_build_uses_generic_validation_when_no_adapter_exists(fixture, monkeypatch):
    from director64 import build_inputs, rom_metadata, toolchain

    game, _, _, _, sdk, *_ = fixture
    monkeypatch.setattr(build_inputs, "verify", lambda *_: None)
    monkeypatch.setattr(rom_metadata, "prepare", lambda *_: None)
    monkeypatch.setattr(toolchain, "record_build_context", lambda *_: None)
    monkeypatch.setattr(toolchain, "container_engine", lambda: "docker")
    commands = []
    monkeypatch.setattr(cli, "run", lambda *args: commands.append(args))
    cli._build(game, sdk, "release", 1)
    assert commands and "make" in commands[0]
    result = json.loads((game.dist / "findus-birthday.json").read_text())
    assert result["overlays"] == 1 and result["status"] == "passing"

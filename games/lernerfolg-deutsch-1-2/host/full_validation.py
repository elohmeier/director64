"""Check the complete Lernerfolg Deutsch 1-2 ROM against its compiler and packed asset manifests."""

import hashlib
import json
import os
import re
import subprocess

from director64.project import selected_game
from director64.rom import (
    ROM_LIMIT,
    ROM_TARGET_MIB,
    RomError,
    dfs_files,
    packed_dfs_names,
    validate_header,
    validate_image_pack,
    validate_size,
)
from director64.rom_inputs import verify as verify_rom_inputs
from director64.toolchain import container_engine, inspect_image


def validate_rom(root, profile):
    if profile not in {"release", "probe"}:
        raise RomError("expected release or probe profile")
    game = selected_game()
    target = game.slug + ("-probe" if profile == "probe" else "")
    build = game.work / "n64" / profile
    source = game.work / "n64" / f"{target}.z64"
    verify_rom_inputs(game, profile, source)
    release_budget_mib = game.data["port"].get("rom_budget_mib", ROM_TARGET_MIB)
    validate_size(source.stat().st_size, release_budget_mib=release_budget_mib)
    rom = source.read_bytes()
    metadata = validate_header(
        rom,
        title=game.data["port"]["rom_title"],
        save_type=game.data["port"]["rom_save_type"],
        controller_count=game.data["port"].get("controller_count", 1),
        expected_metadata=(build / "metadata.ini").read_bytes(),
        release_budget_mib=release_budget_mib,
    )
    packed = json.loads((game.work / "director/packed.json").read_text())
    sdk = inspect_image(
        root, os.environ.get("DIRECTOR64_TOOLCHAIN_IMAGE", "director64-toolchain:local")
    )
    if (
        sdk["Id"] != packed["toolchain"]["image"]
        or sdk["Config"]["Labels"]["org.director64.libdragon-commit"]
        != packed["toolchain"]["revision"]
    ):
        raise RomError("ROM and assets must use the pinned SDK")
    container = [
        container_engine(),
        "run",
        "--rm",
        "--network=none",
        "-v",
        f"{root}:/workdir:ro",
        "-w",
        "/workdir",
        sdk["Id"],
    ]
    elf = str((build / f"{target}.elf").relative_to(root))
    symbols = subprocess.check_output([*container, "mips64-elf-nm", elf], text=True)
    if bool(re.search(r"\breplay_|\bdirector_replay_", symbols)) != (profile == "probe"):
        raise RomError("playable/replay code separation failed")
    sizes = subprocess.check_output([*container, "mips64-elf-size", elf], text=True)
    static = sum(map(int, sizes.splitlines()[-1].split()[:3]))
    # The budget covers the engine plus two statically reserved 656 KiB
    # full-stage plane slots (director_main plane_pool), which keep 640x480
    # stage planes clear of overlay fragmentation in the 8 MiB console.
    if static > 2 * 1024 * 1024 + 2 * 656 * 1024:
        raise RomError("permanent D10 engine exceeds static memory budget")
    dfs = (build / f"{target}.dfs").read_bytes()
    if rom.count(dfs) != 1:
        raise RomError("DFS must be embedded exactly once")
    files = dfs_files(dfs)
    aot = json.loads((game.work / "aot/c/manifest.json").read_text())
    code = {"code/" + m["movie"].replace(".", "_").lower() + ".dso" for m in aot["movies"]}
    symbols = {
        "code/" + m["movie"].replace(".", "_").lower() + ".dso.sym"
        for m in aot["movies"]
        if m["native_functions"]
    }
    speech = {
        f"{folder}/{p.name}"
        for folder in ("voc", "sfx")
        for p in (game.work / "filesystem" / folder).glob("*.wav64")
    }
    if (
        len([n for n in speech if n.startswith("voc/")]) != 3660
        or len([n for n in speech if n.startswith("sfx/")]) != 54
    ):
        raise RomError("streamed speech and effects sets are incomplete")
    databases = {
        "tests_db/" + str(p.relative_to(game.work / "filesystem/tests_db")).replace("\\", "/")
        for p in (game.work / "filesystem/tests_db").rglob("*")
        if p.is_file()
    }
    if len(databases) != 266:
        raise RomError("exercise task database set is incomplete")
    expected = packed_dfs_names(packed) | code | symbols | speech | databases
    if len(code) != 77 or set(files) != expected:
        raise RomError("ROM must contain all manifested assets and 77 native overlays")
    validate_image_pack(files, packed)
    for name, payload in files.items():
        if payload != (game.work / "filesystem" / name).read_bytes():
            raise RomError(f"embedded resource differs from build input: {name}")
        if (
            name in packed["files"]
            and hashlib.sha256(payload).hexdigest() != packed["files"][name]["sha256"]
        ):
            raise RomError(f"packed asset hash mismatch: {name}")
    model = json.loads((game.work / "director/model.json").read_text())
    return {
        "status": "passing",
        "profile": profile,
        "source_sha256": game.source["sha256"],
        "director_version": game.data["port"]["director_version"],
        "metadata": metadata,
        "rom_sha256": hashlib.sha256(rom).hexdigest(),
        "rom_bytes": len(rom),
        "rom_budget_bytes": release_budget_mib * 1024 * 1024,
        "rom_capacity_bytes": ROM_LIMIT,
        "dfs_sha256": hashlib.sha256(dfs).hexdigest(),
        "static_bytes": static,
        "overlays": len(code),
        "native_handlers": sum(movie["handlers"] for movie in aot["movies"]),
        "largest_overlay_bytes": max(map(lambda n: len(files[n]), code)),
        "assets": len(packed["files"]),
        "toolchain": packed["toolchain"],
        "approximations": model.get("approximations", []),
        "compatibility_dispositions": json.loads(
            (game.work / "aot/native-program.json").read_text()
        ).get("compatibility_fixes", []),
        "hardware_qualified": False,
        "original_projector_compared": False,
    }

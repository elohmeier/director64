"""Independent checks for full-game ROMs, captures, and the F64D save journal."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

from director64.captures import CaptureError, validate_media
from director64.project import selected_game, work_dir
from director64.rom import (
    RomError,
    dfs_files,
    packed_dfs_names,
    validate_header,
    validate_image_pack,
)
from director64.rom_inputs import verify as verify_rom_inputs
from director64.toolchain import container_engine, inspect_image


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def inspect_archive(data: bytes) -> dict:
    if len(data) != 131072:
        raise CaptureError("full-game save must be 128 KiB")
    records = []
    for slot in range(2):
        block = data[slot * 65536 : (slot + 1) * 65536]
        magic, version, generation, used, checksum, commit, count, reserved = struct.unpack_from(
            ">4s7I", block
        )
        if (magic, commit, count, reserved) != (b"F64D", 0x434F4D54, 9, 0) or version not in (1, 2):
            continue
        if not generation or checksum != zlib.crc32(block[:16] + bytes(4) + block[20:]):
            continue
        lengths = struct.unpack_from(">9I", block, 32)
        if max(lengths) > 16383 or sum(lengths) != used or used > 65408:
            continue
        # V2 reserves a 0/1 header word that a removed setting once wrote.
        legacy = struct.unpack_from(">I", block, 68)[0] if version == 2 else 0
        if legacy > 1 or any(block[72 if version == 2 else 68 : 128]):
            continue
        if any(block[128 + used :]) or 0 in block[128 : 128 + used]:
            continue
        position = 128
        files = []
        for length in lengths:
            payload = block[position : position + length]
            files.append({"bytes": length, "sha256": digest(payload)})
            position += length
        records.append(
            {
                "slot": slot,
                "generation": generation,
                "files": files,
                "version": version,
            }
        )
    if not records:
        raise CaptureError("no valid committed full-game save generation")
    return max(records, key=lambda r: r["generation"])


def validate_rom(root: Path, profile: str) -> dict:
    if profile not in {"release", "probe"}:
        raise RomError("expected full or full-probe profile")
    target = "findus-workshop" + ("-probe" if profile == "probe" else "")
    build = root / f"{work_dir()}/n64" / profile
    verify_rom_inputs(selected_game(), profile, root / work_dir() / "n64" / f"{target}.z64")
    rom = (root / work_dir() / "n64" / f"{target}.z64").read_bytes()
    metadata = validate_header(
        rom,
        title="Findus Workshop",
        save_type=0x50,
        controller_count=selected_game().data["port"].get("controller_count", 1),
        expected_metadata=(build / "metadata.ini").read_bytes(),
    )
    packed = json.loads((root / f"{work_dir()}/director/packed.json").read_text())
    image = os.environ.get("DIRECTOR64_TOOLCHAIN_IMAGE", "director64-toolchain:local")
    sdk = inspect_image(root, image)
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
    elf = f"{work_dir()}/n64/{profile}/{target}.elf"
    symbols = subprocess.check_output([*container, "mips64-elf-nm", elf], text=True)
    has_replay = bool(re.search(r"\breplay_|\bdirector_replay_", symbols))
    if has_replay != (profile == "probe"):
        raise RomError("playable/replay code separation failed")
    sizes = subprocess.check_output([*container, "mips64-elf-size", elf], text=True)
    static = sum(map(int, sizes.splitlines()[-1].split()[:3]))
    if static > 786432:
        raise RomError("permanent native engine exceeds static memory budget")
    dfs = (build / f"{target}.dfs").read_bytes()
    if rom.count(dfs) != 1:
        raise RomError("DFS must be embedded exactly once")
    files = dfs_files(dfs)
    aot = json.loads((root / f"{work_dir()}/aot/c/manifest.json").read_text())
    code_names = {"code/" + m["movie"].replace(".", "_").lower() + ".dso" for m in aot["movies"]}
    symbol_names = {name + ".sym" for name in code_names}
    if len(code_names) != 38 or set(files) != packed_dfs_names(packed) | code_names | symbol_names:
        raise RomError("full ROM must contain exactly all manifested assets and 38 overlays")
    validate_image_pack(files, packed)
    for name, payload in files.items():
        if payload != (root / f"{work_dir()}/filesystem" / name).read_bytes():
            raise RomError(f"embedded resource differs from build input: {name}")
        if name in packed["files"] and digest(payload) != packed["files"][name]["sha256"]:
            raise RomError(f"packed asset hash mismatch: {name}")
    if max(len(files[n]) for n in code_names) > 512 * 1024:
        raise RomError("movie overlay exceeds 512 KiB budget")
    return {
        "status": "passing",
        "profile": profile,
        "rom_sha256": digest(rom),
        "rom_bytes": len(rom),
        "metadata": metadata,
        "dfs_sha256": digest(dfs),
        "static_bytes": static,
        "overlays": len(code_names),
        "assets": len(packed["files"]),
        "toolchain": packed["toolchain"],
        "hardware_qualified": False,
    }


def snapshot(root: Path, run: Path) -> None:
    validation = validate_rom(root, "probe")
    names = [
        "platforms/n64/Makefile",
        "config/provenance.toml",
        f"{work_dir()}/aot/c/manifest.json",
        f"{work_dir()}/director/model.json",
        "games/findus-workshop/host/full_validation.py",
        "src/director64/full_assets.py",
        "src/director64/aot.py",
        "src/director64/director.py",
        "games/findus-workshop/tests/full_probe.py",
        "compiler/src/convert/compile.rs",
        "compiler/src/convert/movie.rs",
        "compiler/src/convert/bitmap.rs",
    ]
    names += [
        str(p.relative_to(root))
        for directory in ("runtime", "platforms/n64", "games/findus-workshop/runtime")
        for p in sorted((root / directory).rglob("*"))
        if p.suffix in {".c", ".h"}
    ]
    pins = {}
    for name in names:
        dest = run / "inputs/source" / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / name, dest)
        pins[name] = digest(dest.read_bytes())
    validation["source_sha256"] = pins
    validation["emulator"] = subprocess.check_output(["gopher64", "--version"], text=True).strip()
    (run / "inputs/validation.json").write_text(json.dumps(validation, indent=2) + "\n")


def validate_capture(run: Path) -> dict:
    replay = json.loads((run / "inputs/replay.json").read_text())
    log = (run / "gopher64.log").read_text()
    if re.search(
        r"NATIVE_FAIL|REQUIREMENT_FAIL|ASSERTION FAILED|RSP CRASH|panicked|NATIVE_SAVE_ERROR",
        log,
    ):
        raise CaptureError("full-game guest failed; retained capture contains diagnostics")
    match = re.findall(
        r"FULL_REPLAY_COMPLETE id=(\S+) movie=(\S+) frame=(\d+) ticks=(\d+) feathers=(\d+)", log
    )
    expected = (
        replay["target"],
        replay["expected_movie"],
        str(replay["expected_frame"]),
        str(replay.get("expected_game_ticks", replay["ticks"])),
        str(replay["expected_feathers"]),
    )
    if match != [expected]:
        raise CaptureError(f"native replay did not reach its exact checkpoint: {match!r}")
    if replay.get("controller") == "feedback-mouse":
        events = re.findall(r"FULL_CONTROL_ASSERT (\w+)", log)
        assertions = {
            "MOSSEN": ["initial_failure", "retry", "reward", "return"],
            "FINNDUNK": ["initial_failure", "reward", "return"],
            "SNIKBOD": ["all_nine_fragments", "special_treasure", "return"],
            **{f"PLOCKSPL:{i}": ["difficulty", "reward", "return"] for i in (1, 2, 3)},
            **{f"VEMORY:{i}": ["difficulty", "reward", "return"] for i in (1, 2, 3)},
        }
        if replay["target"] not in assertions or events != assertions[replay["target"]]:
            raise CaptureError(f"feedback controller assertions failed: {events!r}")
    media, streams = validate_media(
        run / "full.mp4", expected_frames=(math.ceil(replay["ticks"] / 60) + 15) * 60
    )
    if not any(s["codec_type"] == "audio" for s in streams["streams"]):
        raise CaptureError("native capture has no audio stream")
    # Decode and measure actual audio, not merely the presence of an AAC header.
    sound = subprocess.run(
        [
            "ffmpeg",
            "-nostdin",
            "-i",
            str(run / "full.mp4"),
            "-af",
            "volumedetect",
            "-vn",
            "-f",
            "null",
            "-",
        ],
        capture_output=True,
        text=True,
        check=True,
    ).stderr
    peak = re.search(r"max_volume: ([-\w.]+) dB", sound)
    if not peak:
        raise CaptureError("native capture audio level could not be measured")
    expected_audio = replay.get("expected_audio", "audible")
    silent = float(peak[1]) < -60
    if expected_audio not in {"audible", "silent"}:
        raise CaptureError("invalid replay audio expectation")
    if expected_audio == "audible" and silent:
        raise CaptureError("native capture audio is silent")
    if expected_audio == "silent" and not silent:
        raise CaptureError("native capture has unexpected audio")
    saves = list((run / "data").rglob("*.fla"))
    journal = None
    save_oracle = None
    if "NATIVE_SAVE_OK" in log:
        if len(saves) != 1:
            raise CaptureError("expected one isolated FlashRAM journal")
        journal = inspect_archive(saves[0].read_bytes())
        oracles = [replay.get("expected_saves", [])]
        if replay.get("expected_save_alternatives"):
            if replay["target"] != "SNIKBOD":
                raise CaptureError(
                    "alternate save oracles are only defined for source map treasures"
                )
            oracles += replay["expected_save_alternatives"]
        if len(oracles[0]) == 9:
            if journal["files"] not in oracles:
                raise CaptureError(
                    "N64 persisted files differ from the source-driven host scenario"
                )
            save_oracle = oracles.index(journal["files"])
    result = {
        "status": "passing",
        "platform": "gopher64",
        "controller": replay.get("controller", "recorded-mouse"),
        "hardware_qualified": False,
        "checkpoint": expected,
        "media": media,
        "audio_peak_db": float(peak[1]) if peak[1] != "-inf" else None,
        "expected_audio": expected_audio,
        "journal": journal,
        "save_oracle": save_oracle,
        "log_sha256": digest(log.encode()),
        "rom_sha256": digest((run / "inputs/findus-workshop-probe.z64").read_bytes()),
        "timing_overruns": log.count("NATIVE_TIMING_OVERRUN"),
    }
    (run / "validation.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def reboot(run: Path) -> dict:
    """Reboot only a completed capture's newly created save sandbox."""
    run = run.resolve()
    result = json.loads((run / "validation.json").read_text())
    if result["status"] != "passing" or not result["journal"]:
        raise CaptureError("reload requires a passing capture with a committed save")
    saves = list((run / "data").rglob("*.fla"))
    if len(saves) != 1:
        raise CaptureError("expected exactly the isolated capture save")
    before = saves[0].read_bytes()
    env = os.environ.copy()
    for key in ("CONFIG", "DATA", "CACHE"):
        env[f"XDG_{key}_HOME"] = str(run / key.lower())
    with (run / "reload.log").open("x") as log:
        subprocess.run(
            [
                "gopher64",
                "--overclock",
                "false",
                "--disable-expansion-pak",
                "false",
                "--capture-output",
                str(run / "reload.mp4"),
                "--capture-width",
                "640",
                "--capture-height",
                "480",
                "--capture-framerate",
                "60",
                "--capture-start-marker",
                "DIRECTOR64 NATIVE_RENDER_READY",
                "--capture-start-timeout",
                "40",
                "--capture-duration",
                "2",
                "--capture-wall-timeout",
                "90",
                str(run / "inputs/findus-workshop-probe.z64"),
            ],
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
            timeout=110,
        )
    log = (run / "reload.log").read_text()
    marker = f"NATIVE_SAVE_LOADED status=1 generation={result['journal']['generation']} migrated=0"
    if marker not in log or re.search(
        r"NATIVE_FAIL|REQUIREMENT_FAIL|NATIVE_SAVE_ERROR|panicked", log
    ):
        raise CaptureError("saved archive did not reload successfully")
    if saves[0].read_bytes() != before:
        raise CaptureError("read-only startup unexpectedly changed the committed save")
    media, _ = validate_media(run / "reload.mp4", expected_frames=120)
    result["reload"] = {
        "status": "passing",
        "unchanged_sha256": digest(before),
        "generation": result["journal"]["generation"],
        "media": media,
    }
    (run / "validation.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["rom", "snapshot", "capture", "reboot"])
    parser.add_argument("target")
    args = parser.parse_args()
    if args.mode == "snapshot":
        snapshot(Path.cwd(), Path(args.target))
    else:
        result = (
            validate_rom(Path.cwd(), args.target)
            if args.mode == "rom"
            else reboot(Path(args.target))
            if args.mode == "reboot"
            else validate_capture(Path(args.target))
        )
        print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()

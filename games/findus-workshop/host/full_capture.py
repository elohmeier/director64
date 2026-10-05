"""Freeze a full-game probe before capturing it in an independent emulator process."""

from __future__ import annotations

import json
import math
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from director64.project import selected_game, work_dir
from director64.rom_inputs import verify as verify_rom_inputs

from .full_validation import reboot, snapshot, validate_capture


def prepare(root: Path) -> Path:
    parent = root / f"{work_dir()}/captures"
    parent.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="full.", dir=parent))
    for directory in ("inputs", "config", "data", "cache"):
        (run / directory).mkdir()
    probe = root / f"{work_dir()}/n64/findus-workshop-probe.z64"
    verify_rom_inputs(selected_game(), "probe", probe)
    for source in (
        f"{work_dir()}/n64/findus-workshop-probe.z64",
        f"{work_dir()}/n64/probe/findus-workshop-probe.elf",
        f"{work_dir()}/director/c/replay.json",
        f"{work_dir()}/director/c/replay.inc",
        f"{work_dir()}/director/packed.json",
    ):
        shutil.copyfile(root / source, run / "inputs" / Path(source).name)
    snapshot(root, run)
    return run


def capture(run: Path) -> dict:
    replay = json.loads((run / "inputs/replay.json").read_text())
    duration = math.ceil(replay["ticks"] / 60) + 15
    env = os.environ | {
        "XDG_CONFIG_HOME": str(run / "config"),
        "XDG_DATA_HOME": str(run / "data"),
        "XDG_CACHE_HOME": str(run / "cache"),
    }
    with (run / "gopher64.log").open("x") as log:
        subprocess.run(
            [
                "gopher64",
                "--overclock",
                "false",
                "--disable-expansion-pak",
                "false",
                "--capture-output",
                str(run / "full.mp4"),
                "--capture-width",
                "640",
                "--capture-height",
                "480",
                "--capture-framerate",
                "60",
                "--capture-start-marker",
                "DIRECTOR64 NATIVE_RENDER_READY",
                "--capture-start-timeout",
                "60",
                "--capture-duration",
                str(duration),
                "--capture-wall-timeout",
                "600",
                str(run / "inputs/findus-workshop-probe.z64"),
            ],
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
            timeout=680,
        )
    subprocess.run(
        [
            "ffmpeg",
            "-nostdin",
            "-v",
            "error",
            "-i",
            str(run / "full.mp4"),
            "-vf",
            f"fps=12/{duration},scale=320:240,tile=4x3",
            "-frames:v",
            "1",
            str(run / "contact.png"),
        ],
        check=True,
        timeout=120,
    )
    result = validate_capture(run)
    if replay["target"] in {"SNIKBOD", "VERKORK", "BREDHOGN"}:
        result = reboot(run)
    (run / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result

"""Validate manual-ROM boot and a visible, non-destructive corrupt-save failure."""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from director64.captures import CaptureError, validate_media
from director64.pacing import analyze, assert_pacing
from director64.project import work_dir

from .full_validation import digest, validate_rom

# The introduction holds a flat 60 Hz: every measurable window of the cold
# boot delivers 1.000, with the handle count and value heap unchanged across
# the run because nothing is being created there. This gate says so, and the
# growth limits are tight because a boot screen that starts retaining is
# exactly what they should catch. The corrupt-archive case is not judged: it
# fails closed on purpose and never gets far enough to pace anything.
PACING_BUDGET = {
    "min_tick_delivery": 0.99,
    "sustained_windows": 2,
    "max_object_growth": 1.05,
    "max_heap_growth": 1.05,
    "min_growth_windows": 4,
    # Measured 280.00 us per Lingo step on 2026-09-26; about 5% headroom, so a
    # runtime or layout regression fails here even where delivery has slack.
    "max_service_us_per_step": 294.0,
}


def main():
    root = Path.cwd()
    validation = validate_rom(root, "release")
    (root / f"{work_dir()}/captures").mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="manual-boot.", dir=root / f"{work_dir()}/captures"))
    rom = run / "findus-workshop.z64"
    shutil.copyfile(root / f"{work_dir()}/n64/findus-workshop.z64", rom)
    shutil.copyfile(
        root / f"{work_dir()}/n64/release/findus-workshop.elf", run / "findus-workshop.elf"
    )
    validation["emulator"] = subprocess.check_output(["gopher64", "--version"], text=True).strip()
    for corrupt in (False, True):
        case = run / ("corrupt" if corrupt else "cold")
        env = os.environ.copy()
        for name in ("config", "data", "cache"):
            (case / name).mkdir(parents=True)
            env[f"XDG_{name.upper()}_HOME"] = str(case / name)
        save = (
            case / "data/gopher64/saves" / f"Findus Workshop-{digest(rom.read_bytes()).upper()}.fla"
        )
        fixture = bytes([0x5A]) * 131072
        if corrupt:
            save.parent.mkdir(parents=True)
            save.write_bytes(fixture)
        marker = "DIRECTOR64 NATIVE_FAIL" if corrupt else "DIRECTOR64 NATIVE_RENDER_READY"
        seconds = 3 if corrupt else 20
        with (case / "gopher64.log").open("x") as log:
            subprocess.run(
                [
                    "gopher64",
                    "--overclock",
                    "false",
                    "--disable-expansion-pak",
                    "false",
                    "--capture-output",
                    str(case / "boot.mp4"),
                    "--capture-width",
                    "640",
                    "--capture-height",
                    "480",
                    "--capture-framerate",
                    "60",
                    "--capture-start-marker",
                    marker,
                    "--capture-start-timeout",
                    "60",
                    "--capture-duration",
                    str(seconds),
                    "--capture-wall-timeout",
                    "120",
                    str(rom),
                ],
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
                check=True,
                timeout=150,
            )
        log = (case / "gopher64.log").read_text()
        if "FULL_REPLAY_" in log or re.search(r"panicked|RSP CRASH|REQUIREMENT_FAIL", log):
            raise CaptureError("manual boot produced replay or crash markers")
        if corrupt:
            if "Save data unreadable; existing saves were not changed" not in log:
                raise CaptureError("corrupt archive did not fail closed")
            if save.read_bytes() != fixture or "NATIVE_SAVE_OK" in log:
                raise CaptureError("corrupt save was changed")
        elif "NATIVE_FAIL" in log or "NATIVE_SCENE name=GINTRO.DXR" not in log:
            raise CaptureError("manual native introduction did not boot")
        media, _ = validate_media(case / "boot.mp4", expected_frames=seconds * 60)
        subprocess.run(
            [
                "ffmpeg",
                "-nostdin",
                "-v",
                "error",
                "-ss",
                "2",
                "-i",
                str(case / "boot.mp4"),
                "-frames:v",
                "1",
                str(case / "screen.png"),
            ],
            check=True,
        )
        pacing = analyze(log)
        if not corrupt:
            try:
                assert_pacing(pacing, PACING_BUDGET)
            except ValueError as error:
                raise CaptureError(f"{error} ({case})") from error
        validation[case.name] = {
            "status": "passing",
            "media": media,
            "pacing": pacing["summary"],
            "log_sha256": digest(log.encode()),
            "save_unchanged": corrupt,
        }
    (run / "validation.json").write_text(json.dumps(validation, indent=2) + "\n")
    print(f"FULL_MANUAL_BOOT={run}")


if __name__ == "__main__":
    main()

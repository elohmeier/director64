"""Capture and validate the release ROM booting to the login screen."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from director64.captures import CaptureError, validate_media
from director64.pacing import analyze, assert_pacing
from director64.project import selected_game

from .full_validation import validate_rom

FAILURE = re.compile(
    r"\b(?:NATIVE_\w*(?:FAIL|ERROR|UNSUPPORTED)\w*|REQUIREMENT_FAIL|RSP ?CRASH|"
    r"ASSERTION FAILED|EXCEPTION|REPLAY_ERROR)\b|\bpanick?ed\b|^Error:",
    re.I | re.M,
)

# Observe both 640x480 interlaced fields. Sampling at 30 Hz can repeatedly
# capture one field; black alternate rows also destroy chroma in YUV420 video.
CAPTURE_FPS = 60

# The launcher opens the controller window, the controller checks the
# environment, the Tivola intro and transition play, and the login screen
# holds. TIVOLAINTRO2's Flash intro is a pinned deferred conversion, so the
# intro scene may render sparsely; the login screen is the boot contract.
BOOT_SCENES = (
    "DEUTSCH.DXR",
    "CONTROL.DXR",
    "CHECK.DXR",
    "TIVINTRO.DXR",
    "TRANSITION.DXR",
    "LOGIN.DXR",
)


def capture(
    game,
    *,
    profile="release",
    marker="DIRECTOR64 SCENE_RENDERED name=LOGIN.DXR frame=8",
    duration=20,
    completion=None,
    required=BOOT_SCENES,
    pacing_budget=None,
):
    validation = validate_rom(game.root, profile)
    emulator = Path.home() / ".cargo/bin/gopher64"
    version = subprocess.check_output([str(emulator), "--version"], text=True).strip()
    help_text = subprocess.check_output([str(emulator), "--help"], text=True)
    if "--capture-start-marker" not in help_text or "--capture-wall-timeout" not in help_text:
        raise CaptureError("Gopher64 lacks the required bounded capture interface")
    parent = game.work / "captures"
    parent.mkdir(exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix=profile + "-", dir=parent))
    name = game.slug + ("-probe" if profile == "probe" else "")
    for suffix in ("elf", "msym", "map"):
        shutil.copyfile(
            game.work / "n64" / profile / f"{name}.{suffix}", output / f"input.{suffix}"
        )
    shutil.copyfile(game.work / "n64" / f"{name}.z64", output / "input.z64")
    if hashlib.sha256((output / "input.z64").read_bytes()).hexdigest() != validation["rom_sha256"]:
        raise CaptureError("ROM changed while freezing capture inputs")
    args = [
        str(emulator),
        str(output / "input.z64"),
        "--overclock",
        "false",
        "--disable-expansion-pak",
        "false",
        "--capture-output",
        str(output / "video.mp4"),
        "--capture-start-marker",
        marker,
        "--capture-start-timeout",
        "240",
        "--capture-wall-timeout",
        "360",
        "--capture-duration",
        str(duration),
        "--capture-framerate",
        str(CAPTURE_FPS),
        "--capture-width",
        "640",
        "--capture-height",
        "480",
    ]
    (output / "command.json").write_text(json.dumps(args, indent=2) + "\n")
    env = os.environ | {
        f"XDG_{key.upper()}_HOME": str(output / key) for key in ("config", "data", "cache")
    }
    with (output / "emulator.log").open("w") as log:
        result = subprocess.run(args, stdout=log, stderr=subprocess.STDOUT, env=env, timeout=390)
    log = (output / "emulator.log").read_text()
    failed = [line for line in log.splitlines() if FAILURE.search(line)]
    if result.returncode or failed:
        raise CaptureError(f"capture failed ({output}): {' | '.join(failed)}")
    scenes = re.findall(r"DIRECTOR64 NATIVE_SCENE name=(\S+)", log)
    if not all(name in scenes for name in required):
        raise CaptureError(f"missing required scenes: {scenes}")
    if (
        "Capture marker matched" not in log
        or f"Capture complete: {duration * CAPTURE_FPS} frames" not in log
    ):
        raise CaptureError("capture did not reach its render gate and declared duration")
    if completion and completion not in log:
        raise CaptureError(f"missing replay completion marker: {output}")
    media, probe = validate_media(
        output / "video.mp4", expected_frames=duration * CAPTURE_FPS, expected_fps=CAPTURE_FPS
    )
    if not any(stream["codec_type"] == "audio" for stream in probe["streams"]):
        raise CaptureError("capture has no audio stream")
    audio = subprocess.run(
        [
            "ffmpeg",
            "-hide_banner",
            "-i",
            str(output / "video.mp4"),
            "-af",
            "volumedetect",
            "-vn",
            "-sn",
            "-f",
            "null",
            "-",
        ],
        capture_output=True,
        text=True,
        check=True,
    ).stderr
    peak = re.search(r"max_volume: ([-\d.]+) dB", audio)
    if not peak:
        raise CaptureError("capture audio level could not be measured")
    pictures = []
    for second in (5, duration // 2, duration - 2):
        path = output / f"frame-{second}.png"
        subprocess.run(
            [
                "ffmpeg",
                "-v",
                "error",
                "-ss",
                str(second),
                "-i",
                str(output / "video.mp4"),
                "-frames:v",
                "1",
                str(path),
            ],
            check=True,
        )
        raw = subprocess.check_output(
            [
                "ffmpeg",
                "-v",
                "error",
                "-i",
                str(path),
                "-vf",
                "scale=64:48",
                "-f",
                "rawvideo",
                "-pix_fmt",
                "rgb24",
                "-",
            ]
        )
        colors = len({raw[i : i + 3] for i in range(0, len(raw), 3)})
        if colors < 32:
            raise CaptureError(f"nearly blank representative image at {second}s")
        chroma = sum(max(raw[i : i + 3]) - min(raw[i : i + 3]) for i in range(0, len(raw), 3)) / (
            len(raw) / 3
        )
        # The authored login artwork is a gray castle wall with color only in
        # the accents (arrows, crest, grass): the real screen measures ~3.7.
        if chroma < 2:
            raise CaptureError(f"representative image lost source color at {second}s")
        pictures.append(
            {"second": second, "path": path.name, "colors": colors, "mean_chroma": chroma}
        )
    pacing = analyze(log)
    summary = pacing["summary"]
    print(
        "Pacing: {} windows, min tick delivery {}, min audio sync {}, "
        "overruns {}, dropped {} ms, gc {}+{}".format(
            summary["windows"],
            f"{summary['min_tick_delivery']:.3f}" if summary["min_tick_delivery"] else "n/a",
            f"{summary['min_audio_sync']:.3f}" if summary["min_audio_sync"] else "n/a",
            summary["total_overruns"],
            summary["total_dropped_us"] // 1000,
            summary["total_gc"],
            summary["total_egc"],
        ),
        flush=True,
    )
    if pacing_budget is not None:
        try:
            assert_pacing(pacing, pacing_budget)
        except ValueError as error:
            raise CaptureError(f"{error} ({output})") from error
    report = {
        "status": "passing",
        "platform": "gopher64",
        "emulator": version,
        "rom": validation,
        "scenes": scenes,
        "capture": media,
        "pictures": pictures,
        "completion": completion,
        "audio_peak_dbfs": float(peak[1]),
        "pacing": summary,
        "hardware_qualified": False,
        "original_projector_compared": False,
    }
    (output / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    print(output)
    return output, report


# Measured on this boot capture: the mean is 0.979 and the only window under
# 0.90 is the one a scene change lands in, never two in a row. The still
# login screen collects its scene's garbage (director_main.c), so handles end
# the run below where they started (0.67x on 2026-09-28) and the heap 1.24x
# above it; the growth limits sit clear of that. Without a collection the
# census counts garbage: the leaner boot of 2026-09-27 reached no allocation
# trigger and read 1.83x and 2.92x, though live data had shrunk.
PACING_BUDGET = {
    "min_tick_delivery": 0.90,
    "sustained_windows": 2,
    "max_object_growth": 1.6,
    "max_heap_growth": 1.8,
    # Measured 195.06 us per Lingo step on 2026-09-26; about 5% headroom, so a
    # runtime or layout regression fails here even where delivery has slack.
    "max_service_us_per_step": 205.0,
}


def main():
    capture(selected_game(), pacing_budget=PACING_BUDGET)

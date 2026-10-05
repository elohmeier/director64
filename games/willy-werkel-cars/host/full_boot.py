"""Capture and validate the release ROM's launcher and rendered player selection."""

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
    r"ASSERTION FAILED|EXCEPTION|REPLAY_ERROR|SCRIPT_ERROR)\b|\bpanick?ed\b|^Error:",
    re.I | re.M,
)

# Observe both 640x480 interlaced fields. Sampling at 30 Hz can repeatedly
# capture one field; black alternate rows also destroy chroma in YUV420 video.
CAPTURE_FPS = 60


def capture(
    game,
    *,
    profile="release",
    marker="DIRECTOR64 SCENE_RENDERED name=10.DXR frame=3",
    duration=20,
    start_timeout=240,
    completion=None,
    required=("START.DXR", "LBSTART.DXR", "10.DXR"),
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
        str(start_timeout),
        "--capture-wall-timeout",
        "300",
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
        result = subprocess.run(args, stdout=log, stderr=subprocess.STDOUT, env=env, timeout=330)
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
    if not peak or float(peak[1]) < -55:
        raise CaptureError("capture audio is silent or nearly silent")
    video_members = re.findall(r"VIDEO_OPEN sprite=(\d+) member=(\d+)", log)
    pictures = []
    for second in (5, 15, duration - 2):
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
        chroma = sum(
            max(raw[i : i + 3]) - min(raw[i : i + 3]) for i in range(0, len(raw), 3)
        ) / (len(raw) / 3)
        if chroma < 5:
            raise CaptureError(f"representative image lost source color at {second}s")
        pictures.append(
            {"second": second, "path": path.name, "colors": colors, "mean_chroma": chroma}
        )
    pacing = analyze(log)
    if pacing_budget is not None:
        assert_pacing(pacing, pacing_budget)
    report = {
        "status": "passing",
        "platform": "gopher64",
        "emulator": version,
        "rom": validation,
        "scenes": scenes,
        "pacing": pacing["summary"],
        "capture": media,
        "pictures": pictures,
        "completion": completion,
        "video_members_opened": video_members,
        "audio_peak_dbfs": float(peak[1]),
        "hardware_qualified": False,
        "original_projector_compared": False,
    }
    (output / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    print(output)
    return output, report


def main():
    capture(selected_game())

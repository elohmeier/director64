"""Capture and validate the release ROM's intro and rendered panorama."""

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


def validate_intro_stops(log, model):
    movie = next(m for m in model["movies"] if m["name"] == "INTRO.DXR")
    stops = [
        {"member": int(member), "time": int(time), "duration": int(duration)}
        for member, time, duration in re.findall(
            r"VIDEO_STOP member=(\d+) time=(\d+) duration=(\d+)", log
        )
    ]
    completed = []
    for name in ("INTRO_1.MOV", "INTRO_2.MOV"):
        member = next(m for m in movie["members"] if m["name"] == name)
        identity = movie["id"] << 20 | member["cast"] << 16 | member["number"]
        stop = next((s for s in stops if s["member"] == identity), None)
        if not stop or stop["time"] != member["frames"] or stop["duration"] != member["frames"]:
            raise CaptureError(f"intro clip did not reach its full duration: {name}")
        completed.append(stop)
    return completed


def capture(
    game,
    *,
    profile="release",
    marker="DIRECTOR64 SCENE_RENDERED name=INTRO.DXR",
    duration=75,
    completion=None,
    required=("START.DXR", "INTRO.DXR", "PANO.DXR"),
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
        # Preserve RGB: neighbor-scaled 4:2:0 conversion can select the blank
        # interlaced scanlines for chroma and turn otherwise colored frames gray.
        "--capture-video-codec",
        "libx264rgb",
        "--capture-start-marker",
        marker,
        "--capture-start-timeout",
        "240",
        "--capture-wall-timeout",
        "300",
        "--capture-duration",
        str(duration),
        "--capture-framerate",
        "30",
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
        or f"Capture complete: {duration * 30} frames" not in log
    ):
        raise CaptureError("capture did not reach its render gate and declared duration")
    if completion and completion not in log:
        raise CaptureError(f"missing replay completion marker: {output}")
    media, probe = validate_media(
        output / "video.mp4", expected_frames=duration * 30, expected_fps=30
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
    if len(video_members) < 2:
        raise CaptureError("capture did not open both source intro clips")
    intro_stops = validate_intro_stops(
        log, json.loads((game.work / "director/model.json").read_text())
    )
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
        pixels = [raw[i : i + 3] for i in range(0, len(raw), 3)]
        colors = len(set(pixels))
        if colors < 32:
            raise CaptureError(f"nearly blank representative image at {second}s")
        chromatic = sum(max(pixel) - min(pixel) > 12 for pixel in pixels)
        if second == duration - 2 and chromatic < 32:
            raise CaptureError("final panorama capture lost its source colors")
        pictures.append(
            {"second": second, "path": path.name, "colors": colors, "chromatic_pixels": chromatic}
        )
    pacing = analyze(log)
    if pacing_budget is not None:
        try:
            assert_pacing(pacing, pacing_budget)
        except ValueError as error:
            raise CaptureError(f"{error} ({output})") from error
    report = {
        "status": "passing",
        "platform": "gopher64",
        "pacing": pacing["summary"],
        "emulator": version,
        "rom": validation,
        "scenes": scenes,
        "capture": media,
        "pictures": pictures,
        "completion": completion,
        "video_members_opened": video_members,
        "intro_clips_completed": intro_stops,
        "audio_peak_dbfs": float(peak[1]),
        "hardware_qualified": False,
        "original_projector_compared": False,
    }
    (output / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    print(output)
    return output, report


# Measured on this boot capture: every window delivers at least 0.985 and the
# mean is 0.999 -- the intro and the panorama both hold 60 Hz. Growth is
# recorded rather than judged tightly, because the run starts on the boot
# screen and ends in the panorama, so the quartile ratio is the scene
# difference (3.8x handles, 6.1x heap) and the limits only catch a runaway.
PACING_BUDGET = {
    "min_tick_delivery": 0.95,
    "sustained_windows": 2,
    "max_object_growth": 6.0,
    "max_heap_growth": 9.0,
    # Measured 122.57 us per Lingo step on 2026-09-26; about 5% headroom, so a
    # runtime or layout regression fails here even where delivery has slack.
    "max_service_us_per_step": 129.0,
}


def main():
    capture(selected_game(), pacing_budget=PACING_BUDGET)

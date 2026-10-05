"""Capture train and obstacle-course input, rendering and measured guest work."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import time
from pathlib import Path

from director64.captures import CaptureError, validate_media, write_json
from director64.cli import build
from director64.evidence import sha256
from director64.pacing import analyze, assert_pacing, summarize
from director64.project import selected_game

from .full_boot import FAILURE, capture_command, validate_audio

SCENARIOS = {
    "train": (1, "TS.DXR"),
    "obstacle": (2, "BR.DXR"),
    "train-departure": (3, "TS.DXR"),
    "train-soak": (4, "TS.DXR"),
}
# Interpreter cost per Lingo step each activity replay was measured at on
# 2026-09-26, with about 5% headroom. Tick delivery hid a 17% per-step
# regression here once, because the train scene has slack; this does not.
SERVICE_US_PER_STEP = {"train": 53.5, "obstacle": 66.0, "train-departure": 56.0}
# Image bytes the port's cache may hold: the region game.c reserves plus the
# platform's arena overflow allowance for what does not fit in it.
CACHE_CEILING = 3456 * 1024 + 64 * 1024
# An hour of station play in a few minutes of wall time. The reported
# slowdown was play-driven and took minutes of game time to show, so the
# gate has to outlast it.
SOAK_SECONDS = 3600


def validate_log(log: str, movie: str, *, departure: bool = False) -> dict:
    failures = [line for line in log.splitlines() if FAILURE.search(line)]
    if failures:
        raise CaptureError("guest/emulator failure: " + " | ".join(failures))
    marker = re.search(rf"ACTIVITY_READY movie={re.escape(movie)} frame=\d+ tick=(\d+)", log)
    if not marker or "Capture complete: 3600 frames" not in log:
        raise CaptureError("activity capture did not reach and finish its gate")
    if "ACTIVITY_NAME_TYPED value=A" not in log or f"ACTIVITY_INTERACTION movie={movie}" not in log:
        raise CaptureError("missing source name-entry or activity interaction")
    if departure:
        motion = re.search(r"ACTIVITY_TRAIN_MOTION ticks=(\d+) updates=(\d+)", log)
        if (
            not re.search(r"ACTIVITY_TRAIN_ANSWER number=([1-9]|[12]\d|30)\b", log)
            or not motion
            or int(motion[1]) < 120
            or int(motion[2]) < 12
        ):
            raise CaptureError("missing train answer or sustained bounded piston motion")
    samples, caches, current = [], [], {}
    for line in log.splitlines():
        tick = re.search(r"NATIVE_TICK movie=(\S+) frame=(\d+) tick=(\d+) free=(\d+)", line)
        if tick:
            current = {
                "movie": tick[1],
                "frame": int(tick[2]),
                "tick": int(tick[3]),
                "free": int(tick[4]),
            }
        if current.get("movie") != movie or current.get("tick", 0) < int(marker[1]) + 300:
            continue  # Exclude the interval crossing the gate.
        cost = re.search(
            r"NATIVE_COST ticks_us=(\d+) render_us=(\d+) audio_us=(\d+) renders=(\d+) steps=(\d+)",
            line,
        )
        if cost:
            row = current | dict(
                zip(
                    ("ticks_us", "render_us", "audio_us", "renders", "steps"),
                    map(int, cost.groups()),
                    strict=True,
                )
            )
            row["work_us"] = row["ticks_us"] + row["render_us"] + row["audio_us"]
            samples.append(row)
        if "NATIVE_CACHE " in line:
            caches.append({key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line)})
    if len(samples) < 8 or len(caches) != len(samples):
        raise CaptureError("need eight complete activity work/cache samples")
    if any(b["tick"] - a["tick"] != 300 for a, b in zip(samples, samples[1:], strict=False)):
        raise CaptureError("activity samples are not contiguous")
    realtime_budget_met = max(row["work_us"] for row in samples) <= 5_000_000
    # Departure adds continuous motion and scene loads beyond the idle activity
    # timing gates. Keep its crash regression distinct and report pacing truthfully.
    if not realtime_budget_met and not departure:
        raise CaptureError("activity work exceeds its five-second guest budget")
    allocation_retries = sum(row["allocation_retries"] for row in caches)
    if (allocation_retries and not departure) or any(
        row["min_free"] <= 0 or row["peak_bytes"] > CACHE_CEILING for row in caches
    ):
        raise CaptureError("activity allocation or cache budget failed")
    return {
        "movie": movie,
        "samples": samples,
        "cache_samples": caches,
        "maximum_work_us": max(row["work_us"] for row in samples),
        "realtime_budget_met": realtime_budget_met,
        "allocation_retries": allocation_retries,
        "minimum_observed_free_bytes": min(row["min_free"] for row in caches),
        "hardware_qualified": False,
        "original_projector_verified": False,
    }


def validate_soak(log: str, movie: str) -> dict:
    """Judge an hour of continuous station play for progressive slowdown.

    The reported fault was a session that got slower the longer it ran and
    ended with the station platform missing for seconds at a time, so this
    gate asks the two questions that describe it: did tick delivery hold from
    the first quarter of the run to the last, and did every sprite that asked
    for an image get one."""
    failures = [line for line in log.splitlines() if FAILURE.search(line)]
    if failures:
        raise CaptureError("guest/emulator failure: " + " | ".join(failures))
    if not re.search(rf"ACTIVITY_READY movie={re.escape(movie)}", log):
        raise CaptureError("soak never reached the station")
    arrivals = len(re.findall(r"ACTIVITY_TRAIN_ARRIVAL ", log))
    answers = len(re.findall(r"ACTIVITY_TRAIN_ANSWER ", log))
    if arrivals < 10 or answers < 10:
        raise CaptureError(f"soak did not keep playing: {arrivals} arrivals, {answers} answers")
    report = analyze(log)
    rows = [
        row
        for row in report["windows"]
        if row["movie"] == movie and row["tick_delivery"] is not None
    ]
    if len(rows) < 100:
        raise CaptureError(f"soak covered only {len(rows)} station windows")
    quarter = len(rows) // 4
    first = sum(row["tick_delivery"] for row in rows[:quarter]) / quarter
    last = sum(row["tick_delivery"] for row in rows[-quarter:]) / quarter
    # A skipped sprite is the missing-platform symptom; the fixed build has
    # none across the hour, so any is a regression worth reading.
    skipped = sum(row["repacks"] for row in rows)
    if last < first * 0.9:
        raise CaptureError(f"station play degraded: delivery {first:.3f} -> {last:.3f}")
    if skipped:
        raise CaptureError(f"{skipped} sprite renders skipped for want of image memory")
    # Station play costs about 90% of the service budget in its own scripts
    # and rendering, so it delivers ticks at ~0.91 throughout rather than at
    # 1.0; the measured hour never stays under 0.70 for even two windows.
    # What this asserts is that the shape holds — no decay, no accumulation —
    # not that the scene is inside the 60 Hz budget, which it is not.
    assert_pacing(
        {"summary": summarize(rows), "windows": rows},
        {"min_tick_delivery": 0.70, "sustained_windows": 3},
    )
    return {
        "movie": movie,
        "station_windows": len(rows),
        "mean_tick_delivery": round(
            sum(row["tick_delivery"] for row in rows) / len(rows), 4
        ),
        "arrivals": arrivals,
        "answers": answers,
        "first_quarter_tick_delivery": round(first, 4),
        "last_quarter_tick_delivery": round(last, 4),
        "minimum_tick_delivery": round(min(row["tick_delivery"] for row in rows), 4),
        "skipped_sprite_renders": skipped,
        "minimum_observed_free_bytes": min(row["free"] for row in rows),
        "hardware_qualified": False,
        "original_projector_verified": False,
    }


def capture(rom: Path, run: Path, scenario: str) -> dict:
    run.mkdir(parents=True, exist_ok=False)
    emulator = shutil.which(os.environ.get("GOPHER64", "gopher64"))
    if not emulator:
        raise CaptureError("gopher64 is required")
    frozen = run / "activity.z64"
    shutil.copyfile(rom, frozen)
    soak = scenario == "train-soak"
    args = capture_command(emulator, str(frozen), run)
    args[args.index("--capture-start-marker") + 1] = "DIRECTOR64 ACTIVITY_READY"
    args[args.index("--capture-start-timeout") + 1] = "480"
    args[args.index("--capture-wall-timeout") + 1] = "10800" if soak else "480"
    if soak:
        # The soak is judged from the guest's own markers, so the recording
        # only has to exist; a small, sparse one keeps an hour of game time
        # affordable to encode.
        args[args.index("--capture-duration") + 1] = str(SOAK_SECONDS)
        args[args.index("--capture-framerate") + 1] = "5"
        args[args.index("--capture-width") + 1] = "320"
        args[args.index("--capture-height") + 1] = "240"
    write_json(run / "command.json", args)
    env = os.environ.copy()
    for name in ("data", "config", "cache"):
        env[f"XDG_{name.upper()}_HOME"] = str(run / name)
    with (run / "gopher64.log").open("x") as output:
        subprocess.run(
            args,
            env=env,
            stdout=output,
            stderr=subprocess.STDOUT,
            check=True,
            timeout=10_800 if soak else 500,
        )
    if soak:
        result = validate_soak((run / "gopher64.log").read_text(), SCENARIOS[scenario][1])
        result |= {"scenario": scenario, "rom_sha256": sha256(frozen), "status": "passing"}
        write_json(run / "result.json", result)
        return result
    result = validate_log(
        (run / "gopher64.log").read_text(),
        SCENARIOS[scenario][1],
        departure=scenario == "train-departure",
    )
    per_step = analyze((run / "gopher64.log").read_text())["summary"]["service_us_per_step"]
    if per_step is None or per_step > SERVICE_US_PER_STEP[scenario]:
        raise CaptureError(
            f"service cost {per_step} us per Lingo step exceeds "
            f"{SERVICE_US_PER_STEP[scenario]} for {scenario}"
        )
    result["service_us_per_step"] = round(per_step, 2)
    video = run / "lo.mp4"
    media, probe = validate_media(video, expected_frames=3600, expected_fps=60)
    write_json(run / "ffprobe.json", probe)
    audio = subprocess.run(
        [
            "ffmpeg",
            "-nostdin",
            "-v",
            "info",
            "-i",
            str(video),
            "-af",
            "astats=metadata=0:reset=0",
            "-f",
            "null",
            "-",
        ],
        capture_output=True,
        text=True,
        check=True,
        timeout=60,
    ).stderr
    (run / "audio.log").write_text(audio)
    result |= {
        "scenario": scenario,
        "rom_sha256": sha256(frozen),
        "video": media,
        "audio": validate_audio(probe, audio),
        "status": "passing",
    }
    write_json(run / "result.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    # The soak runs an hour of game time; ask for it by name.
    parser.add_argument(
        "scenarios",
        choices=SCENARIOS,
        nargs="*",
        default=[name for name in SCENARIOS if name != "train-soak"],
    )
    parser.add_argument("--reports", type=Path)
    args = parser.parse_args()
    game = selected_game()
    reports = (
        args.reports or game.work / "activity-captures" / time.strftime("%Y%m%d-%H%M%S")
    ).resolve()
    results = []
    for scenario in args.scenarios:
        build(game, "probe", replay_scenario=SCENARIOS[scenario][0])
        result = capture(game.dist / f"{game.slug}-probe.z64", reports / scenario, scenario)
        results.append(result)
        summary = {"scenario": scenario, "status": result["status"]} | {
            key: result[key]
            for key in (
                "maximum_work_us",
                "realtime_budget_met",
                "allocation_retries",
                "station_windows",
                "first_quarter_tick_delivery",
                "last_quarter_tick_delivery",
                "skipped_sprite_renders",
            )
            if key in result
        }
        print(json.dumps(summary))
    write_json(reports / "validation.json", {"status": "passing", "results": results})

"""Capture source startup through the LO chooser, without qualifying later play."""

from __future__ import annotations

import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from director64.captures import CaptureError, command, validate_media, write_json
from director64.evidence import sha256
from director64.project import selected_game

from .full_validation import validate_rom

MARKER = "NATIVE_SCENE name=LO.DXR"
DURATION = 60
FPS = 60
TIMEOUT = 240
FRAMES = (0, 720, 1440, 2160, 2880, 3540)
FAILURE = re.compile(
    r"\b(?:NATIVE_\w*(?:FAIL|ERROR|UNSUPPORTED)\w*|REQUIREMENT_FAIL|RSP ?CRASH|"
    r"ASSERTION FAILED|FULL_REPLAY_\w*)\b|^Error:|\bpanick?ed\b|\bpanic\b",
    re.IGNORECASE | re.MULTILINE,
)


def validate_log(log: str, *, require_text: bool = False) -> dict:
    failures = [line for line in log.splitlines() if FAILURE.search(line)]
    if failures:
        raise CaptureError("guest/emulator failure: " + " | ".join(failures))
    scenes = re.findall(r"DIRECTOR64 NATIVE_SCENE name=(\S+)", log)
    if scenes != ["START.DXR", "INTRO.DXR", "POFINTRO.DXR", "LO.DXR"]:
        raise CaptureError(f"unexpected startup scene sequence: {scenes}")
    if "DIRECTOR64 NATIVE_RENDER_READY" not in log:
        raise CaptureError("missing native render-ready marker")
    matched = re.search(r"Capture marker matched at ([\d.]+) emulated seconds", log)
    if not matched or f"Capture complete: {DURATION * FPS} frames" not in log:
        raise CaptureError("LO capture did not start and finish at its declared gate")
    overruns = [int(n) for n in re.findall(r"NATIVE_TIMING_OVERRUN us=(\d+)", log)]
    states = re.findall(
        r"NATIVE_SOURCE_STATE movie=LO.DXR name=lo_state value=(\w+) tick=(\d+)", log
    )
    if not states or states[-1][0] != "valjer":
        raise CaptureError("source LO state did not reach and remain at the player chooser")
    alpha = re.findall(
        r"NATIVE_ALPHA_DRAW movie=(\S+) format=FDIA rgb_bits=15 alpha_bits=8 "
        r"tile_bytes=3072 member=(\d+)",
        log,
    )
    if not any(movie == "LO.DXR" for movie, _ in alpha):
        raise CaptureError("missing LO full-alpha texture draw marker")
    text = re.findall(
        r"NATIVE_TEXT_DRAW movie=(\S+) member=(\d+) font=(\d+) size=(\d+) glyphs=(\d+)",
        log,
    )
    if require_text and not any(
        movie == "LO.DXR" and font == "1" and int(glyphs) > 0 for movie, _, font, _, glyphs in text
    ):
        raise CaptureError("saved chooser did not draw original Pettson font glyphs")
    costs = []
    current = {}
    for line in log.splitlines():
        tick = re.search(r"NATIVE_TICK movie=(\S+) frame=(\d+) tick=(\d+)", line)
        if tick:
            current = {"movie": tick[1], "frame": int(tick[2]), "tick": int(tick[3])}
        cost = re.search(
            r"NATIVE_COST ticks_us=(\d+) render_us=(\d+) audio_us=(\d+) "
            r"renders=(\d+) steps=(\d+)",
            line,
        )
        if cost:
            costs.append(
                current
                | dict(
                    zip(
                        ("ticks_us", "render_us", "audio_us", "renders", "steps"),
                        map(int, cost.groups()),
                        strict=True,
                    )
                )
            )
    # The first LO sample can include the preceding scene and its asset loads.
    # Later samples each cover the full 300-tick interval in LO.
    lo_costs = [row for row in costs if row.get("movie") == "LO.DXR"][1:]
    for row in lo_costs:
        row["guest_work_us"] = sum(row[key] for key in ("ticks_us", "render_us", "audio_us"))
        row["interval_budget_us"] = 5_000_000
        row["work_to_budget_ratio"] = row["guest_work_us"] / row["interval_budget_us"]
    return {
        "scenes": scenes,
        "lo_marker_emulated_seconds": float(matched[1]),
        "timing_overrun_count": len(overruns),
        "max_reported_guest_timing_overrun_us": max(overruns, default=0),
        "source_state_changes": [{"value": value, "tick": int(tick)} for value, tick in states],
        "chooser_observed": True,
        "full_alpha_draws_queued": [
            {"movie": movie, "member": int(member), "rgb_bits": 15, "alpha_bits": 8}
            for movie, member in alpha
        ],
        "text_draws_queued": [
            {
                "movie": movie,
                "member": int(member),
                "font": int(font),
                "size": int(size),
                "glyphs": int(glyphs),
            }
            for movie, member, font, size, glyphs in text
        ],
        "cost_samples": costs,
        "steady_lo_cost_samples": lo_costs,
        "steady_lo_max_work_to_budget_ratio": max(
            (row["work_to_budget_ratio"] for row in lo_costs), default=None
        ),
        # VI capture throughput alone never establishes guest realtime pacing.
        "realtime_qualified": False,
    }


def validate_audio(probe: dict, astats: str) -> dict:
    try:
        streams = [s for s in probe["streams"] if s["codec_type"] == "audio"]
        if len(streams) != 1:
            raise CaptureError("expected one recorded audio stream")
        stream = streams[0]
        duration = float(stream["duration"])
        if (
            stream["codec_name"] != "aac"
            or int(stream["channels"]) != 2
            or int(stream["sample_rate"]) != 22050
            or not math.isfinite(duration)
            or abs(duration - DURATION) > 0.1
        ):
            raise CaptureError("expected AAC stereo 22050 Hz audio for the full capture")
        metrics = {}
        for key in ("RMS level dB", "Peak level dB", "Number of samples"):
            matches = re.findall(re.escape(key) + r":\s*([^\s]+)", astats)
            if not matches:
                raise CaptureError(f"missing decoded audio measurement: {key}")
            # astats prints channels first, then the overall measurement.
            metrics[key] = float(matches[-1])
        if (
            not all(math.isfinite(v) for v in metrics.values())
            or metrics["RMS level dB"] <= -90
            or metrics["Number of samples"] < DURATION * 22050
        ):
            raise CaptureError("decoded introduction audio is silent or incomplete")
        return {**metrics, "duration_seconds": duration, "non_silent": True}
    except (KeyError, TypeError, ValueError) as error:
        if isinstance(error, CaptureError):
            raise
        raise CaptureError(f"invalid audio evidence: {error}") from error


def validate_work_budget(log_result: dict) -> dict:
    """Check measured guest work; this does not establish realtime or hardware pacing."""
    samples = [row for row in log_result["cost_samples"] if row.get("movie") == "LO.DXR"]
    if len(samples) < 9:
        raise CaptureError("LO work budget requires at least eight complete 300-tick windows")
    work = []
    for previous, current in zip(samples, samples[1:], strict=False):
        if current["tick"] - previous["tick"] != 300:
            raise CaptureError("LO work budget samples are not contiguous 300-tick windows")
        total = sum(current[key] for key in ("ticks_us", "render_us", "audio_us"))
        if total > 5_000_000:
            raise CaptureError(
                f"LO work at tick {current['tick']} exceeds 5000000 us budget: {total} us"
            )
        work.append(total)
    return {
        "passed": True,
        "qualified_scope": "measured LO work after the first mixed scene sample",
        "contiguous_windows": len(work),
        "ticks_per_window": 300,
        "budget_us_per_window": 5_000_000,
        "includes": ["ticks_us", "render_us", "audio_us"],
        "maximum_measured_work_us": max(work),
        "maximum_work_to_budget_ratio": max(work) / 5_000_000,
        "realtime_qualified": False,
    }


def validate_pixels(raw: bytes) -> list[dict]:
    stride = 64 * 48 * 3
    if len(raw) != stride * len(FRAMES):
        raise CaptureError("representative image decoder returned an unexpected frame count")
    result = []
    for index, frame in enumerate(FRAMES):
        pixels = raw[index * stride : (index + 1) * stride]
        colors = len({pixels[n : n + 3] for n in range(0, stride, 3)})
        if colors < 16 or max(pixels) - min(pixels) < 32:
            raise CaptureError(f"representative frame {frame} is blank or nearly uniform")
        result.append(
            {
                "frame": frame,
                "colors_at_64x48": colors,
                "rgb_sha256": hashlib.sha256(pixels).hexdigest(),
            }
        )
    return result


def freeze(root: Path, game, run: Path, validation: dict) -> dict:
    """Copy the checked build; never build or change the selected ROM here."""
    build = game.work / "n64/release"
    files = [
        game.work / f"n64/{game.slug}.z64",
        build / f"{game.slug}.elf",
        build / "metadata.ini",
        game.work / "director/model.json",
        game.work / "director/packed.json",
        game.work / "aot/c/manifest.json",
        game.work / "build-inputs.json",
    ]
    files += sorted(build.glob("*.map")) + sorted(build.glob("*.msym"))
    files += sorted(build.glob("*.elf.sym"))
    files += sorted(game.work.glob("*build*.log"))
    files += [game.directory / "game.toml", root / "config/provenance.toml"]
    for directory, pattern in (
        ("runtime", "**/*.[ch]"),
        ("platforms/n64", "*"),
        ("src/director64", "*.py"),
        ("tools/director", "*.mjs"),
        (f"games/{game.slug}/runtime", "*.[ch]"),
        (f"games/{game.slug}/host", "*.py"),
    ):
        files += [p for p in sorted((root / directory).glob(pattern)) if p.is_file()]
    frozen = {}
    for source in dict.fromkeys(files):
        relative = source.relative_to(root)
        target = run / "inputs" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        frozen[str(relative)] = {"path": str(target.relative_to(run)), "sha256": sha256(target)}
    rom = run / "inputs" / (game.work / f"n64/{game.slug}.z64").relative_to(root)
    if sha256(rom) != validation["rom_sha256"]:
        raise CaptureError("ROM changed between validation and capture snapshot")
    return {"rom": str(rom), "files": frozen}


def capture_command(emulator: str, rom: str, run: Path) -> list[str]:
    return [
        emulator,
        rom,
        "--overclock",
        "false",
        "--disable-expansion-pak",
        "false",
        "--capture-output",
        str(run / "lo.mp4"),
        "--capture-start-marker",
        MARKER,
        "--capture-start-timeout",
        str(TIMEOUT),
        "--capture-warmup",
        "1",
        "--capture-duration",
        str(DURATION),
        "--capture-framerate",
        str(FPS),
        "--capture-width",
        "640",
        "--capture-height",
        "480",
        "--capture-wall-timeout",
        str(TIMEOUT),
    ]


def seed_save_fixture(report_path: Path, rom_path: Path, run: Path) -> dict:
    """Seed isolated FlashRAM from a hash-checked successful native journey."""
    report_bytes = report_path.read_bytes()
    report = json.loads(report_bytes)
    journey_path = report_path.parent / "journey.json"
    journey_bytes = journey_path.read_bytes()
    journey = json.loads(journey_bytes)
    if (
        report.get("status") != "passing"
        or report.get("platform") != "host-native"
        or not report.get("scenario", "").startswith("startup-player-create-reload")
        or report.get("journey_sha256") != hashlib.sha256(journey_bytes).hexdigest()
        or journey.get("error") is not None
    ):
        raise CaptureError("save fixture requires an intact passing native journey report")
    export = report.get("flash_export")
    if not isinstance(export, dict) or journey.get("flash_export") != export:
        raise CaptureError("save fixture export is not pinned by the native journey")
    name = export.get("path", "")
    if not name or Path(name).name != name:
        raise CaptureError("save fixture path must be a file beside the journey report")
    fixture_bytes = (report_path.parent / name).read_bytes()
    fixture_sha = hashlib.sha256(fixture_bytes).hexdigest()
    if (
        len(fixture_bytes) != 131072
        or export.get("bytes") != len(fixture_bytes)
        or export.get("sha256") != fixture_sha
    ):
        raise CaptureError("native FlashRAM fixture bytes differ from their journey receipt")
    evidence = run / "inputs/saved-fixture"
    evidence.mkdir(parents=True)
    for name, data in (
        ("validation.json", report_bytes),
        ("journey.json", journey_bytes),
        ("initial.fla", fixture_bytes),
    ):
        (evidence / name).write_bytes(data)
    # Gopher64 0b96c500: ui/storage.rs get_game_name/init and cart/rom.rs
    # calculate_hash use the sanitized ROM title and uppercase full-ROM SHA256.
    with rom_path.open("rb") as rom:
        header = rom.read(64)
    title = re.sub(r"[^a-zA-Z0-9_ -]", "", header[0x20:0x34].decode("utf-8")).strip()
    fallback = header[0x3B:0x3E].decode("ascii")
    prefix = title or (fallback if "\0" not in fallback else "UNK")
    target = run / "data/gopher64/saves" / f"{prefix}-{sha256(rom_path).upper()}.fla"
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("xb") as output:
        output.write(fixture_bytes)
    return {
        "origin": export["origin"],
        "fixture": {"path": "inputs/saved-fixture/initial.fla", "sha256": fixture_sha},
        "journey_sha256": report["journey_sha256"],
        "report_sha256": hashlib.sha256(report_bytes).hexdigest(),
        "emulator_save_path": str(target.relative_to(run)),
        "n64_input_journey_qualified": False,
    }


def inspect_media(run: Path) -> dict:
    video = run / "lo.mp4"
    media, probe = validate_media(video, expected_frames=DURATION * FPS, expected_fps=FPS)
    write_json(run / "ffprobe.json", probe)
    with (run / "audio-astats.log").open("x") as output:
        subprocess.run(
            [
                "ffmpeg",
                "-nostdin",
                "-hide_banner",
                "-xerror",
                "-i",
                str(video),
                "-map",
                "0:a:0",
                "-af",
                "astats=metadata=0:reset=0",
                "-f",
                "null",
                "-",
            ],
            stdout=output,
            stderr=subprocess.STDOUT,
            check=True,
            timeout=60,
        )
    audio = validate_audio(probe, (run / "audio-astats.log").read_text())
    select = "+".join(f"eq(n,{frame})" for frame in FRAMES)
    decoded = subprocess.run(
        [
            "ffmpeg",
            "-nostdin",
            "-v",
            "error",
            "-xerror",
            "-i",
            str(video),
            "-vf",
            f"select='{select}',scale=64:48",
            "-fps_mode",
            "passthrough",
            "-f",
            "rawvideo",
            "-pix_fmt",
            "rgb24",
            "-",
        ],
        check=True,
        capture_output=True,
        timeout=60,
    )
    representatives = validate_pixels(decoded.stdout)
    command(
        [
            "ffmpeg",
            "-nostdin",
            "-v",
            "error",
            "-xerror",
            "-i",
            str(video),
            "-vf",
            f"select='{select}',scale=320:240,tile=3x2",
            "-frames:v",
            "1",
            str(run / "contact.png"),
        ],
        timeout=60,
    )
    if not (run / "contact.png").is_file():
        raise CaptureError("representative contact image was not written")
    command(
        [
            "ffmpeg",
            "-nostdin",
            "-v",
            "error",
            "-xerror",
            "-i",
            str(video),
            "-vf",
            f"select='eq(n,{FRAMES[-1]})'",
            "-frames:v",
            "1",
            str(run / "chooser.png"),
        ],
        timeout=60,
    )
    if not (run / "chooser.png").is_file():
        raise CaptureError("full-resolution chooser image was not written")
    return {
        "video": media,
        "audio": audio,
        "representative_frames": representatives,
        "contact": {"path": "contact.png", "sha256": sha256(run / "contact.png")},
        "chooser": {
            "path": "chooser.png",
            "sha256": sha256(run / "chooser.png"),
            "frame": FRAMES[-1],
            "resolution": [640, 480],
        },
    }


def main():
    game = selected_game()
    root = game.root
    captures = game.work / "gopher-boot"
    captures.mkdir(parents=True, exist_ok=True)
    fixture_report = os.environ.get("DIRECTOR64_BOOT_JOURNEY")
    run = Path(tempfile.mkdtemp(prefix="lo-saved-" if fixture_report else "lo-boot-", dir=captures))
    result = {
        "schema_version": 2,
        "game": game.slug,
        "source_sha256": game.source["sha256"],
        "status": "failed",
        "highest_gate": None,
        "intended_gate": "LO introduction and source player chooser rendering and audio",
        "hardware_qualified": False,
        "original_projector_compared": False,
        "gameplay_qualified": False,
        "realtime_qualified": False,
        "input": "No controller input; source speech and animations advance to lo_state=valjer",
        "save_initialization": "native journey fixture" if fixture_report else "blank FlashRAM",
        "duration_seconds": DURATION,
        "warmup_seconds": 1,
        "start_marker": MARKER,
        "marker_timeout_emulated_seconds": TIMEOUT,
        "wall_timeout_seconds": TIMEOUT,
    }
    try:
        result["rom_validation"] = validation = validate_rom(root, "release")
        result["inputs"] = frozen = freeze(root, game, run, validation)
        emulator = Path.home() / ".cargo/bin/gopher64"
        executable = str(emulator) if emulator.is_file() else shutil.which("gopher64")
        if not executable:
            raise CaptureError("Gopher64 capture binary is unavailable")
        result["emulator"] = command([executable, "--version"]).strip()
        help_text = command([executable, "--help"])
        (run / "gopher64-help.txt").write_text(help_text)
        args = capture_command(executable, frozen["rom"], run)
        if any(flag not in help_text for flag in args if flag.startswith("--")):
            raise CaptureError("installed Gopher64 lacks a required capture option")
        result["ffmpeg"] = command(["ffmpeg", "-version"]).splitlines()[0]
        result["ffprobe"] = command(["ffprobe", "-version"]).splitlines()[0]
        env = os.environ.copy()
        for name in ("config", "data", "cache"):
            (run / name).mkdir()
            env[f"XDG_{name.upper()}_HOME"] = str(run / name)
        if fixture_report:
            result["save_fixture"] = seed_save_fixture(
                Path(fixture_report), Path(frozen["rom"]), run
            )
            result["intended_gate"] = "LO saved-player chooser with original Pettson font glyphs"
        result["command"] = args
        write_json(run / "capture.json", {**result, "status": "prepared"})
        with (run / "gopher64.log").open("x") as log:
            process = subprocess.run(
                args,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=TIMEOUT + 10,
            )
        result["exit_code"] = process.returncode
        log = (run / "gopher64.log").read_text(errors="replace")
        result.update(validate_log(log, require_text=bool(fixture_report)))
        if process.returncode:
            raise CaptureError(f"Gopher64 exited with status {process.returncode}")
        result["media"] = inspect_media(run)
        result["measured_work_budget"] = validate_work_budget(result)
        result["highest_gate"] = result["intended_gate"]
        result["status"] = (
            "lo-render-audio-passed-with-timing-overrun"
            if result["timing_overrun_count"]
            else "lo-render-audio-passed"
        )
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        result["failure"] = str(error)
        raise CaptureError(f"boot capture failed; preserved evidence: {run}: {error}") from error
    finally:
        if (run / "gopher64.log").exists():
            log = (run / "gopher64.log").read_text(errors="replace")
            result["log_sha256"] = sha256(run / "gopher64.log")
            scenes = re.findall(r"DIRECTOR64 NATIVE_SCENE name=(\S+)", log)
            result["last_scene"] = scenes[-1] if scenes else None
        write_json(run / "result.json", result)
    print(f"MUCKLAS_BOOT={run}")


if __name__ == "__main__":
    main()

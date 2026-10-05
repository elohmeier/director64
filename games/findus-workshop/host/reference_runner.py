#!/usr/bin/env python3
"""Capture the user's original projector in an isolated Wine container.

Captured media is observational evidence. This runner never declares semantic or
platform equivalence, injects random choices, or awards scene coverage.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import uuid
from datetime import UTC, datetime
from pathlib import Path

SCHEMA = 1
IDENTIFIER = re.compile(r"[a-zA-Z0-9][a-zA-Z0-9_.-]{0,95}\Z")
INTERRUPT_ERRORS = (subprocess.TimeoutExpired, KeyboardInterrupt)


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def read_replay(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict) or set(value) != {
        "schema_version",
        "id",
        "clock",
        "duration_ms",
        "actions",
    }:
        raise ValueError("replay has missing or unsupported fields")
    if (
        type(value["schema_version"]) is not int
        or value["schema_version"] != SCHEMA
        or not isinstance(value["id"], str)
        or not IDENTIFIER.fullmatch(value["id"])
    ):
        raise ValueError("replay requires schema_version=1 and a safe id")
    if value.get("clock") != "host_monotonic_ms":
        raise ValueError("original runner currently requires clock=host_monotonic_ms")
    duration = value.get("duration_ms")
    if type(duration) is not int or not 1000 <= duration <= 600_000:
        raise ValueError("duration_ms must be between 1000 and 600000")
    previous = -1
    checkpoint_ids = set()
    if not isinstance(value["actions"], list) or len(value["actions"]) > 10000:
        raise ValueError("actions must be a list containing at most 10000 entries")
    for action in value.get("actions", []):
        if not isinstance(action, dict):
            raise ValueError("actions must be objects")
        at = action.get("at_ms")
        if type(at) is not int or not previous <= at <= duration:
            raise ValueError("actions must be in timestamp order within duration_ms")
        previous = at
        kind = action.get("type")
        fields = {
            "pointer": {"x", "y"},
            "button_down": {"button"},
            "button_up": {"button"},
            "key": {"key"},
            "checkpoint": {"id"},
        }
        if not isinstance(kind, str) or kind not in fields:
            raise ValueError(f"unsupported action type: {kind}")
        if set(action) != {"at_ms", "type"} | fields[kind]:
            raise ValueError("action has missing or unsupported fields")
        if kind == "pointer":
            if any(type(action.get(k)) is not int for k in ("x", "y")):
                raise ValueError("pointer x/y must be integers")
            if not 0 <= action["x"] < 640 or not 0 <= action["y"] < 480:
                raise ValueError("pointer must remain within 640x480")
        elif kind in {"button_down", "button_up"}:
            if action.get("button") not in ("left", "right"):
                raise ValueError("button must be left or right")
        elif kind == "key":
            if action.get("key") not in ("Return", "Escape", "space", "Tab"):
                raise ValueError("unsupported key")
        elif kind == "checkpoint":
            identifier = action.get("id", "")
            if (
                not isinstance(identifier, str)
                or not IDENTIFIER.fullmatch(identifier)
                or identifier in checkpoint_ids
            ):
                raise ValueError("checkpoint ids must be safe and unique")
            checkpoint_ids.add(identifier)
        else:
            raise ValueError(f"unsupported action type: {kind}")
    if not checkpoint_ids:
        raise ValueError("replay must include a checkpoint")
    return value


def command(argv: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(argv, check=True, text=True, capture_output=True, **kwargs)


def capture(args: argparse.Namespace) -> int:
    # Host-only import: the isolated Python 3.11 container runs _capture, and
    # deliberately has no access to the repository's host conversion package.
    from director64.iso import extract
    from director64.project import GameSpec

    game_spec = GameSpec.load("findus-workshop")
    game_spec.activate()
    replay_path = (args.replay or game_spec.directory / "tests/reference/boot.json").resolve()
    replay = read_replay(replay_path)
    source = (args.source or game_spec.work / "extracted").resolve(strict=True)
    iso = (args.iso or game_spec.media).resolve(strict=True)
    extract(iso, source, expected_sha256=game_spec.source["sha256"])
    for relative in ("ANNAT/PETT16.EXE", "ANNAT/DATA/FILEIO.DLL", "POFMEDIA/GINTRO.DXR"):
        if not (source / relative).is_file():
            raise ValueError(f"original source file missing: {relative}")
    output = (
        args.output
        or game_spec.work
        / "reference"
        / (datetime.now(UTC).strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:8])
    ).resolve()
    # Prefixes, copied saves, and licensed captures must stay in the ignored local tree.
    if not output.is_relative_to(game_spec.work / "reference"):
        raise ValueError(
            "output must be inside the selected source workspace’s reference directory"
        )
    output.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(replay_path, output / "replay.json")
    runner_directory = output / "runner"
    runner_directory.mkdir()
    shutil.copyfile(Path(__file__), runner_directory / "reference_runner.py")
    image = json.loads(
        command([os.environ.get("DOCKER") or "docker", "image", "inspect", args.image]).stdout
    )[0]
    source_hashes = {
        path.relative_to(source).as_posix(): digest(path)
        for path in sorted(source.rglob("*"))
        if path.is_file()
    }
    metadata = {
        "schema_version": SCHEMA,
        "kind": "original-reference-capture",
        "run_id": output.name,
        "created_utc": datetime.now(UTC).isoformat(),
        "runner": {
            "name": "wine-xvfb",
            "verified": False,
            "image_id": image["Id"],
            "image_name": args.image,
            "script_sha256": digest(runner_directory / "reference_runner.py"),
            "windows_version": args.windows_version,
            "display": {"width": 640, "height": 480, "depth": 24},
        },
        "source": {"iso_sha256": digest(iso), "files": source_hashes},
        "replay": {"id": replay["id"], "sha256": digest(replay_path)},
        "limitations": [
            "Wine is not qualified as equivalent to the original supported Windows runtime.",
            "Input uses host monotonic time; semantic movie/frame synchronization is unobserved.",
            "Original random seed, state hashes, sprite state and Director ticks are unobserved.",
            "No scene tolerance or fidelity claim is derived from successful media capture.",
        ],
    }
    write_json(output / "provenance.json", metadata)
    container = "director64-reference-" + uuid.uuid4().hex[:12]
    argv = [
        (os.environ.get("DOCKER") or "docker"),
        "run",
        "--rm",
        "--name",
        container,
        "--network",
        "none",
        "--user",
        f"{os.getuid()}:{os.getgid()}",
        "--cap-drop",
        "ALL",
        "--security-opt",
        "no-new-privileges",
        "--pids-limit",
        "128",
        "--env",
        "WINEPREFIX=/session/wine",
        "--env",
        "XDG_CACHE_HOME=/session/cache",
        "--env",
        "XDG_CONFIG_HOME=/session/config",
        "--env",
        "XDG_RUNTIME_DIR=/session/runtime",
        "--env",
        "PULSE_SERVER=unix:/session/pulse.sock",
        "--env",
        "PULSE_SINK=reference",
        "--env",
        "PULSE_SOURCE=reference.monitor",
        "--volume",
        f"{source}:/media/findus:ro",
        "--volume",
        f"{runner_directory}:/runner:ro",
        "--volume",
        f"{output}:/session",
        image["Id"],
        "--windows-version",
        args.windows_version,
    ]
    try:
        with (output / "container.log").open("w") as log:
            result = subprocess.run(
                argv,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=replay["duration_ms"] / 1000 + 180,
                check=False,
            )
    except INTERRUPT_ERRORS:
        subprocess.run(
            [(os.environ.get("DOCKER") or "docker"), "stop", "--time", "3", container],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        print(f"Capture interrupted; diagnostics retained in {output}", file=sys.stderr)
        return 1
    if not (output / "capture.json").is_file():
        write_json(
            output / "capture.json",
            {
                "schema_version": SCHEMA,
                "capture_complete": False,
                "runner_verified": False,
                "semantic_assertions": [],
                "error": f"container exited {result.returncode} without a capture record",
            },
        )
    print(output / "capture.json")
    return result.returncode


def visible_windows() -> list[dict]:
    found = subprocess.run(
        ["xdotool", "search", "--onlyvisible", "--name", ".*"],
        capture_output=True,
        text=True,
        check=False,
    )
    windows = []
    for identifier in found.stdout.splitlines():
        name = subprocess.run(
            ["xdotool", "getwindowname", identifier], capture_output=True, text=True, check=False
        ).stdout.strip()
        if name:
            windows.append({"id": identifier, "name": name})
    return windows


def stop(process: subprocess.Popen, graceful: bool = False) -> None:
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGINT if graceful else signal.SIGTERM)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def container_capture(args: argparse.Namespace) -> int:
    session = Path("/session")
    replay = read_replay(session / "replay.json")
    processes = []
    logs = []
    checkpoints = []
    performed = []
    result = {
        "schema_version": SCHEMA,
        "capture_complete": False,
        "semantic_assertions": [],
        "runner_verified": False,
    }

    def launch(argv: list[str], log_name: str, **kwargs) -> subprocess.Popen:
        log = (session / log_name).open("w")
        logs.append(log)
        process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, **kwargs)
        processes.append(process)
        return process

    try:
        for directory in ("runtime", "cache", "config"):
            (session / directory).mkdir(mode=0o700)
        launch(["Xvfb", ":99", "-screen", "0", "640x480x24", "-nolisten", "tcp"], "xvfb.log")
        deadline = time.monotonic() + 10
        while not Path("/tmp/.X11-unix/X99").exists():
            if time.monotonic() > deadline:
                raise RuntimeError("Xvfb startup timeout")
            time.sleep(0.05)
        boot = command(["wineboot", "-u"], timeout=90)
        (session / "wineboot.log").write_text(boot.stdout + boot.stderr, encoding="utf-8")
        command(
            [
                "wine",
                "reg",
                "add",
                r"HKCU\Software\Wine",
                "/v",
                "Version",
                "/d",
                args.windows_version,
                "/f",
            ],
            timeout=30,
        )
        game = session / "wine" / "drive_c" / "Findus"
        game.mkdir()
        shutil.copy2("/media/findus/ANNAT/PETT16.EXE", game / "Findus.exe")
        shutil.copytree("/media/findus/ANNAT/DATA", game / "DATA")
        result["initial_save_hashes"] = {
            path.name: digest(path) for path in sorted((game / "DATA").glob("*.TXT"))
        }
        (session / "wine" / "dosdevices" / "d:").symlink_to("/media/findus")
        pulse = launch(
            [
                "pulseaudio",
                "-n",
                "--daemonize=no",
                "--exit-idle-time=-1",
                "--load=module-native-protocol-unix auth-anonymous=1 socket=/session/pulse.sock",
                "--load=module-null-sink sink_name=reference rate=44100 channels=2",
            ],
            "pulse.log",
        )
        deadline = time.monotonic() + 10
        while not (session / "pulse.sock").exists():
            if pulse.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("isolated PulseAudio startup failed; inspect pulse.log")
            time.sleep(0.05)
        command(["pactl", "set-default-sink", "reference"])
        command(
            [
                "wine",
                "reg",
                "add",
                r"HKCU\Software\Wine\Drivers",
                "/v",
                "Audio",
                "/d",
                "pulse",
                "/f",
            ],
            timeout=30,
        )
        recorder = launch(
            [
                "ffmpeg",
                "-nostdin",
                "-hide_banner",
                "-loglevel",
                "warning",
                "-thread_queue_size",
                "512",
                "-f",
                "x11grab",
                "-video_size",
                "640x480",
                "-framerate",
                "30",
                "-i",
                ":99.0",
                "-thread_queue_size",
                "512",
                "-f",
                "pulse",
                "-sample_rate",
                "44100",
                "-channels",
                "2",
                "-i",
                "reference.monitor",
                "-c:v",
                "ffv1",
                "-level",
                "3",
                "-pix_fmt",
                "bgr0",
                "-c:a",
                "pcm_s16le",
                str(session / "original.mkv"),
            ],
            "ffmpeg.log",
        )
        time.sleep(0.5)
        if recorder.poll() is not None:
            raise RuntimeError("lossless recorder failed to start; inspect ffmpeg.log")
        launch(["wine", r"C:\Findus\Findus.exe"], "wine.log", cwd=game)
        epoch = time.monotonic()
        result["tools"] = {
            name: command(argv).stdout.splitlines()[0]
            for name, argv in {
                "wine": ["wine", "--version"],
                "ffmpeg": ["ffmpeg", "-version"],
            }.items()
        }
        for action in replay["actions"]:
            delay = epoch + action["at_ms"] / 1000 - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            actual = round((time.monotonic() - epoch) * 1000, 3)
            kind = action["type"]
            if kind == "pointer":
                command(["xdotool", "mousemove", str(action["x"]), str(action["y"])])
            elif kind in {"button_down", "button_up"}:
                command(
                    [
                        "xdotool",
                        "mousedown" if kind == "button_down" else "mouseup",
                        "1" if action["button"] == "left" else "3",
                    ]
                )
            elif kind == "key":
                command(["xdotool", "key", action["key"]])
            else:
                png = session / f"{action['id']}.png"
                command(["import", "-window", "root", str(png)])
                checkpoints.append(
                    {
                        "id": action["id"],
                        "at_ms": actual,
                        "path": png.name,
                        "sha256": digest(png),
                        "windows": visible_windows(),
                        "semantic_status": "unobserved",
                    }
                )
            performed.append({**action, "actual_at_ms": actual})
        delay = epoch + replay["duration_ms"] / 1000 - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        stop(recorder, graceful=True)
        if recorder.returncode not in {0, 255}:
            raise RuntimeError(f"recorder exited {recorder.returncode}; inspect ffmpeg.log")
        probe = json.loads(
            command(
                [
                    "ffprobe",
                    "-v",
                    "error",
                    "-count_frames",
                    "-show_streams",
                    "-show_format",
                    "-of",
                    "json",
                    str(session / "original.mkv"),
                ]
            ).stdout
        )
        video = next(item for item in probe["streams"] if item["codec_type"] == "video")
        audio = next(item for item in probe["streams"] if item["codec_type"] == "audio")
        if (video["codec_name"], video["width"], video["height"]) != ("ffv1", 640, 480):
            raise RuntimeError("capture must contain lossless 640x480 FFV1 video")
        if audio["codec_name"] != "pcm_s16le" or int(video["nb_read_frames"]) < 1:
            raise RuntimeError("capture must contain decoded video and PCM audio")
        command(
            [
                "ffmpeg",
                "-v",
                "error",
                "-i",
                str(session / "original.mkv"),
                "-map",
                "0:a:0",
                "-c:a",
                "copy",
                str(session / "original.wav"),
            ]
        )
        write_json(session / "media.json", probe)
        result.update(
            {
                "capture_complete": True,
                "media": {
                    name: digest(session / name)
                    for name in ("original.mkv", "original.wav", "media.json")
                },
            }
        )
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
        if isinstance(error, subprocess.CalledProcessError):
            result["command_stderr"] = error.stderr
    finally:
        for process in reversed(processes):
            stop(process)
        for log in logs:
            log.close()
        result["checkpoints"] = checkpoints
        result["actions"] = performed
        data = session / "wine" / "drive_c" / "Findus" / "DATA"
        result["final_save_hashes"] = {
            path.name: digest(path) for path in sorted(data.glob("*.TXT"))
        }
        result["logs"] = {
            path.name: digest(path)
            for path in session.glob("*.log")
            if path.name != "container.log"
        }
        write_json(session / "capture.json", result)
    return 0 if result["capture_complete"] else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    host = sub.add_parser("capture", help="run a new capture with a fresh private Wine prefix")
    host.add_argument("--source", type=Path)
    host.add_argument("--iso", type=Path)
    host.add_argument("--replay", type=Path)
    host.add_argument(
        "--image", default=os.getenv("DIRECTOR64_REFERENCE_IMAGE", "director64-reference:local")
    )
    host.add_argument(
        "--output",
        type=Path,
        default=None,
    )
    host.add_argument(
        "--windows-version", choices=("win31", "win95", "win98", "win7"), default="win95"
    )
    internal = sub.add_parser("_capture", help=argparse.SUPPRESS)
    internal.add_argument("--windows-version", default="win95")
    args = parser.parse_args()
    try:
        return capture(args) if args.operation == "capture" else container_capture(args)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"reference capture: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

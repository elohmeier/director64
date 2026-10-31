"""Replay player input through an isolated Android test APK and retain device evidence.

Observations are read-only snapshots published by the game thread. Pointer and
text actions enter through Android's normal input dispatch; no gameplay RPC is
accepted. Wall-clock replay does not claim tick-for-tick parity with the probe.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import shlex
import struct
import subprocess
import time
import uuid
from pathlib import Path

from . import android, android_pacing, journeys, web
from .project import GameSpec


def png_size(data: bytes) -> tuple[int, int]:
    if len(data) < 33 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError("Android screenshot is not a complete PNG header")
    width, height = struct.unpack(">II", data[16:24])
    if not 1 <= width <= 8192 or not 1 <= height <= 8192:
        raise ValueError("Android screenshot dimensions are invalid")
    return width, height


def screen_point(point: tuple[int, int] | list[int], surface: list[int],
                 stage: tuple[int, int]) -> tuple[int, int]:
    x, y = point
    left, top, width, height = surface
    if not (0 <= x < stage[0] and 0 <= y < stage[1] and width > 0 and height > 0):
        raise ValueError(f"point {point} is outside the stage {stage}")
    return left + x * width // stage[0], top + y * height // stage[1]


def audio_interval(before: dict, after: dict, rules: dict) -> dict:
    """Judge actual AAudio callback signal, separately from acoustic output."""
    if not before or not after or before["rate"] != after["rate"]:
        raise ValueError("Android audio callback measurements are missing or incomparable")
    delta = {key: after[key] - before[key] for key in ("samples", "nonzero", "clipped", "energy")}
    if any(value < 0 for value in delta.values()) or not delta["samples"]:
        raise ValueError("Android audio callback produced no comparable active output")
    delta["seconds"] = delta["samples"] / (after["rate"] * after["channels"])
    delta["rms"] = math.sqrt(delta["energy"] / delta["samples"])
    delta["clipped_ratio"] = delta["clipped"] / delta["samples"]
    checks = {"min_rms": delta["rms"] >= rules.get("min_rms", 0),
              "max_rms": delta["rms"] <= rules.get("max_rms", 1),
              "min_seconds": delta["seconds"] >= rules.get("min_seconds", 0),
              "min_nonzero_samples": delta["nonzero"] >= rules.get("min_nonzero_samples", 0),
              "max_clipped_ratio": delta["clipped_ratio"] <= rules.get("max_clipped_ratio", 1)}
    failed = [key for key, passing in checks.items() if not passing]
    if failed:
        raise ValueError("Android audio signal check failed: " + ", ".join(failed))
    return delta


def replay_plan(data: dict) -> dict:
    """Reuse a recorded probe input stream, preserving checkpoint destinations."""
    commands = data.get("commands")
    if not isinstance(commands, list) or not commands:
        raise ValueError("replay needs a nonempty commands list")
    labels = {item["state"]: item["label"] for item in data.get("checkpoints", [])}
    steps = []
    for index, command in enumerate(commands):
        action = {"command": command}
        if index + 1 in labels:
            action["label"] = labels[index + 1]
            states = data.get("states", [])
            if index + 1 < len(states) and "movie" in states[index + 1]:
                action["expect"] = {"movie": states[index + 1]["movie"]}
        steps.append(action)
    return {"name": "recorded-replay", "steps": steps}


def validate_plan(plan: dict) -> None:
    if not isinstance(plan.get("steps"), list) or not plan["steps"]:
        raise ValueError("Android validation needs a journey with steps; for legacy journeys "
                         "run the host journey and pass its replay.json via --replay")
    allowed = {"label", "command", "idle", "click", "sprite", "settle", "when", "wait", "ticks",
               "expect", "android_key", "lifecycle", "sequence", "expect_android", "audio"}
    labels = {"boot", "complete"}
    for step in plan["steps"]:
        if not isinstance(step, dict) or set(step) - allowed:
            raise ValueError(f"unknown Android journey action: {step}")
        actions = set(step) & {"command", "idle", "click", "sprite", "android_key", "lifecycle"}
        if len(actions) > 1:
            raise ValueError("a journey step may contain only one input action")
        if "label" in step and not re.fullmatch(r"[a-zA-Z0-9_-]{1,80}", step["label"]):
            raise ValueError("checkpoint labels must be safe filenames")
        if "label" in step:
            if step["label"] in labels:
                raise ValueError("checkpoint labels must be unique and cannot be boot or complete")
            labels.add(step["label"])
        for field in ("expect", "wait", "when", "expect_android"):
            if field in step and not isinstance(step[field], dict):
                raise ValueError(f"{field} must be a state expectation object")
        if "sprite" in step and (type(step["sprite"]) is not int or step["sprite"] < 1):
            raise ValueError("sprite must be a positive channel number")
        if "click" in step and (not isinstance(step["click"], list) or len(step["click"]) != 2
                                or any(type(v) is not int for v in step["click"])):
            raise ValueError("click must contain two integer stage coordinates")
        if "command" in step:
            command = step["command"]
            if not isinstance(command, str) or "\n" in command or "\r" in command:
                raise ValueError("invalid replay command")
            if not (re.fullmatch(r"step \d+ -?\d+ -?\d+ [01]", command)
                    or re.fullmatch(r"text \d+ [\x20-\x7e]{1,20}", command)
                    or command in {"reboot", "document cancel"}):
                raise ValueError(f"Android cannot faithfully replay {command!r}; use stage "
                                 "pointer actions, text, reboot, document cancel or explicit "
                                 "android_key actions")
        if "lifecycle" in step and step["lifecycle"] not in {"home", "resume", "reboot", "menu"}:
            raise ValueError("lifecycle must be home, resume, reboot or menu")
        if "android_key" in step:
            key = step["android_key"]
            if not isinstance(key, dict) or set(key) - {"source", "code", "longpress"}:
                raise ValueError("android_key needs source, code and optional longpress")
            if key.get("source") not in {"keyboard", "dpad", "gamepad"} or not re.fullmatch(
                    r"KEYCODE_[A-Z0-9_]+", key.get("code", "")):
                raise ValueError("invalid Android key source or code")
            if "longpress" in key and type(key["longpress"]) is not bool:
                raise ValueError("android_key longpress must be boolean")
        for key in ("idle", "settle", "ticks"):
            if key in step and (type(step[key]) is not int or not 0 <= step[key] <= 100000):
                raise ValueError(f"{key} must be 0..100000 service ticks")
        if "sequence" in step:
            sequence = step["sequence"]
            if not isinstance(sequence, dict) or set(sequence) - {"seconds", "frames", "motion"}:
                raise ValueError("sequence needs seconds, frames and optional motion")
            if not (0 < sequence.get("seconds", 0) <= 60
                    and type(sequence.get("frames")) is int and 2 <= sequence["frames"] <= 120):
                raise ValueError("sequence must sample 2..120 frames over at most 60 seconds")
        if "audio" in step:
            audio = step["audio"]
            if not isinstance(audio, dict) or set(audio) - {
                "min_rms", "max_rms", "min_seconds", "min_nonzero_samples", "max_clipped_ratio"
            } or any(type(v) not in (int, float) or not math.isfinite(v) or v < 0
                     for v in audio.values()):
                raise ValueError("invalid Android audio signal rules")


class Adb:
    def __init__(self, root: Path, *, emulator: bool = False, serial: str | None = None):
        if emulator and serial:
            raise ValueError("choose --emulator or --serial")
        if not emulator and not serial:
            raise ValueError("select one Android target with --emulator or --serial")
        self.root, self.emulator, self.serial = root, emulator, serial
        self.prefix = ([str(root / ".mise-tasks/android-emulator"), "adb"] if emulator
                       else ["adb", "-s", serial])
        self.deadline = float("inf")

    def run(self, *args: str, binary: bool = False, check: bool = True, timeout: float = 30):
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("ADB command timeout must be finite and positive")
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise ValueError("Android journey exceeded its wall-clock deadline")
        result = subprocess.run([*self.prefix, *map(str, args)], capture_output=True,
                                text=not binary, check=False, cwd=self.root,
                                timeout=min(timeout, remaining))
        if check and result.returncode:
            error = result.stderr.decode(errors="replace") if binary else result.stderr
            raise ValueError(f"ADB {' '.join(args[:3])} failed: {error.strip()}")
        return result

    def shell(self, *args: str, **kwargs):
        return self.run("shell", shlex.join(map(str, args)), **kwargs)

    def identity(self) -> dict:
        if self.run("get-state").stdout.strip() != "device":
            raise ValueError("selected Android device is not ready")
        properties = {key: self.shell("getprop", key).stdout.strip() for key in (
            "ro.product.model", "ro.build.fingerprint", "ro.product.cpu.abilist", "ro.kernel.qemu"
        )}
        emulated = properties["ro.kernel.qemu"] == "1"
        if self.emulator and not emulated:
            raise ValueError("emulator transport selected a physical device")
        return {"transport": "isolated-container" if self.emulator else self.serial,
                "kind": "android-emulator" if emulated else "physical-android",
                "properties": properties}


class Session:
    def __init__(self, game: GameSpec, adb: Adb, output: Path, *, build_id: str | None = None):
        self.game, self.adb, self.output = game, adb, output
        self.package = android.package_name(game, validation=True)
        self.component = self.package + "/org.director64.tv.GameActivity"
        self.stage = (800, 600) if web.native_stage(game) else (640, 480)
        self.surface: list[int] | None = None
        self.down = False
        self.last_point: tuple[int, int] | None = None
        self.checkpoints: list[dict] = []
        self.observations: list[dict] = []
        self.audio_intervals: list[dict] = []
        self.commands = output / "commands.jsonl"
        self.commands.touch()
        self.pids: set[str] = set()
        self.build_id = build_id

    def record(self, action: dict) -> None:
        # Append before execution, including the input that crashes or hangs.
        with self.commands.open("a") as stream:
            stream.write(json.dumps({"at_monotonic": time.monotonic(), **action}) + "\n")

    def launch(self, *, reboot: bool = False) -> None:
        if reboot:
            self.adb.shell("am", "force-stop", self.package)
        result = self.adb.shell("am", "start", "-W", "-n", self.component)
        if "Error:" in result.stdout:
            raise ValueError(result.stdout.strip())
        self.down = False
        self.last_point = None
        self.pids.update(self.adb.shell("pidof", self.package).stdout.split())
        self.observe(timeout=30)

    def observe(self, *, timeout: float = 10) -> dict:
        token = uuid.uuid4().hex
        self.adb.shell("am", "broadcast", "-a", self.package + ".OBSERVE", "-p", self.package,
                       "--es", "token", token)
        limit = min(time.monotonic() + timeout, self.adb.deadline)
        while time.monotonic() < limit:
            result = self.adb.shell("run-as", self.package, "cat",
                                    f"files/validation/{token}.json", check=False)
            if result.returncode == 0:
                try:
                    observation = json.loads(result.stdout)
                except json.JSONDecodeError as error:
                    raise ValueError("Android published an invalid observation") from error
                self.adb.shell("run-as", self.package, "rm", f"files/validation/{token}.json")
                self.observations.append(observation)
                self.surface = observation["surface"]
                if self.build_id and observation.get("build_id") != self.build_id:
                    raise ValueError("installed validation APK differs from the current build; "
                                     "install it again without --no-install")
                if observation["runtime_status"] >= 0:
                    raise ValueError(f"Android runtime stopped ({observation['runtime_status']}): "
                                     + observation["runtime_error"])
                state = observation.get("state")
                if not isinstance(state, dict):
                    raise ValueError("Android runtime did not publish a state snapshot")
                if state.get("error") or state.get("script_errors") or state.get("quit"):
                    raise ValueError(state.get("error") or state.get("last_script_error")
                                     or "Android game unexpectedly quit")
                return observation
            time.sleep(0.05)
        raise ValueError("Android observation timed out; inspect logcat and app process status")

    def idle(self, ticks: int) -> dict:
        end = min(time.monotonic() + ticks / 60, self.adb.deadline)
        while time.monotonic() < end:
            time.sleep(min(1, end - time.monotonic()))
            if end - time.monotonic() > 2:
                self.observe()
        return self.observe()

    def pointer(self, ticks: int, x: int, y: int, down: bool) -> dict:
        if self.surface is None:
            self.observe()
        screen = screen_point((x, y), self.surface, self.stage)
        if down != self.down or screen != self.last_point:
            source = "touchscreen" if down or self.down else "mouse"
            action = ("DOWN" if down and not self.down else "UP"
                      if self.down and not down else "MOVE")
            self.adb.shell("input", source, "motionevent", action, *map(str, screen))
            self.down, self.last_point = down, screen
        return self.idle(ticks)

    def sprite(self, state: dict, number: int) -> tuple[int, int]:
        sprite = next((s for s in state.get("sprites", []) if s["id"] == number), None)
        if not sprite:
            raise ValueError(f"sprite {number} is not visible")
        left, top, right, bottom = sprite["bounds"]
        left, top = max(0, left), max(0, top)
        right, bottom = min(self.stage[0], right), min(self.stage[1], bottom)
        if left >= right or top >= bottom:
            raise ValueError(f"sprite {number} is outside the stage")
        return (left + right) // 2, (top + bottom) // 2

    def text(self, number: int, text: str) -> dict:
        if not re.fullmatch(r"[a-zA-Z0-9 _.-]{1,20}", text):
            raise ValueError("ADB text replay supports 1..20 ASCII name characters; use a "
                             "physical keyboard scenario for other character sets")
        observation = self.observe()
        if not observation["text_entry"]:
            limit = min(time.monotonic() + 30, self.adb.deadline)
            while True:
                try:
                    x, y = self.sprite(observation["state"], number)
                    break
                except ValueError:
                    if time.monotonic() >= limit:
                        raise
                    observation = self.idle(6)
            self.pointer(12, x, y, True)
            self.pointer(30, x, y, False)
            observation = self.observe()
            limit = min(time.monotonic() + 5, self.adb.deadline)
            while not observation["text_entry"] and time.monotonic() < limit:
                observation = self.idle(6)
        if not observation["text_entry"]:
            raise ValueError(f"clicking editable sprite {number} did not open the Android keyboard")
        # These are normal EditText key events; no native text commit bypass.
        self.adb.shell("input", "keycombination", "113", "29")  # Ctrl+A
        self.adb.shell("input", "text", text.replace(" ", "%s"))
        self.adb.shell("input", "keyevent", "66")  # IME action / Enter
        return self.idle(60)

    def command(self, command: str) -> dict:
        fields = command.split(" ", 2)
        if fields[0] == "step":
            _, ticks, x, y, down = command.split()
            return self.pointer(int(ticks), int(x), int(y), down == "1")
        if fields[0] == "text":
            return self.text(int(fields[1]), fields[2])
        if command == "reboot":
            self.launch(reboot=True)
            return self.observe()
        if command == "document cancel":
            observation = self.observe()
            identity = (observation.get("document_id"), observation.get("document_kind"))
            if not identity[0] or identity[1] not in {1, 2, 4}:
                raise ValueError("document cancel requires an active Android file or print dialog")
            for _ in range(4):
                self.record({"document": "cancel", "id": identity[0], "kind": identity[1],
                             "transport": "Android KEYCODE_BACK"})
                self.adb.shell("input", "keyevent", "KEYCODE_BACK")
                time.sleep(0.25)
                observation = self.observe()
                if (observation.get("document_id"), observation.get("document_kind")) != identity:
                    return self.idle(6)
            raise ValueError("Android did not close the active document dialog")
        raise ValueError(f"unsupported Android replay command: {command}")

    def capture(self, label: str) -> dict:
        observation = self.observe()
        result = self.adb.run("exec-out", "screencap -p", binary=True)
        width, height = png_size(result.stdout)
        full = self.output / f"{label}.screen.png"
        full.write_bytes(result.stdout)
        left, top, w, h = observation["surface"]
        if not (0 <= left < width and 0 <= top < height and w > 0 and h > 0
                and left + w <= width and top + h <= height):
            raise ValueError("Android stage bounds are outside its screenshot")
        stage = self.output / f"{label}.stage.png"
        remaining = self.adb.deadline - time.monotonic()
        if remaining <= 0:
            raise ValueError("Android journey exceeded its wall-clock deadline during capture")
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(full),
                        "-vf", f"crop={w}:{h}:{left}:{top},scale={self.stage[0]}:{self.stage[1]}",
                        "-frames:v", "1", str(stage)], check=True, timeout=min(30, remaining))
        if png_size(stage.read_bytes()) != self.stage:
            raise ValueError("captured Android stage has incorrect dimensions")
        item = {"label": label, "android": observation, "observation": {}, "screen": full.name,
                "stage": stage.name, "sha256": web.sha256(stage),
                "scope": ("system-ui" if observation.get("paused")
                          or not observation.get("has_surface", True) else "stage-viewport"),
                "frames": [{"png": stage.name, "state": observation["state"]}]}
        self.checkpoints.append(item)
        return item

    def sequence(self, label: str, spec: dict) -> dict:
        from .visual import read_png, sequence_findings

        frames = []
        begin = time.monotonic()
        for index in range(spec["frames"]):
            target = begin + index * spec["seconds"] / (spec["frames"] - 1)
            delay = target - time.monotonic()
            if delay > 0:
                limit = min(target, self.adb.deadline)
                while time.monotonic() < limit:
                    time.sleep(min(1, limit - time.monotonic()))
            item = self.capture(f"{label}-frame-{index:03}")
            self.checkpoints.pop()
            item["elapsed_ms"] = round((time.monotonic() - begin) * 1000, 3)
            frames.append(item)
        unique = len({item["sha256"] for item in frames})
        rules = {"max_blank_frames": 0}
        if spec.get("motion") is True:
            rules["min_changed_frames"] = 1
        pixels = [read_png(self.output / item["stage"])[2] for item in frames]
        findings = sequence_findings(pixels, *self.stage, rules)
        if spec.get("motion") is False and any(frame != pixels[0] for frame in pixels[1:]):
            findings.append("expected a stable stage but captured frames differ")
        checkpoint = {"label": label, "frames": [item["frames"][0] for item in frames],
                      "observation": {"sequence": rules}, "samples": frames,
                      "unique_images": unique, "findings": findings}
        self.checkpoints.append(checkpoint)
        if findings:
            raise ValueError(f"{label}: " + "; ".join(findings))
        return checkpoint

    def run(self, plan: dict) -> None:
        observation = self.observe()
        for index, action in enumerate(plan["steps"]):
            before_audio = observation.get("audio")
            if "when" in action:
                try:
                    journeys.expect(observation["state"], action["when"])
                except ValueError:
                    continue
            self.record({"step": index, "action": action})
            if "idle" in action:
                observation = self.idle(action["idle"])
            elif "click" in action or "sprite" in action:
                point = (action["click"] if "click" in action
                         else self.sprite(observation["state"], action["sprite"]))
                self.pointer(12, *point, False)
                self.pointer(4, *point, True)
                observation = self.pointer(action.get("settle", 120), *point, False)
            elif "command" in action:
                observation = self.command(action["command"])
            elif "android_key" in action:
                key = action["android_key"]
                self.adb.shell("input", key["source"], "keyevent",
                               *(["--longpress"] if key.get("longpress") else []), key["code"])
                observation = self.idle(action.get("settle", 60))
            elif "lifecycle" in action:
                lifecycle = action["lifecycle"]
                before_tick = None
                if lifecycle in {"resume", "reboot"}:
                    self.launch(reboot=lifecycle == "reboot")
                else:
                    self.adb.shell("input", "keyevent", "KEYCODE_HOME" if lifecycle == "home"
                                   else "KEYCODE_MENU")
                    expected_hold = "paused" if lifecycle == "home" else "dialog"
                    limit = min(time.monotonic() + 5, self.adb.deadline)
                    while True:
                        observation = self.observe()
                        if observation.get(expected_hold):
                            break
                        if time.monotonic() >= limit:
                            raise ValueError(f"Android {lifecycle} did not become visible")
                        time.sleep(0.05)
                    before_tick = observation["state"].get("tick")
                observation = self.idle(action.get("settle", 60))
                after_tick = observation["state"].get("tick")
                if (lifecycle in {"home", "menu"} and isinstance(before_tick, int)
                        and isinstance(after_tick, int) and after_tick - before_tick > 1):
                    raise ValueError(f"Android {lifecycle} hold kept advancing game time")
            if "wait" in action:
                limit = min(time.monotonic() + action.get("ticks", 6000) / 60,
                            self.adb.deadline)
                while True:
                    try:
                        journeys.expect(observation["state"], action["wait"])
                        break
                    except ValueError:
                        if time.monotonic() >= limit:
                            raise
                        observation = self.idle(15)
            journeys.expect(observation["state"], action.get("expect", {}))
            journeys.expect(observation, action.get("expect_android", {}))
            if "label" in action and "sequence" not in action:
                self.capture(action["label"])
            if "sequence" in action:
                label = action.get("label", f"step-{index:03}")
                self.sequence(label, action["sequence"])
            if "audio" in action:
                after_audio = self.observe().get("audio")
                metrics = audio_interval(before_audio, after_audio, action["audio"])
                self.audio_intervals.append({"step": index, **metrics})


def run(game: GameSpec, plan: dict, adb: Adb, output: Path, *, install: bool = True,
        timeout: float = 600, min_pacing_windows: int = 10, baseline: Path | None = None) -> dict:
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("Android journey timeout must be finite and positive")
    output.mkdir(parents=True, exist_ok=False)
    report = {"schema_version": 1, "game": game.slug, "source_sha256": game.source["sha256"],
              "source_image": game.image_identity, "status": "failed", "kind": "android-journey",
              "original_projector_compared": False, "hardware_qualified": False,
              "input_path": "Android input dispatch; no gameplay RPC",
              "clock": "host-monotonic; real-time Android worker",
              "plan": plan, "plan_sha256": hashlib.sha256(
                  json.dumps(plan, sort_keys=True).encode()).hexdigest(),
              "runner_sha256": web.sha256(Path(__file__)), "checkpoints": []}
    package = android.package_name(game, validation=True)
    session = None
    started = False
    try:
        validate_plan(plan)
        apk = android.verify_apk(game, validation=True)
        report["apk_sha256"] = web.sha256(apk)
        report["receipt"] = json.loads(android.apk_receipt_path(game, validation=True).read_text())
        report["device"] = adb.identity()
        adb.deadline = time.monotonic() + timeout
        if install:
            path = ("/workdir/" + apk.relative_to(game.root).as_posix()
                    if adb.emulator else str(apk))
            installed = adb.run("install", "-r", "-t", path, timeout=180)
            if "Success" not in installed.stdout:
                raise ValueError("ADB did not confirm installation of the validation APK")
        # This fixed suffix and verified receipt are required before clearing.
        # Never reset or uninstall the user's normal game package.
        cleared = adb.shell("pm", "clear", package)
        if cleared.stdout.strip() != "Success":
            raise ValueError("Android did not clear the isolated validation save data")
        report["log_start"] = adb.shell("date", "+%m-%d %H:%M:%S.000").stdout.strip()
        session = Session(game, adb, output, build_id=report["receipt"]["inputs"])
        started = True
        session.launch()
        session.capture("boot")
        session.run(plan)
        session.capture("complete")
        report["status"] = "passing"
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        if session:
            report["checkpoints"] = session.checkpoints
            report["audio_signal_intervals"] = session.audio_intervals
            report["process_ids"] = sorted(session.pids)
            (output / "observations.json").write_text(
                json.dumps(session.observations, indent=2) + "\n")
        if started:
            # Retain failure evidence even after the overall deadline expired.
            adb.deadline = time.monotonic() + 60
            try:
                log = adb.run("logcat", "-d", "-v", "threadtime", "-T", report["log_start"],
                              "director64:I", "AndroidRuntime:E", "libc:F", "ActivityManager:I",
                              "*:S").stdout
                (output / "logcat.txt").write_text(log)
                own_log = "\n".join(line for line in log.splitlines()
                                    if (len(line.split()) >= 3 and line.split()[2] in session.pids)
                                    or package in line)
                (output / "game-logcat.txt").write_text(own_log)
                pacing = android_pacing.analyze(own_log)
                report["pacing"] = pacing
                android_pacing.assert_pacing(pacing, min_windows=min_pacing_windows)
                if re.search(r"FATAL EXCEPTION|Fatal signal|ANR in " + re.escape(package), own_log):
                    raise ValueError("Android log contains a crash or ANR; inspect logcat.txt")
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                report["status"] = "failed"
                report["evidence_error"] = str(error)
            try:
                adb.shell("am", "force-stop", package)
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                report["cleanup_error"] = str(error)
    if session:
        capture = {"schema_version": 1,
                   "status": "captured" if report["status"] == "passing" else "failed",
                   "stage": list(session.stage), "checkpoints": session.checkpoints,
                   "clock": report["clock"], "environment": report.get("device"),
                   "provenance": {key: report[key] for key in (
                       "game", "source_sha256", "source_image", "plan_sha256", "runner_sha256")},
                   "audio_output_captured": False,
                   "error": report.get("error", report.get("evidence_error", ""))}
        (output / "capture.json").write_text(json.dumps(capture, indent=2) + "\n")
        if report["status"] == "passing":
            from .visual import assess_capture, compare_capture

            try:
                report["visual"] = (compare_capture(output, baseline, output / "differences")
                                    if baseline else assess_capture(output))
                if report["visual"]["status"] == "failed":
                    report["status"] = "failed"
            except (OSError, ValueError, KeyError) as error:
                report["status"] = "failed"
                report["visual_error"] = str(error)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main(game: GameSpec, argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="director64 android-validate", description=__doc__)
    parser.add_argument("journey", nargs="?")
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--emulator", action="store_true",
                        help="use the isolated container emulator")
    target.add_argument("--serial", help="select one explicitly connected Android target")
    plan = parser.add_mutually_exclusive_group()
    plan.add_argument("--plan", type=Path, help="journey JSON including optional Android actions")
    plan.add_argument("--replay", type=Path, help="recorded host journey replay.json")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--no-install", action="store_true")
    parser.add_argument("--min-pacing-windows", type=int, default=10)
    parser.add_argument("--baseline", type=Path,
                        help="explicitly reviewed visual baseline directory")
    parser.add_argument("--exercise-lifecycle", action="store_true",
                        help="append menu and Home/resume checks to each journey")
    parser.add_argument("--expect-animation", action="store_true",
                        help="require motion in a final five-frame sequence")
    parser.add_argument("--expect-audio", action="store_true",
                        help="require a non-silent final AAudio callback interval")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0 or args.min_pacing_windows < 1:
        parser.error("timeout must be finite and positive; minimum pacing windows must be positive")
    if args.plan:
        selected = [json.loads(args.plan.read_text())]
    elif args.replay:
        selected = [replay_plan(json.loads(args.replay.read_text()))]
    else:
        selected = [p for p in journeys.plans(game)
                    if not args.journey or p["name"] == args.journey]
    if not selected:
        parser.error("no matching journey")
    for item in selected:
        if not re.fullmatch(r"[a-zA-Z0-9_-]{1,80}", item.get("name", "")):
            parser.error("journey name must be a safe directory name")
        if not item.get("steps"):
            continue
        if args.expect_animation or args.expect_audio:
            observation = {"idle": 120, "label": "android-output"}
            if args.expect_animation:
                observation["sequence"] = {"seconds": 2, "frames": 5, "motion": True}
            if args.expect_audio:
                observation["audio"] = {"min_rms": 0.00001, "min_nonzero_samples": 100}
            item["steps"].append(observation)
        if args.exercise_lifecycle:
            item["steps"].extend([
                {"lifecycle": "menu", "settle": 60, "expect_android": {"dialog": True},
                 "label": "android-menu"},
                {"android_key": {"source": "dpad", "code": "KEYCODE_BACK"}, "settle": 60,
                 "expect_android": {"dialog": False}, "label": "android-menu-resumed"},
                {"lifecycle": "home", "settle": 120, "expect_android": {"paused": True},
                 "label": "android-home"},
                {"lifecycle": "resume", "settle": 120,
                 "expect_android": {"paused": False, "has_surface": True},
                 "label": "android-home-resumed"},
            ])
    base = args.output or game.work / "android-validation" / time.strftime("%Y%m%d-%H%M%S")
    adb = Adb(game.root, emulator=args.emulator, serial=args.serial)
    reports = [run(game, p, adb, base / p["name"], install=not args.no_install,
                   timeout=args.timeout, min_pacing_windows=args.min_pacing_windows,
                   baseline=args.baseline)
               for p in selected]
    summary = {"game": game.slug, "status": "passing" if all(
        r["status"] == "passing" for r in reports) else "failed", "results": reports}
    base.mkdir(parents=True, exist_ok=True)
    (base / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps({"game": game.slug, "status": summary["status"], "output": str(base)}))
    return int(summary["status"] != "passing")

"""Catalogued, rendered game journeys and comparable host performance reports."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import re
import signal
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

from .fuzz import Probe, ProbeFailure
from .project import GameSpec, repository


def plans(game: GameSpec) -> list[dict]:
    path = game.directory / "journeys.json"
    if not path.is_file():
        raise ValueError(f"{game.slug}: missing journeys.json")
    data = json.loads(path.read_text())
    if data.get("schema_version") != 1 or not data.get("journeys"):
        raise ValueError(f"{path}: expected schema_version 1 and nonempty journeys")
    result = data["journeys"]
    names = [p["name"] for p in result]
    if len(names) != len(set(names)):
        raise ValueError(f"{path}: duplicate journey names")
    for plan in result:
        if not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", plan["name"]):
            raise ValueError(f"{path}: invalid journey name")
        if not plan.get("covers") or not plan.get("description"):
            raise ValueError(f"{path}: journey needs a description and coverage")
        if bool(plan.get("legacy")) == bool(plan.get("steps")):
            raise ValueError(f"{path}: journey needs either legacy or steps")
        for action in plan.get("steps", []):
            if (set(action) - {"label", "command", "idle", "click", "sprite", "settle", "when",
                               "wait", "ticks", "expect"} or not
                    (set(action) & {"command", "idle", "click", "sprite", "wait", "expect"})
                    or len(set(action) & {"command", "idle", "click", "sprite"}) > 1):
                raise ValueError(f"{path}: unknown or empty journey action {action}")
    return result


def expect(state: dict, expected: dict) -> None:
    for path, wanted in expected.items():
        actual = state
        for key in path.split("."):
            actual = actual.get(key) if isinstance(actual, dict) else None
        if isinstance(wanted, dict):
            if set(wanted) == {"gte"}:
                ok = isinstance(actual, (int, float)) and actual >= wanted["gte"]
            elif set(wanted) == {"contains"}:
                ok = actual is not None and wanted["contains"] in actual
            elif set(wanted) == {"in"}:
                ok = actual in wanted["in"]
            else:
                raise ValueError(f"unknown expectation: {wanted}")
        else:
            ok = actual == wanted
        if not ok:
            raise ValueError(f"{path}: expected {wanted!r}, got {actual!r}")


def run_script(game: GameSpec, plan: dict, output: Path) -> dict:
    from .web import probe_path

    output.mkdir(parents=True, exist_ok=True)
    probe = Probe(probe_path(game), game.data["port"]["entry_movie"],
                  output / "stderr.txt", os.environ.copy())
    states, checkpoints = [], []

    def read(state):
        states.append(state)
        if state.get("quit") or state.get("script_errors"):
            raise ValueError(state.get("last_script_error") or "unexpected quit")
        return state

    width, height = plan.get("stage", [800, 600])
    center = (width // 2, height // 2)

    def send(command):
        ticks = int(command.split()[1]) if command.startswith(("step ", "pad ")) else 0
        return read(probe.send(command, ticks))

    failure = None
    try:
        read(probe.receive(60))
        for index, action in enumerate(plan["steps"]):
            if "when" in action:
                try:
                    expect(probe.state, action["when"])
                except ValueError:
                    continue
            try:
                if "command" in action:
                    command = action["command"]
                    if (not command.strip() or "\n" in command or "\r" in command or
                            command.split()[0] not in {"step", "pad", "text", "key", "reboot"}):
                        raise ValueError(f"journey cannot inject runtime state: {command}")
                    send(command)
                elif "idle" in action:
                    send(f"step {action['idle']} {center[0]} {center[1]} 0")
                elif "click" in action or "sprite" in action:
                    if "sprite" in action:
                        sprite = next(s for s in probe.state["sprites"]
                                      if s["id"] == action["sprite"])
                        left, top, right, bottom = sprite["bounds"]
                        left, right = max(0, left), min(width, right)
                        top, bottom = max(0, top), min(height, bottom)
                        if left >= right or top >= bottom:
                            raise ValueError("control has no visible bounds")
                        x, y = (left + right) // 2, (top + bottom) // 2
                    else:
                        x, y = action["click"]
                    if not (0 <= x < width and 0 <= y < height):
                        raise ValueError("control is outside the stage")
                    send(f"step 12 {x} {y} 0")
                    send(f"step 4 {x} {y} 1")
                    send(f"step {action.get('settle', 120)} {x} {y} 0")
                if "wait" in action:
                    for elapsed in range(0, action.get("ticks", 6000) + 1, 60):
                        try:
                            expect(probe.state, action["wait"])
                            break
                        except ValueError:
                            if elapsed + 60 > action.get("ticks", 6000):
                                raise
                            send(f"step 60 {center[0]} {center[1]} 0")
                expect(probe.state, action.get("expect", {}))
            except (ValueError, StopIteration, ProbeFailure) as error:
                label = action.get("label", action)
                raise ValueError(f"step {index + 1} ({label}): {error}") from error
            if "label" in action:
                checkpoints.append({"label": action["label"], "state": len(states) - 1})
    except BaseException as error:
        failure = str(error)
        raise
    finally:
        outcome = probe.close()
        (output / "replay.json").write_text(json.dumps({
            "commands": probe.commands, "states": states,
            "checkpoints": checkpoints, "error": failure,
        }, indent=2) + "\n")
        if failure is None and outcome.kind != "ok":
            raise ValueError(f"probe {outcome.kind}: {outcome.error}")
    return {"status": "passing", "checkpoints": checkpoints, "commands": len(probe.commands)}


def performance(directory: Path) -> dict:
    sessions = []
    for path in sorted(directory.glob("*.jsonl")):
        samples = [json.loads(line) for line in path.read_text().splitlines()]
        if samples:
            sessions.append(samples)
    samples = [sample for session in sessions for sample in session]
    if not samples:
        raise ValueError("probe produced no performance samples; rebuild its launcher")
    durations = sorted(s["elapsed_ms"] for s in samples if s["command"] != "boot")
    streams = [hashlib.sha256(json.dumps([s["command"] for s in session]).encode()).hexdigest()
               for session in sessions]
    scenes = {}
    for sample in samples:
        scene = scenes.setdefault(sample["movie"], {"commands": 0, "elapsed_ms": 0,
                                                    "max_command_ms": 0})
        scene["commands"] += 1
        scene["elapsed_ms"] += sample["elapsed_ms"]
        scene["max_command_ms"] = max(scene["max_command_ms"], sample["elapsed_ms"])
    return {
        "sessions": len(sessions), "samples": len(samples),
        "command_stream_sha256": sorted(streams),
        "elapsed_ms": sum(s["elapsed_ms"] for s in samples),
        "cpu_ms": sum(s["cpu_ms"] for s in samples),
        "boot_ms": sum(s["elapsed_ms"] for s in samples if s["command"] == "boot"),
        "p95_command_ms": durations[math.ceil(len(durations) * .95) - 1] if durations else 0,
        "max_command_ms": max(durations, default=0),
        "peak_rss_bytes": max(s["peak_rss_bytes"] for s in samples),
        "wasm_bytes": max(s["wasm_bytes"] for s in samples),
        "script_errors": max(s["script_errors"] for s in samples), "scenes": scenes,
    }


def compare(current: dict, baseline: dict, slowdown: float, memory_growth: float) -> list[str]:
    errors = []
    for key in ("source_sha256", "source_image", "plan_sha256", "environment"):
        if current.get(key) != baseline.get(key):
            errors.append(f"baseline {key} differs; record a matching baseline")
    if baseline.get("status") != "passing":
        errors.append("baseline journey did not pass")
    a, b = current.get("performance", {}), baseline.get("performance", {})
    if a.get("command_stream_sha256") != b.get("command_stream_sha256"):
        errors.append("baseline command stream differs; timing comparison is invalid")
    if errors:
        return errors
    for key, ratio in (("elapsed_ms", slowdown), ("boot_ms", slowdown),
                       ("p95_command_ms", slowdown), ("max_command_ms", slowdown),
                       ("peak_rss_bytes", memory_growth),
                       ("wasm_bytes", memory_growth)):
        if a[key] > b[key] * ratio:
            errors.append(f"{key}: {a[key]:.1f} exceeds baseline {b[key]:.1f} × {ratio:g}")
    for scene, old in b["scenes"].items():
        new = a["scenes"].get(scene)
        if new is None or new["commands"] != old["commands"]:
            errors.append(f"scene coverage differs: {scene}")
        elif new["elapsed_ms"] > old["elapsed_ms"] * slowdown:
            errors.append(f"scene {scene} is slower than baseline × {slowdown:g}")
    return errors


def environment() -> dict:
    return {"system": platform.system(), "machine": platform.machine(),
            "host": platform.node(), "processor": platform.processor(),
            "node": subprocess.check_output(["node", "--version"], text=True).strip(),
            "rendered": True}


def run_case(game, plan, output, timeout, baseline, slowdown, memory_growth):
    output.mkdir(parents=True, exist_ok=True)
    metrics = output / "metrics"
    metrics.mkdir(exist_ok=True)
    for path in metrics.iterdir():
        path.unlink()
    report = {
        "game": game.slug, "journey": plan["name"], "status": "failed",
        "platform": "web-probe",
        "source_sha256": game.source["sha256"], **game.image_identity,
        "plan_sha256": hashlib.sha256(json.dumps(plan, sort_keys=True).encode()).hexdigest(),
        "covers": plan["covers"], "environment": environment(),
        "performance_gate": "not-run" if baseline else "unbaselined",
        "hardware_qualified": False, "original_projector_compared": False,
    }
    if plan.get("calendar"):
        report["environment"]["calendar_date"] = datetime.now().astimezone().isoformat()[:10]
    site = game.dist / "web/build.json"
    if site.is_file():
        report["build"] = json.loads(site.read_text())
    report["runner_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    harness = game.root / "platforms/web/node"
    report["harness_sha256"] = {name: hashlib.sha256((harness / name).read_bytes()).hexdigest()
                                for name in ("probe.mjs", "metrics.mjs")
                                if (harness / name).is_file()}
    command = [sys.executable, "-m", "director64", "journey", "--game", game.slug,
               "--source", game.source["id"], plan["name"], "--worker", "--output", str(output),
               "--plan-sha256", report["plan_sha256"]]
    started = time.monotonic()
    with (output / "runner.log").open("w") as log:
        process = subprocess.Popen(command, stdout=log, stderr=log, start_new_session=True,
                                   env=os.environ | {"DIRECTOR64_PROBE_METRICS": str(metrics),
                                                     "DIRECTOR64_PROBE_STRICT": "1"})
        try:
            code = process.wait(timeout=timeout)
            if code:
                report["error"] = f"journey exited {code}; see runner.log and probe logs"
            else:
                report["status"] = "passing"
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            report["error"] = f"journey exceeded {timeout:g}s; process group terminated"
    report["wall_seconds"] = time.monotonic() - started
    if report.get("error") and (lines := (output / "runner.log").read_text().splitlines()):
        report["detail"] = lines[-1]
        logs = sorted([*output.rglob("*.log"), output / "stderr.txt"],
                      key=lambda p: p.stat().st_mtime if p.is_file() else 0, reverse=True)
        for path in logs:
            if not path.is_file():
                continue
            failures = [line for line in path.read_bytes()[-200_000:].decode(
                "utf-8", "replace").splitlines()
                        if line.startswith(("runtime failure after", "script alert after",
                                            "NATIVE_FAIL", "SUMMARY:"))]
            if failures:
                report["detail"] = failures[-1]
                break
    try:
        report["performance"] = performance(metrics)
        if report["performance"]["script_errors"]:
            report.update(status="failed", error="recovered script alerts in probe metrics")
    except (OSError, ValueError) as error:
        report.update(status="failed", metrics_error=str(error))
    if baseline and report["status"] == "passing":
        path = baseline / "report.json"
        if not path.is_file():
            report.update(status="failed", error=f"missing baseline: {path}")
        else:
            failures = compare(report, json.loads(path.read_text()), slowdown, memory_growth)
            report["performance_gate"] = "failed" if failures else "passing"
            if failures:
                report.update(status="failed", regressions=failures)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"{game.slug}/{plan['name']}: {report['status']} ({report['wall_seconds']:.1f}s)",
          flush=True)
    if report["status"] != "passing":
        print("  " + report.get("detail", report.get("error", report.get("metrics_error", ""))),
              flush=True)
        for failure in report.get("regressions", []):
            print("  " + failure, flush=True)
    return report


def main(game: GameSpec | None, argv: list[str]) -> int:
    from .cli import legacy_journey

    parser = argparse.ArgumentParser(prog="director64 journey")
    parser.add_argument("name", nargs="?", default="suite")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--baseline", type=Path, help="previous suite output directory")
    parser.add_argument("--max-slowdown", type=float, default=1.25)
    parser.add_argument("--max-memory-growth", type=float, default=1.10)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--activity", action="store_true")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--plan-sha256", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if (not math.isfinite(args.timeout) or args.timeout <= 0 or args.repeat <= 0
            or not math.isfinite(args.max_slowdown) or args.max_slowdown < 1
            or not math.isfinite(args.max_memory_growth) or args.max_memory_growth < 1):
        parser.error("timeout/repeat must be positive; regression ratios must be finite and >= 1")
    if args.activity and game and game.slug == "findus-mucklas" and args.name == "suite":
        args.name = "full_journey"
    games = [game] if game else [GameSpec.load(p.parent.name)
                                for p in sorted(repository().glob("games/*/game.toml"))]
    unavailable, cases = [], []
    for selected in games:
        if selected.data["status"] == "catalogued":
            unavailable.append({"game": selected.slug, "status": "unavailable",
                                "reason": "no port"})
            if args.list:
                print(f"{selected.slug}: unavailable (no executable port)")
            continue
        configured = plans(selected)
        if args.list:
            for plan in configured:
                print(f"{selected.slug}/{plan['name']}: {plan['description']}")
            continue
        matches = configured if args.name == "suite" else [p for p in configured
                                                          if p["name"] == args.name]
        if not matches:
            if game is None:
                raise ValueError(f"{selected.slug}: unknown journey {args.name}")
            matches = [{"name": args.name, "legacy": args.name, "covers": [args.name]}]
        if args.activity:
            if selected.slug != "findus-mucklas" or any(
                    p.get("legacy") != "full_journey" for p in matches):
                parser.error("--activity requires Mucklas' full_journey")
            matches = [p | {"activity": True} for p in matches]
        cases.extend((selected, plan) for plan in matches)
    if args.list:
        return 0
    if args.worker:
        if len(cases) != 1 or args.output is None:
            parser.error("worker requires exactly one journey and --output")
        selected, plan = cases[0]
        digest = hashlib.sha256(json.dumps(plan, sort_keys=True).encode()).hexdigest()
        if args.plan_sha256 and digest != args.plan_sha256:
            raise ValueError("journey plan changed while the suite was running; rerun the suite")
        selected.activate()
        if plan.get("legacy"):
            legacy_journey(selected, plan["legacy"], args.output,
                           plan.get("activity", False) or args.activity)
        else:
            run_script(selected, plan, args.output)
        return 0
    output = (args.output or (game.work / "journeys/suite" if game else
                              repository() / "build/journeys/suite")).resolve()
    baseline = args.baseline.resolve() if args.baseline else None
    if baseline == output:
        raise ValueError("baseline and output must be different directories")
    results = []
    for selected, plan in cases:
        for repeat in range(args.repeat):
            relative = Path(selected.slug) / plan["name"] / str(repeat + 1)
            try:
                result = run_case(selected, plan, output / relative, args.timeout,
                                  baseline / relative if baseline else None,
                                  args.max_slowdown, args.max_memory_growth)
            except (OSError, ValueError) as error:
                result = {"game": selected.slug, "journey": plan["name"],
                          "status": "failed", "error": str(error)}
                (output / relative).mkdir(parents=True, exist_ok=True)
                (output / relative / "report.json").write_text(json.dumps(result, indent=2) + "\n")
                print(f"{selected.slug}/{plan['name']}: failed: {error}", flush=True)
            results.append(result)
            output.mkdir(parents=True, exist_ok=True)
            (output / "summary.json").write_text(json.dumps({
                "schema_version": 1, "status": "passing" if all(
                    r["status"] == "passing" for r in results) else "failed",
                "results": results, "unavailable": unavailable,
                "hardware_qualified": False, "original_projector_compared": False,
            }, indent=2) + "\n")
    return int(any(r["status"] != "passing" for r in results))

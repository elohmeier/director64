"""Compare equivalent renderer and input executions, including their pixels.

This is implementation equivalence, not an original-projector or audio-output
comparison: the Node host uses nominal glyphs and records audio callbacks.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import time
from pathlib import Path

from .fuzz import Probe, ProbeFailure
from .parity import RecordingProbe, difference, normalized

INPUT = re.compile(r"(?:step|pad) (\d+) (-?\d+) (-?\d+) (\d+)\Z")


class ComparisonProbe(RecordingProbe):
    """Canonicalize handles only at shared logical observation boundaries.

    A split run observes intermediate objects a batched run never reports.
    Interning those objects would assign different comparison identities to
    identical final state. Input execution itself still uses the normal probe.
    """

    def send(self, command: str, ticks: int = 0) -> dict:
        if command == "reboot":
            self.instances = {}
        return Probe.send(self, command, ticks)

    def receive(self, timeout: float) -> dict:
        # The deliberately uncached renderer is much slower than a normal
        # fuzz probe. Bound it by this session's total deadline, not the
        # fuzzer's per-command throughput estimate.
        if self.deadline is not None:
            timeout = max(0, self.deadline - time.monotonic())
        return Probe.receive(self, timeout)


def sessions(path: Path) -> list[list[str]]:
    data = json.loads(path.read_text()) if path.suffix == ".json" else {
        "commands": path.read_text().splitlines()}
    cases = data if isinstance(data, list) else [data]
    result = []
    for case in cases:
        commands = case.get("commands") if isinstance(case, dict) else None
        if not isinstance(commands, list) or not commands:
            raise ValueError("replay needs nonempty commands in each session")
        if len(commands) > 4096:
            raise ValueError("replay exceeds 4096 commands per session")
        total_ticks = 0
        for command in commands:
            if (not isinstance(command, str) or not command.strip()
                    or "\n" in command or "\r" in command
                    or command.split()[0] not in {"step", "pad", "text", "key", "state",
                                                   "point", "reboot"}):
                raise ValueError("replay must contain player input or read-only observation")
            if command.startswith(("step ", "pad ")):
                match = INPUT.fullmatch(command)
                if not match or not 0 <= int(match[1]) <= 100_000:
                    raise ValueError(f"invalid bounded input: {command}")
                total_ticks += int(match[1])
        if total_ticks > 1_000_000:
            raise ValueError("replay exceeds 1000000 service ticks per session")
        result.append(commands)
    if not result:
        raise ValueError("replay has no sessions")
    return result


def chunks(command: str, split: bool) -> list[str]:
    match = INPUT.fullmatch(command)
    if not split or not match or int(match[1]) <= 1:
        return [command]
    kind = command.split()[0]
    return [f"{kind} 1 {match[2]} {match[3]} {match[4]}"] * int(match[1])


def observe(probe: RecordingProbe, commands: list[str]) -> dict:
    audio, frames = [], []
    state = probe.state
    for command in commands:
        match = INPUT.fullmatch(command)
        state = probe.send(command, int(match[1]) if match else 0)
        if state.get("script_errors") or state.get("quit"):
            raise ValueError(state.get("last_script_error") or "unexpected quit")
        if not re.fullmatch(r"[a-f0-9]{64}", state.get("frame_sha256", "")):
            raise ValueError("probe has no pixel observations; rebuild the web player")
        audio.extend(state.get("audio_events", []))
        if "frame_trace" not in state:
            raise ValueError("probe has no per-tick observations; rebuild the web player")
        frames.extend(state["frame_trace"])
    result = probe.renumbered(state)
    result["audio_events"] = audio
    result["frame_trace"] = frames
    return result


def compare_session(game, commands: list[str], output: Path, relation: str,
                    timeout: float = 600) -> dict:
    from .web import probe_path

    output.mkdir(parents=True, exist_ok=True)
    environment = os.environ | {"DIRECTOR64_PROBE_OBSERVATIONS": "1",
                                "DIRECTOR64_PROBE_STRICT": "1"}
    report = {"relation": relation, "status": "failed", "commands": len(commands)}
    a = b = None
    try:
        a = ComparisonProbe(probe_path(game), game.data["port"]["entry_movie"],
                            output / "a.stderr", environment | {"DIRECTOR64_PROBE_CACHE": "on"},
                            timeout=timeout)
        b = ComparisonProbe(probe_path(game), game.data["port"]["entry_movie"],
                            output / "b.stderr", environment | {
                                "DIRECTOR64_PROBE_CACHE": "off" if relation == "cache" else "on"},
                            timeout=timeout)
        initial_a, initial_b = a.renumbered(a.receive(120)), b.renumbered(b.receive(120))
        if normalized(initial_a) != normalized(initial_b):
            (output / "divergence.json").write_text(json.dumps({
                "index": -1, "command": "boot", "a": initial_a, "b": initial_b}, indent=2) + "\n")
            raise ValueError("boot differs: " + difference(initial_a, initial_b))
        if not initial_a.get("frame_sha256"):
            raise ValueError("probe has no pixel observations; rebuild the web player")
        for index, command in enumerate(commands):
            # Preserve inputs before executing them, including one that hangs.
            (output / "progress.json").write_text(json.dumps({"index": index,
                                                             "command": command}) + "\n")
            left = observe(a, [command])
            right = observe(b, chunks(command, relation == "chunks"))
            if normalized(left) != normalized(right):
                (output / "divergence.json").write_text(json.dumps({
                    "index": index, "command": command, "a": left, "b": right}, indent=2) + "\n")
                raise ValueError(f"command {index}: {command}\n{difference(left, right)}")
        report["status"] = "passing"
    except (ProbeFailure, ValueError, OSError) as error:
        report["error"] = str(error)
    finally:
        for name, probe in (("a", a), ("b", b)):
            if probe is None:
                continue
            outcome = probe.close()
            (output / f"{name}.commands.txt").write_text("\n".join(probe.commands) + "\n")
            if outcome.kind != "ok":
                report["status"] = "failed"
                report.setdefault("error", f"{name}: {outcome.kind} {outcome.error}")
                report.setdefault("shutdown_errors", []).append(
                    f"{name}: {outcome.kind} {outcome.error}")
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main(game, argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--replay", type=Path, required=True)
    parser.add_argument("--relation", choices=("cache", "chunks", "all"), default="all")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=600,
                        help="total seconds per equivalence session (default 600)")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        raise ValueError("timeout must be finite and positive")
    output = (args.output or game.work / "metamorphic").resolve()
    if not output.is_relative_to((game.root / "build").resolve()):
        raise ValueError("equivalence evidence must stay under ignored build/")
    commands = sessions(args.replay)
    output.mkdir(parents=True, exist_ok=True)
    relations = ("cache", "chunks") if args.relation == "all" else (args.relation,)
    results = [compare_session(game, stream, output / relation / str(index + 1),
                               relation, args.timeout)
               for relation in relations for index, stream in enumerate(commands)]
    build = game.dist / "web/build.json"
    report = {"schema_version": 1, "game": game.slug,
              "source_sha256": game.source["sha256"], **game.image_identity,
              "build": json.loads(build.read_text()) if build.is_file() else None,
              "replay_sha256": hashlib.sha256(args.replay.read_bytes()).hexdigest(),
              "status": "passing" if all(r["status"] == "passing" for r in results) else "failed",
              "results": results, "original_projector_compared": False,
              "hardware_qualified": False, "audible_output_compared": False}
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k not in {"build", "results"}}))
    return int(report["status"] != "passing")

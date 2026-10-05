"""Randomized controller-input fuzzing against the sanitized native probes.

Episodes boot the game like the playable ROM and inject only input a player
could produce: pointer motion within the hardware envelope, clicks, drags,
pad samples, Director key events where the probe supports them, and console
reboots. Any Lingo failure, sanitizer report, crash, hang or protocol break
is recorded with the exact command stream that reproduces it.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
import queue
import random
import re
import subprocess
import threading
import time
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path

# The shared input controller clamps the pointer to this envelope (input.c);
# coordinates outside it cannot occur on hardware and would report fake bugs.
POINTER_X = (8, 630)
POINTER_Y = (8, 470)
STDERR_TAIL = 200_000

def mucklas_key_pairs() -> list[tuple[int, int]]:
    """(code, character) pairs the Mucklas platform keyboard can emit (controls.c)."""
    letters = [0, 11, 8, 2, 14, 3, 5, 4, 34, 38, 40, 37, 46, 45, 31, 35, 12, 15, 1, 17]
    letters += [32, 9, 13, 7, 16, 6]
    pairs = [(letters[i], ord("A") + i) for i in range(26)]
    return pairs + [(49, ord(" ")), (27, ord("-")), (18, ord("1")), (51, 8), (36, 13)]


# Arrow key events mucklas_pad_keys synthesizes from the pad in KR/KB.
MUCKLAS_ARROWS = [(126, 30), (124, 29), (125, 31), (123, 28), (49, ord(" "))]


def mucklas_key_gate(state: dict) -> bool:
    """Text-entry keys only flow while the LO.DXR name keyboard is open."""
    if state.get("movie") in {"KR.DXR", "KB.DXR"}:
        return True
    return "skrivernamn" in str(state.get("globals", {}).get("lo_state", ""))

# RPC abilities differ per probe; sending an unsupported command can be a
# protocol error (the workshop probe exits), so the generator must know them.
# "keys" maps a movie to the (code, character) events the game's platform
# layer can produce there; anything else would report bugs unreachable on
# hardware (Mucklas text entry only opens on the LO.DXR name field, and the
# pad-to-key mapping only runs in the KR/KB activities).
PROBE_FEATURES = {
    "findus-workshop": {"pad": True, "buttons": 0x7F, "keys": {}},
    "findus-mucklas": {
        "pad": True,
        "buttons": 0x7FF,
        "keys": {
            "LO.DXR": mucklas_key_pairs(),
            "KR.DXR": MUCKLAS_ARROWS,
            "KB.DXR": MUCKLAS_ARROWS,
        },
        "key_gate": mucklas_key_gate,
        # Cost accounting and heap census (platforms/native/probe_stats.inc);
        # the perf sweep refuses probes without them instead of hanging.
        "perf": True,
        "heap": True,
    },
    "loewenzahn-1": {"pad": True, "buttons": 0x7FF, "keys": {}},
    "willy-werkel-cars": {
        "pad": False,
        "buttons": 0,
        "keys": {},
        # The D6 controller keyboard's character set (text_input.c); entries
        # land through dg_edit_text exactly like an accepted keyboard name.
        "text": "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.",
    },
    "lernerfolg-deutsch-1-2": {
        "pad": False,
        "buttons": 0,
        "keys": {},
        # The ASCII subset of the D10 on-screen keyboard: the RPC line is
        # UTF-8 while dg_edit_text validates windows-1252 bytes, so the
        # umlaut keys stay covered by the C contract and the baked replay.
        "text": "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.",
        "perf": True,
        "heap": True,
    },
}
DEFAULT_FEATURES = {"pad": False, "buttons": 0, "keys": {}}

SANITIZER_MARKS = (
    "ERROR: AddressSanitizer",
    "ERROR: LeakSanitizer",
    "runtime error:",
    "UndefinedBehaviorSanitizer",
)


@dataclass
class Outcome:
    kind: str  # ok | script-error | sanitizer | crash | hang | protocol
    error: str = ""
    signature: str = ""
    returncode: int | None = None


class ProbeFailure(Exception):
    def __init__(self, outcome: Outcome):
        super().__init__(outcome.error or outcome.kind)
        self.outcome = outcome


def normalize(text: str) -> str:
    return re.sub(r"0x[0-9a-f]+|\d+", "N", text)


def classify_exit(returncode: int | None, stderr: str) -> Outcome:
    for line in stderr.splitlines():
        if any(mark in line for mark in SANITIZER_MARKS):
            summary = next(
                (s for s in stderr.splitlines() if s.startswith("SUMMARY:")), line
            )
            return Outcome("sanitizer", line.strip(), normalize(summary), returncode)
    for line in stderr.splitlines():
        if line.startswith("NATIVE_FAIL"):
            return Outcome("script-error", line.strip(), normalize(line), returncode)
    if returncode is not None and returncode < 0:
        return Outcome("crash", f"signal {-returncode}", f"signal {-returncode}", returncode)
    return Outcome("protocol", f"probe exited {returncode}", f"exit {returncode}", returncode)


class Probe:
    """One sanitized probe process speaking the line-based RPC protocol."""

    def __init__(self, executable: Path, movie: str, stderr_path: Path, env: dict):
        self.stderr_path = stderr_path
        self.stderr_file = stderr_path.open("wb")
        # Probe state may carry raw Mac Roman text bytes; never die on them.
        self.process = subprocess.Popen(
            [str(executable), movie, "rpc"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr_file,
            encoding="utf-8",
            errors="replace",
            env=env,
        )
        self.lines: queue.Queue[str | None] = queue.Queue()
        self.visited: list[str] = []
        self.commands: list[str] = []
        self.movie_trail: list[str] = []
        # Recovered script alerts: (message, command index, state snapshot).
        self.soft_events: list[tuple[str, int, dict]] = []
        self.soft_count = 0
        self.state: dict = {}
        self.reader = threading.Thread(target=self._pump, daemon=True)
        self.reader.start()

    def _pump(self):
        for line in self.process.stdout:
            self.lines.put(line)
        self.lines.put(None)

    def stderr_tail(self) -> str:
        if not self.stderr_file.closed:
            self.stderr_file.flush()
        data = self.stderr_path.read_bytes()
        return data[-STDERR_TAIL:].decode("utf-8", "replace")

    def _fail_from_exit(self):
        returncode = self.process.wait(timeout=10)
        raise ProbeFailure(classify_exit(returncode, self.stderr_tail()))

    def receive(self, timeout: float) -> dict:
        deadline = time.monotonic() + timeout
        while True:
            try:
                line = self.lines.get(timeout=max(0.0, deadline - time.monotonic()))
            except queue.Empty:
                self.process.kill()
                raise ProbeFailure(Outcome("hang", self._where())) from None
            if line is None:
                self._fail_from_exit()
            if line.startswith("{"):
                try:
                    state = json.loads(line)
                except json.JSONDecodeError:
                    self.process.kill()
                    raise ProbeFailure(
                        Outcome("protocol", f"unparseable state: {line[:200]}")
                    ) from None
                self.state = state
                movie = state.get("movie")
                if movie and (not self.movie_trail or self.movie_trail[-1] != movie):
                    self.movie_trail.append(movie)
                count = state.get("script_errors", 0)
                if count > self.soft_count:
                    self.soft_count = count
                    self.soft_events.append(
                        (state.get("last_script_error", ""), len(self.commands), dict(state))
                    )
                if state.get("error"):
                    outcome = Outcome(
                        "script-error", state["error"], normalize(state["error"])
                    )
                    raise ProbeFailure(outcome)
                return state
            self.visited.append(line.strip())

    def send(self, command: str, ticks: int = 0) -> dict:
        self.commands.append(command)
        try:
            self.process.stdin.write(command + "\n")
            self.process.stdin.flush()
        except (BrokenPipeError, OSError):
            self._fail_from_exit()
        return self.receive(timeout=20.0 + ticks / 200.0)

    def close(self) -> Outcome:
        """Graceful end of episode; a dirty exit (e.g. leak report) still counts."""
        with contextlib.suppress(OSError):
            self.process.stdin.close()
        try:
            returncode = self.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=10)
            return Outcome("hang", self._where())
        finally:
            self.stderr_file.close()
        if returncode != 0:
            return classify_exit(returncode, self.stderr_path.read_text(errors="replace"))
        return Outcome("ok")

    def kill(self):
        self.process.kill()
        self.process.wait(timeout=10)
        self.stderr_file.close()

    def _where(self) -> str:
        movie = self.state.get("movie", "?")
        frame = self.state.get("frame", "?")
        return f"no response in {movie}:{frame}"


def hittable_points(state: dict, rng: random.Random) -> list[tuple[int, int]]:
    """Player-reachable points of interest from the last probe state."""
    points = []
    for sprite in state.get("sprites", []):
        left, top, right, bottom = sprite["bounds"]
        left, right = max(left, POINTER_X[0]), min(right, POINTER_X[1])
        top, bottom = max(top, POINTER_Y[0]), min(bottom, POINTER_Y[1])
        if left < right and top < bottom:
            points.append((rng.randrange(left, right), rng.randrange(top, bottom)))
    return points


def random_point(rng: random.Random) -> tuple[int, int]:
    return rng.randint(*POINTER_X), rng.randint(*POINTER_Y)


def pick_point(state: dict, rng: random.Random) -> tuple[int, int]:
    points = hittable_points(state, rng)
    if points and rng.random() < 0.8:
        return rng.choice(points)
    return random_point(rng)


def episode_commands(probe: Probe, rng: random.Random, actions: int, features: dict):
    """Drive one episode; raises ProbeFailure on the first detected bug."""
    for _ in range(actions):
        state = probe.state
        if state.get("quit"):
            probe.send("reboot", ticks=1200)
            continue
        roll = rng.random()
        if roll < 0.30:  # click something plausible, wait for the reaction
            x, y = pick_point(state, rng)
            probe.send(f"step {rng.randint(2, 25)} {x} {y} 1", ticks=25)
            probe.send(f"step {rng.randint(10, 240)} {x} {y} 0", ticks=240)
        elif roll < 0.45:  # drag between two points (moveable sprites, sliders)
            x1, y1 = pick_point(state, rng)
            x2, y2 = pick_point(state, rng) if rng.random() < 0.5 else random_point(rng)
            probe.send(f"step {rng.randint(2, 30)} {x1} {y1} 1", ticks=30)
            probe.send(f"step {rng.randint(2, 40)} {x2} {y2} 1", ticks=40)
            probe.send(f"step {rng.randint(10, 120)} {x2} {y2} 0", ticks=120)
        elif roll < 0.65:  # let the score play with the pointer parked somewhere
            x, y = random_point(rng)
            probe.send(f"step {rng.randint(30, 900)} {x} {y} 0", ticks=900)
        elif roll < 0.72:  # rapid press/release jitter at one spot
            x, y = pick_point(state, rng)
            for _ in range(rng.randint(2, 6)):
                probe.send(f"step {rng.randint(1, 4)} {x} {y} 1", ticks=4)
                probe.send(f"step {rng.randint(1, 4)} {x} {y} 0", ticks=4)
        elif features["pad"] and roll < 0.90:  # raw pad samples through the controller
            ticks = rng.randint(5, 240)
            stick_x, stick_y = rng.randint(-128, 127), rng.randint(-128, 127)
            buttons = rng.choice([0, 1, 1, 2, 64] + [rng.randint(0, features["buttons"])] * 3)
            probe.send(f"pad {ticks} {stick_x} {stick_y} {buttons}", ticks=ticks)
        elif (
            (charset := features.get("text"))
            and roll < 0.90
            and not state.get("depth")
            and not state.get("text_pending")
            and (editable := [s["id"] for s in state.get("sprites", []) if s.get("editable")])
        ):
            # Controller-keyboard entry: up to 20 keyboard characters accepted
            # into an editable field while the interpreter is idle; the source
            # keyDown/idle handlers then see the same per-character edits as
            # desktop typing. A second entry cannot start while one drains.
            text = "".join(rng.choice(charset) for _ in range(rng.randint(1, 20))).strip()
            if text:
                probe.send(f"text {rng.choice(editable)} {text}", ticks=1)
            probe.send(f"step {rng.randint(25, 120)} 320 240 0", ticks=120)
        elif (
            (key_pairs := features["keys"].get(state.get("movie", "")))
            and features.get("key_gate", lambda _: True)(state)
            and roll < 0.96
        ):
            code, character = rng.choice(key_pairs)  # in-envelope Director key events
            x, y = random_point(rng)
            probe.send(f"key {code} {character} 1", ticks=1)
            probe.send(f"step {rng.randint(1, 30)} {x} {y} 0", ticks=30)
            if rng.random() < 0.9:
                probe.send(f"key {code} {character} 0", ticks=1)
                probe.send(f"step {rng.randint(1, 30)} {x} {y} 0", ticks=30)
        elif roll < 0.98:
            x, y = random_point(rng)
            probe.send(f"step {rng.randint(1, 60)} {x} {y} {rng.randint(0, 1)}", ticks=60)
        else:  # power cycle: volatile state resets, saves persist
            probe.send("reboot", ticks=1200)


@dataclass
class Runner:
    executable: Path
    output: Path
    features: dict
    env: dict
    movies: list[str]
    actions: int
    minimize: bool
    lock: threading.Lock = field(default_factory=threading.Lock)
    signatures: dict[str, dict] = field(default_factory=dict)
    coverage: dict[str, int] = field(default_factory=dict)
    episodes: int = 0
    commands: int = 0

    def run_episode(self, seed: int) -> None:
        rng = random.Random(seed)
        movie = rng.choice(self.movies)
        stderr_path = self.output / f"episode-{seed}.stderr"
        probe = Probe(self.executable, movie, stderr_path, self.env)
        outcome = None
        try:
            probe.receive(timeout=120.0)
            episode_commands(probe, rng, self.actions, self.features)
            outcome = probe.close()
        except ProbeFailure as failure:
            outcome = failure.outcome
            probe.kill()
        finally:
            with self.lock:
                self.episodes += 1
                self.commands += len(probe.commands)
                for movie in set(probe.movie_trail):
                    self.coverage[movie] = self.coverage.get(movie, 0) + 1
        for message, index, state in probe.soft_events:
            self.record_soft(seed, movie, probe, message, index, state)
        if outcome and outcome.kind != "ok":
            self.record(seed, movie, probe, outcome)
        else:
            stderr_path.unlink(missing_ok=True)

    def record(self, seed: int, movie: str, probe: Probe, outcome: Outcome) -> None:
        signature = outcome.signature or normalize(outcome.error) or outcome.kind
        digest = hashlib.sha256(signature.encode()).hexdigest()[:8]
        case = self.output / f"case-{digest}-{outcome.kind}"
        with self.lock:
            if signature in self.signatures:
                self.signatures[signature]["count"] += 1
                known = True
            else:
                self.signatures[signature] = {
                    "kind": outcome.kind,
                    "error": outcome.error,
                    "case": str(case),
                    "count": 1,
                }
                known = False
        if known:
            probe.stderr_path.unlink(missing_ok=True)
            return
        case.mkdir(parents=True, exist_ok=True)
        (case / "commands.txt").write_text("".join(c + "\n" for c in probe.commands))
        (case / "stderr.txt").write_text(probe.stderr_tail())
        probe.stderr_path.unlink(missing_ok=True)
        commands = probe.commands
        if self.minimize and outcome.kind != "hang":
            commands = self.shrink(movie, probe.commands, signature)
            (case / "commands.min.txt").write_text("".join(c + "\n" for c in commands))
        report = {
            "kind": outcome.kind,
            "signature": signature,
            "error": outcome.error,
            "returncode": outcome.returncode,
            "movie": movie,
            "seed": seed,
            "commands": len(probe.commands),
            "minimized_commands": len(commands),
            "visited": probe.visited[-40:],
            "movie_trail": probe.movie_trail[-40:],
            "state": probe.state,
        }
        (case / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"FAIL {outcome.kind} [{signature[:120]}] -> {case}", flush=True)

    def record_soft(
        self, seed: int, movie: str, probe: Probe, message: str, index: int, state: dict
    ) -> None:
        """A recovered script alert: the game keeps playing, but record it."""
        signature = normalize(message) or "script alert"
        digest = hashlib.sha256(signature.encode()).hexdigest()[:8]
        case = self.output / f"case-{digest}-script-alert"
        with self.lock:
            if signature in self.signatures:
                self.signatures[signature]["count"] += 1
                return
            self.signatures[signature] = {
                "kind": "script-alert",
                "error": message,
                "case": str(case),
                "count": 1,
            }
        case.mkdir(parents=True, exist_ok=True)
        commands = probe.commands[:index]
        (case / "commands.txt").write_text("".join(c + "\n" for c in commands))
        (case / "stderr.txt").write_text(probe.stderr_tail())
        if self.minimize:
            commands = self.shrink(movie, commands, signature, soft=True)
            (case / "commands.min.txt").write_text("".join(c + "\n" for c in commands))
        report = {
            "kind": "script-alert",
            "signature": signature,
            "error": message,
            "movie": movie,
            "seed": seed,
            "commands": index,
            "minimized_commands": len(commands),
            "movie_trail": probe.movie_trail[-40:],
            "state": state,
        }
        (case / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"ALERT script-alert [{signature[:120]}] -> {case}", flush=True)

    def replay(self, movie: str, commands: list[str]) -> tuple[Outcome, list[str]]:
        stderr_path = self.output / f"replay-{threading.get_ident()}-{time.monotonic_ns()}.stderr"
        probe = Probe(self.executable, movie, stderr_path, self.env)
        try:
            probe.receive(timeout=120.0)
            for command in commands:
                ticks = sum(int(f) for f in command.split()[1:2] if f.isdigit())
                probe.send(command, ticks=max(ticks, 1200))
            outcome = probe.close()
        except ProbeFailure as failure:
            outcome = failure.outcome
            probe.kill()
        stderr_path.unlink(missing_ok=True)
        return outcome, [normalize(m) or "script alert" for m, _, _ in probe.soft_events]

    def shrink(
        self, movie: str, commands: list[str], signature: str, soft: bool = False
    ) -> list[str]:
        """Bounded ddmin-style chunk removal keeping the same failure signature."""

        def reproduces(candidate: list[str]) -> bool:
            outcome, soft_signatures = self.replay(movie, candidate)
            if soft:
                return signature in soft_signatures
            found = outcome.signature or normalize(outcome.error) or outcome.kind
            return outcome.kind != "ok" and found == signature

        best, budget, chunk = commands, 48, max(1, len(commands) // 2)
        while budget > 0:
            index = 0
            while index < len(best) and budget > 0:
                candidate = best[:index] + best[index + chunk :]
                budget -= 1
                if candidate and reproduces(candidate):
                    best = candidate
                else:
                    index += chunk
            if chunk == 1:
                break
            chunk = max(1, chunk // 2)
        return best


def load_case(path: Path) -> tuple[str, list[str]]:
    if path.is_dir():
        report = json.loads((path / "report.json").read_text())
        best = path / "commands.min.txt"
        commands_file = best if best.exists() else path / "commands.txt"
        return report["movie"], commands_file.read_text().splitlines()
    return "START", path.read_text().splitlines()


def main(game, argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="director64 fuzz", description="randomized native input fuzzing"
    )
    parser.add_argument("--duration", type=float, default=300.0, help="seconds of fuzzing")
    parser.add_argument("--episodes", type=int, help="stop after this many episodes")
    parser.add_argument("--actions", type=int, default=250, help="input actions per episode")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--seed", type=int, help="base seed (default: random)")
    parser.add_argument(
        "--movie", action="append", help="entry movie (repeatable, default START)"
    )
    parser.add_argument("--replay", type=Path, help="re-run a recorded case and exit")
    parser.add_argument("--no-minimize", action="store_true")
    parser.add_argument("--output", type=Path, help="artifact directory")
    args = parser.parse_args(argv)

    executable = game.work / "native/director-probe-sanitized"
    env = os.environ | {
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=1",
    }
    stamp = datetime.now(UTC).strftime("%Y%m%dT%H%M%SZ")
    output = args.output or game.work / "fuzz" / stamp
    output.mkdir(parents=True, exist_ok=True)
    runner = Runner(
        executable=executable,
        output=output,
        features=PROBE_FEATURES.get(game.slug, DEFAULT_FEATURES),
        env=env,
        movies=args.movie or [game.data["port"].get("entry_movie", "START")],
        actions=args.actions,
        minimize=not args.no_minimize,
    )
    if args.replay:
        movie, commands = load_case(args.replay)
        outcome, soft = runner.replay(movie, commands)
        print(
            json.dumps(
                {
                    "kind": outcome.kind,
                    "error": outcome.error,
                    "signature": outcome.signature,
                    "returncode": outcome.returncode,
                    "script_alerts": soft,
                }
            )
        )
        return 1 if outcome.kind != "ok" else 0

    base_seed = args.seed if args.seed is not None else random.randrange(1 << 32)
    deadline = time.monotonic() + args.duration
    counter = iter(range(base_seed, base_seed + (args.episodes or 1 << 30)))
    counter_lock = threading.Lock()

    internal_errors: list[str] = []

    def worker():
        while True:
            with counter_lock:
                seed = next(counter, None)
            if seed is None or time.monotonic() >= deadline:
                return
            try:
                runner.run_episode(seed)
            except Exception:  # noqa: BLE001 - a fuzzer bug must not end the run
                import traceback

                trace = traceback.format_exc()
                with counter_lock:
                    internal_errors.append(f"seed {seed}: {trace}")
                print(f"INTERNAL seed {seed}\n{trace}", flush=True)

    threads = [threading.Thread(target=worker) for _ in range(max(1, args.jobs))]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    summary = {
        "game": game.slug,
        "executable": str(executable),
        "base_seed": base_seed,
        "episodes": runner.episodes,
        "commands": runner.commands,
        "unique_failures": len(runner.signatures),
        "internal_errors": internal_errors,
        "coverage": dict(sorted(runner.coverage.items(), key=lambda kv: -kv[1])),
        "failures": runner.signatures,
    }
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    excluded = {"failures", "internal_errors", "coverage"}
    console = {k: v for k, v in summary.items() if k not in excluded}
    print(json.dumps(console | {"internal_errors": len(internal_errors)}))
    for signature, info in runner.signatures.items():
        print(f"{info['kind']} x{info['count']}: {signature[:140]} -> {info['case']}")
    # Recovered script alerts reproduce original behavior; only failures that
    # would stop the ROM make the run exit non-zero.
    hard = [s for s, i in runner.signatures.items() if i["kind"] != "script-alert"]
    return 1 if hard else 0

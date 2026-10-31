"""Randomized controller-input fuzzing against the game's web probe.

Episodes boot the game like the player and inject only input a player could
produce: pointer motion within the stage, clicks, drags, pad samples, text
entry and reboots. Any Lingo failure, sanitizer report, crash, hang or protocol break
is recorded with the exact command stream that reproduces it.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import math
import os
import queue
import random
import re
import signal
import subprocess
import threading
import time
from collections import Counter
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path

STDERR_TAIL = 200_000
PRELUDE_COMMANDS = 4096
PRELUDE_TICKS = 1_000_000

# What the web probe (platforms/web/node/probe.mjs) answers: one controller drives
# the pointer through the shared controller, and keyboards enter text into
# editable fields. "keys" can restrict a movie to particular (Macintosh
# virtual key code, character) pairs; keyboard-capable runtimes otherwise use
# the ordinary keys forwarded by platforms/web/site/player.js.
KEYS = ((0, 65), (1, 83), (2, 68), (3, 70), (18, 49), (19, 50),
        (36, 13), (49, 32), (51, 8), (53, 27),
        (123, 28), (124, 29), (125, 31), (126, 30))
FEATURES = {
    "pad": True,
    "buttons": 0x7FF,
    "keys": {},
    "keyboard": False,
    "stage": [640, 480],
    "text": "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_.",
}


def game_features(game) -> dict:
    from .web import native_stage

    port = game.data["port"]
    return FEATURES | {
        "stage": [800, 600] if native_stage(game) else [640, 480],
        "keyboard": port["director_version"] >= 700 or bool(port.get("full_engine")),
    }


def semantic_features(state: dict, previous_movie: str | None = None) -> set[str]:
    """Sampled state coverage; these are not executed-handler or branch counts."""
    movie = state.get("movie", "?")
    result = {f"frame:{movie}:{state.get('frame', '?')}"}
    if previous_movie and previous_movie != movie:
        result.add(f"transition:{previous_movie}->{movie}")
    if state.get("handler"):
        result.add(f"handler:{movie}:{state['handler']}:{state.get('line', '?')}")
    sprites = sorted((s.get("id"), s.get("member"), s.get("script"))
                     for s in state.get("sprites", []))
    if sprites:
        digest = hashlib.sha256(json.dumps(sprites).encode()).hexdigest()[:16]
        result.add(f"sprites:{movie}:{digest}")
    result.update(f"save:{name}" for name in state.get("saves", {}))
    if state.get("text_pending"):
        result.add(f"text-entry:{movie}")
    return result

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


class InvariantModel:
    """Explicit game expectations with optional conditions latched across input/reboot."""

    def __init__(self, specification: dict):
        if (not isinstance(specification, dict) or specification.get("schema_version") != 1 or
                not isinstance(specification.get("id"), str) or not specification["id"] or
                not isinstance(specification.get("rules"), list) or not specification["rules"] or
                set(specification) - {"schema_version", "id", "description", "rules"}):
            raise ValueError("invariant model needs schema_version 1, id and nonempty rules")
        self.specification = specification
        self.sha256 = hashlib.sha256(json.dumps(specification, sort_keys=True).encode()).hexdigest()
        self.latched: set[str] = set()
        self.checks: Counter = Counter()
        names = set()
        for rule in specification["rules"]:
            if (not isinstance(rule, dict) or set(rule) - {"id", "after", "when", "expect"} or
                    not isinstance(rule.get("id"), str) or not rule["id"] or
                    rule["id"] in names or not rule.get("expect")):
                raise ValueError("invariant rules need unique ids and nonempty expectations")
            names.add(rule["id"])
            for key in ("after", "when", "expect"):
                expected = rule.get(key, {})
                if not isinstance(expected, dict):
                    raise ValueError(f"invariant {rule['id']}: {key} must be an expectation map")
                for path, wanted in expected.items():
                    if not isinstance(path, str) or not path:
                        raise ValueError("invariant expectation paths must be nonempty strings")
                    if isinstance(wanted, dict) and set(wanted) not in (
                            {"gte"}, {"contains"}, {"in"}):
                        raise ValueError(f"invariant {rule['id']}: unknown expectation {wanted}")
                    if isinstance(wanted, dict):
                        if "gte" in wanted and not isinstance(wanted["gte"], (int, float)):
                            raise ValueError("invariant gte needs a numeric bound")
                        if "in" in wanted and not isinstance(wanted["in"], list):
                            raise ValueError("invariant in needs a list")

    def observe(self, state: dict) -> None:
        # Imported lazily: the journey runner itself uses Probe.
        from .journeys import expect

        def matches(expected):
            try:
                expect(state, expected)
            except (TypeError, ValueError):
                return False
            return True

        for rule in self.specification["rules"]:
            identifier = rule["id"]
            if "after" in rule and identifier not in self.latched:
                if not matches(rule["after"]):
                    continue
                self.latched.add(identifier)
            if not matches(rule.get("when", {})):
                continue
            self.checks[identifier] += 1
            try:
                expect(state, rule["expect"])
            except (TypeError, ValueError) as error:
                message = f"invariant {self.specification['id']}/{identifier}: {error}"
                signature = f"invariant:{self.sha256}:{identifier}:{normalize(str(error))}"
                raise ProbeFailure(Outcome("invariant", message, signature)) from error


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
        failure = re.match(r'^runtime failure after (?:"(?:\\.|[^"\\])*"|boot): (.+)$', line)
        if failure:
            message = failure[1]
            return Outcome("script-error", message, normalize(message), returncode)
    if returncode is not None and returncode < 0:
        return Outcome("crash", f"signal {-returncode}", f"signal {-returncode}", returncode)
    for line in stderr.splitlines():
        if line.startswith(("Error: ", "RuntimeError: ", "TypeError: ")):
            return Outcome("protocol", line.strip(), normalize(line), returncode)
    return Outcome("protocol", f"probe exited {returncode}", f"exit {returncode}", returncode)


class Probe:
    """One sanitized probe process speaking the line-based RPC protocol."""

    def __init__(self, executable: Path, movie: str, stderr_path: Path, env: dict,
                 invariant_spec: dict | None = None, timeout: float | None = None):
        if timeout is not None and (not math.isfinite(timeout) or timeout <= 0):
            raise ValueError("probe timeout must be finite and positive")
        self.deadline = time.monotonic() + timeout if timeout is not None else None
        self.invariants = InvariantModel(invariant_spec) if invariant_spec is not None else None
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
            start_new_session=True,
        )
        self.lines: queue.Queue[str | None] = queue.Queue()
        self.visited: list[str] = []
        self.commands: list[str] = []
        self.movie_trail: list[str] = []
        # Recovered script alerts: (message, command index, state snapshot).
        self.soft_events: list[tuple[str, int, dict]] = []
        self.soft_count = 0
        self.state: dict = {}
        self.observations: dict[str, int] = {}
        self.input_counts: Counter = Counter()
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
        if self.deadline is not None:
            deadline = min(deadline, self.deadline)
        while True:
            if time.monotonic() >= deadline:
                raise self._timeout_failure()
            try:
                line = self.lines.get(timeout=max(0.0, deadline - time.monotonic()))
            except queue.Empty:
                raise self._timeout_failure() from None
            if line is None:
                self._fail_from_exit()
            if line.startswith("{"):
                try:
                    state = json.loads(line)
                except json.JSONDecodeError:
                    self._kill_group()
                    raise ProbeFailure(
                        Outcome("protocol", f"unparseable state: {line[:200]}")
                    ) from None
                previous_movie = self.state.get("movie")
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
                for feature in semantic_features(state, previous_movie):
                    self.observations.setdefault(feature, len(self.commands))
                if self.invariants is not None:
                    self.invariants.observe(state)
                return state
            self.visited.append(line.strip())

    def send(self, command: str, ticks: int = 0) -> dict:
        self.commands.append(command)
        self.input_counts[command.split()[0]] += 1
        if command.strip() == "reboot":
            self.soft_count = 0
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
            timeout = (30 if self.deadline is None
                       else max(0, min(30, self.deadline - time.monotonic())))
            returncode = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self._kill_group()
            self.process.wait(timeout=10)
            return Outcome("hang", self._where())
        finally:
            self._kill_group()
            self.stderr_file.close()
        if returncode != 0:
            return classify_exit(returncode, self.stderr_path.read_text(errors="replace"))
        return Outcome("ok")

    def _kill_group(self):
        with contextlib.suppress(ProcessLookupError):
            os.killpg(self.process.pid, signal.SIGKILL)

    def _timeout_failure(self) -> ProbeFailure:
        self._kill_group()
        detail = self._where()
        if self.deadline is not None and time.monotonic() >= self.deadline:
            detail += "; episode/replay deadline exceeded"
        return ProbeFailure(Outcome("hang", detail))

    def kill(self):
        self._kill_group()
        self.process.wait(timeout=10)
        self.stderr_file.close()

    def _where(self) -> str:
        movie = self.state.get("movie", "?")
        frame = self.state.get("frame", "?")
        return f"no response in {movie}:{frame}"


def pointer_bounds(state: dict, features: dict | None = None) -> tuple[int, int]:
    stage = state.get("stage", (features or FEATURES).get("stage", [640, 480]))
    if (not isinstance(stage, (list, tuple)) or len(stage) != 2 or
            any(not isinstance(n, int) or isinstance(n, bool) or not 20 <= n <= 4096
                for n in stage)):
        raise ValueError(f"invalid probe stage dimensions: {stage!r}")
    return stage[0] - 10, stage[1] - 10


def hittable_points(state: dict, rng: random.Random,
                    features: dict | None = None) -> list[tuple[int, int]]:
    """Player-reachable points of interest from the last probe state."""
    points = []
    max_x, max_y = pointer_bounds(state, features)
    for sprite in state.get("sprites", []):
        left, top, right, bottom = sprite["bounds"]
        left, right = max(left, 8), min(right, max_x + 1)
        top, bottom = max(top, 8), min(bottom, max_y + 1)
        if left < right and top < bottom:
            points.append((rng.randrange(left, right), rng.randrange(top, bottom)))
    return points


def random_point(rng: random.Random, state: dict | None = None,
                 features: dict | None = None) -> tuple[int, int]:
    max_x, max_y = pointer_bounds(state or {}, features)
    return rng.randint(8, max_x), rng.randint(8, max_y)


def pick_point(state: dict, rng: random.Random, features: dict | None = None) -> tuple[int, int]:
    points = hittable_points(state, rng, features)
    if points and rng.random() < 0.8:
        return rng.choice(points)
    return random_point(rng, state, features)


def episode_commands(probe: Probe, rng: random.Random, actions: int, features: dict):
    """Drive one episode; raises ProbeFailure on the first detected bug."""
    for _ in range(actions):
        state = probe.state
        if state.get("quit"):
            probe.send("reboot", ticks=1200)
            continue
        editable = [s["id"] for s in state.get("sprites", []) if s.get("editable")]
        key_pairs = features.get("keys", {}).get(state.get("movie", ""))
        keyboard = state.get("capabilities", {}).get("keyboard", features.get("keyboard", False))
        if key_pairs is None and keyboard:
            key_pairs = KEYS
        actions_available = ["click", "drag", "idle", "jitter", "sample", "reboot"]
        weights = [30, 15, 20, 7, 3, 2]
        if features.get("pad"):
            actions_available.append("pad")
            weights.append(13)
        if (features.get("text") and editable and
                not state.get("depth") and not state.get("text_pending")):
            actions_available.append("text")
            weights.append(8)
        if key_pairs and features.get("key_gate", lambda _: True)(state):
            actions_available.append("key")
            weights.append(6)
        action = rng.choices(actions_available, weights=weights, k=1)[0]
        if action == "click":
            x, y = pick_point(state, rng, features)
            probe.send(f"step {rng.randint(2, 25)} {x} {y} 1", ticks=25)
            probe.send(f"step {rng.randint(10, 240)} {x} {y} 0", ticks=240)
        elif action == "drag":
            x1, y1 = pick_point(state, rng, features)
            x2, y2 = (pick_point(state, rng, features) if rng.random() < 0.5
                      else random_point(rng, state, features))
            probe.send(f"step {rng.randint(2, 30)} {x1} {y1} 1", ticks=30)
            probe.send(f"step {rng.randint(2, 40)} {x2} {y2} 1", ticks=40)
            probe.send(f"step {rng.randint(10, 120)} {x2} {y2} 0", ticks=120)
        elif action == "idle":
            x, y = random_point(rng, state, features)
            probe.send(f"step {rng.randint(30, 900)} {x} {y} 0", ticks=900)
        elif action == "jitter":
            x, y = pick_point(state, rng, features)
            for _ in range(rng.randint(2, 6)):
                probe.send(f"step {rng.randint(1, 4)} {x} {y} 1", ticks=4)
                probe.send(f"step {rng.randint(1, 4)} {x} {y} 0", ticks=4)
        elif action == "pad":
            ticks = rng.randint(5, 240)
            stick_x, stick_y = rng.randint(-128, 127), rng.randint(-128, 127)
            buttons = rng.choice([0, 1, 1, 2, 64] + [rng.randint(0, features["buttons"])] * 3)
            probe.send(f"pad {ticks} {stick_x} {stick_y} {buttons}", ticks=ticks)
        elif action == "text":
            # Controller-keyboard entry: up to 20 keyboard characters accepted
            # into an editable field while the interpreter is idle; the source
            # keyDown/idle handlers then see the same per-character edits as
            # desktop typing. A second entry cannot start while one drains.
            text = "".join(rng.choice(features["text"]) for _ in range(rng.randint(1, 20))).strip()
            if text:
                probe.send(f"text {rng.choice(editable)} {text}", ticks=1)
            max_x, max_y = pointer_bounds(state, features)
            probe.send(f"step {rng.randint(25, 120)} {(max_x + 10) // 2} "
                       f"{(max_y + 10) // 2} 0", ticks=120)
        elif action == "key":
            code, character = rng.choice(key_pairs)  # in-envelope Director key events
            x, y = random_point(rng, state, features)
            probe.send(f"key {code} {character} 1", ticks=1)
            probe.send(f"step {rng.randint(1, 30)} {x} {y} 0", ticks=30)
            if rng.random() < 0.9:
                probe.send(f"key {code} {character} 0", ticks=1)
                probe.send(f"step {rng.randint(1, 30)} {x} {y} 0", ticks=30)
        elif action == "sample":
            x, y = random_point(rng, state, features)
            probe.send(f"step {rng.randint(1, 60)} {x} {y} {rng.randint(0, 1)}", ticks=60)
        else:  # power cycle: volatile state resets, saves persist
            probe.send("reboot", ticks=1200)


def player_commands(commands: list[str], *, max_commands: int = PRELUDE_COMMANDS,
                    max_ticks: int = PRELUDE_TICKS) -> list[str]:
    """A corpus/prelude contains player input, never probe state injection."""
    if not isinstance(commands, list):
        raise ValueError("prelude commands must be a list")
    if len(commands) > max_commands:
        raise ValueError(f"input prefix exceeds {max_commands} commands")
    result = []
    total_ticks = 0
    for command in commands:
        if (not isinstance(command, str) or "\n" in command or "\r" in command or
                not command.strip()):
            raise ValueError("prelude commands must be nonempty single lines")
        parts = command.split()
        if parts == ["reboot"]:
            pass
        elif parts == ["document", "cancel"]:
            # Dismissing a currently open host file/print dialog is player
            # input. The host rejects it when no operation is pending.
            pass
        elif parts[0] == "pad" and len(parts) != 5:
            raise ValueError("single-player controller command is pad ticks x y buttons; "
                             f"player indices are unsupported: {command}")
        elif parts[0] in {"step", "pad"} and len(parts) == 5:
            try:
                ticks, x, y, buttons = map(int, parts[1:])
            except ValueError:
                raise ValueError(f"invalid player command: {command}") from None
            if not 0 <= ticks <= 100000:
                raise ValueError(f"invalid player tick budget: {command}")
            total_ticks += ticks
            if total_ticks > max_ticks:
                raise ValueError(f"input prefix exceeds {max_ticks} service ticks")
            if parts[0] == "pad" and not (-128 <= x <= 127 and -128 <= y <= 127 and
                                          0 <= buttons <= 65535):
                raise ValueError(f"invalid controller sample: {command}")
            if parts[0] == "step" and not (0 <= x <= 4096 and 0 <= y <= 4096 and
                                           0 <= buttons <= 65535):
                raise ValueError(f"invalid pointer sample: {command}")
        elif parts[0] == "key" and len(parts) == 4:
            try:
                code, char, down = map(int, parts[1:])
            except ValueError:
                raise ValueError(f"invalid keyboard command: {command}") from None
            if not (0 <= code <= 127 and 0 <= char <= 255 and down in (0, 1)):
                raise ValueError(f"invalid keyboard sample: {command}")
        elif parts[0] == "text" and len(parts) >= 3:
            try:
                sprite = int(parts[1])
            except ValueError:
                raise ValueError(f"invalid text command: {command}") from None
            if not (sprite > 0 and 1 <= len(command.split(None, 2)[2]) <= 63):
                raise ValueError(f"invalid text entry: {command}")
        else:
            raise ValueError(f"prelude cannot inject runtime state: {command}")
        result.append(command)
    return result


def read_prelude(path: Path) -> list[str]:
    if path.is_dir():
        path = path / ("replay.json" if (path / "replay.json").is_file() else "commands.txt")
    if path.suffix == ".json":
        replay = json.loads(path.read_text())
        if not isinstance(replay, dict) or "commands" not in replay:
            raise ValueError("prelude JSON needs a single session's commands")
        report = path.with_name("report.json")
        failed_report = (path.name == "replay.json" and report.is_file() and
                         json.loads(report.read_text()).get("status") != "passing")
        if replay.get("error") or failed_report:
            raise ValueError(f"journey prelude did not pass: {path}")
        return player_commands(replay["commands"])
    return player_commands([line.strip() for line in path.read_text().splitlines()
                            if line.strip() and not line.lstrip().startswith("#")])


@dataclass
class Runner:
    executable: Path
    output: Path
    features: dict
    env: dict
    movies: list[str]
    actions: int
    minimize: bool
    # Scripted openings (--prelude): one, chosen per episode, runs right after
    # boot, so the random walk starts past a game's login or name entry.
    preludes: list[list[str]] = field(default_factory=list)
    guided: bool = False
    corpus_dir: Path | None = None
    corpus_identity: dict = field(default_factory=dict)
    corpus_limit: int = 128
    prefix_limit: int = 2048
    episode_timeout: float = 600
    minimization_deadline: float | None = None
    lock: threading.Lock = field(default_factory=threading.Lock)
    signatures: dict[str, dict] = field(default_factory=dict)
    coverage: dict[str, int] = field(default_factory=dict)
    observed_coverage: Counter = field(default_factory=Counter)
    input_counts: Counter = field(default_factory=Counter)
    corpus: dict[str, dict] = field(default_factory=dict)
    corpus_features: set[str] = field(default_factory=set)
    invariant_spec: dict | None = None
    invariant_checks: Counter = field(default_factory=Counter)
    provenance: dict = field(default_factory=dict)
    corpus_replays: int = 0
    episodes: int = 0
    commands: int = 0

    def load_corpus(self) -> None:
        if not self.corpus_dir or not self.corpus_dir.exists():
            return
        for path in sorted(self.corpus_dir.glob("*.json")):
            item = json.loads(path.read_text())
            if item.get("schema_version") != 1 or item.get("identity") != self.corpus_identity:
                raise ValueError(f"corpus source identity differs: {path}")
            if item.get("movie") not in self.movies:
                raise ValueError(f"corpus entry movie differs: {path}")
            if (not isinstance(item.get("features"), list) or
                    any(not isinstance(v, str) for v in item["features"])):
                raise ValueError(f"corpus features must be strings: {path}")
            commands = player_commands(item["commands"])
            if len(commands) > self.prefix_limit:
                raise ValueError(f"corpus prefix exceeds {self.prefix_limit} commands: {path}")
            if len(self.corpus) < self.corpus_limit:
                self.corpus[path.stem] = item
                self.corpus_features.update(item["features"])

    def prefix(self, movie: str, rng: random.Random) -> list[str]:
        with self.lock:
            entries = [v for _, v in sorted(self.corpus.items()) if v["movie"] == movie]
            if self.guided and entries and rng.random() < 0.7:
                self.corpus_replays += 1
                return list(rng.choice(entries)["commands"])
        return list(rng.choice(self.preludes)) if self.preludes else []

    def retain(self, movie: str, seed: int, probe: Probe) -> None:
        if not self.guided:
            return
        with self.lock:
            novel = set(probe.observations) - self.corpus_features
            prefixes: dict[int, list[str]] = {}
            for feature in sorted(novel):
                index = probe.observations[feature]
                if 0 < index <= self.prefix_limit:
                    prefixes.setdefault(index, []).append(feature)
            for index, features in sorted(prefixes.items()):
                commands = probe.commands[:index]
                try:
                    player_commands(commands)
                except ValueError:
                    continue
                digest = hashlib.sha256(json.dumps([movie, commands]).encode()).hexdigest()[:24]
                if digest not in self.corpus and len(self.corpus) >= self.corpus_limit:
                    continue
                existing = self.corpus.get(digest, {})
                item = {"schema_version": 1, "identity": self.corpus_identity,
                        "movie": movie, "seed": seed, "commands": commands,
                        "features": sorted(set(features) | set(existing.get("features", [])))}
                self.corpus[digest] = item
                self.corpus_features.update(features)
                if self.corpus_dir:
                    self.corpus_dir.mkdir(parents=True, exist_ok=True)
                    (self.corpus_dir / f"{digest}.json").write_text(
                        json.dumps(item, indent=2) + "\n")

    def run_episode(self, seed: int) -> None:
        rng = random.Random(seed)
        movie = rng.choice(self.movies)
        stderr_path = self.output / f"episode-{seed}.stderr"
        probe = Probe(self.executable, movie, stderr_path, self.env, self.invariant_spec,
                      timeout=self.episode_timeout)
        outcome = None
        try:
            probe.receive(timeout=120.0)
            for line in self.prefix(movie, rng):
                ticks = int(line.split()[1]) if line.startswith(("step ", "pad ")) else 0
                probe.send(line, ticks=ticks)
            episode_commands(probe, rng, self.actions, self.features)
            outcome = probe.close()
        except ProbeFailure as failure:
            outcome = failure.outcome
            probe.kill()
        except BaseException:
            probe.kill()
            raise
        finally:
            with self.lock:
                self.episodes += 1
                self.commands += len(probe.commands)
                for seen_movie in set(probe.movie_trail):
                    self.coverage[seen_movie] = self.coverage.get(seen_movie, 0) + 1
                self.observed_coverage.update(probe.observations.keys())
                self.input_counts.update(probe.input_counts)
                if probe.invariants is not None:
                    self.invariant_checks.update(probe.invariants.checks)
        self.retain(movie, seed, probe)
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
            "invariant_model": self.invariant_spec,
            "provenance": self.provenance,
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
            "invariant_model": self.invariant_spec,
            "provenance": self.provenance,
        }
        (case / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"ALERT script-alert [{signature[:120]}] -> {case}", flush=True)

    def replay(self, movie: str, commands: list[str], *,
               timeout: float | None = None) -> tuple[Outcome, list[str]]:
        stderr_path = self.output / f"replay-{threading.get_ident()}-{time.monotonic_ns()}.stderr"
        probe = Probe(self.executable, movie, stderr_path, self.env, self.invariant_spec,
                      timeout=timeout if timeout is not None else self.episode_timeout)
        try:
            probe.receive(timeout=120.0)
            for command in commands:
                ticks = sum(int(f) for f in command.split()[1:2] if f.isdigit())
                probe.send(command, ticks=max(ticks, 1200))
            outcome = probe.close()
        except ProbeFailure as failure:
            outcome = failure.outcome
            probe.kill()
        except BaseException:
            probe.kill()
            raise
        finally:
            stderr_path.unlink(missing_ok=True)
        return outcome, [normalize(m) or "script alert" for m, _, _ in probe.soft_events]

    def shrink(
        self, movie: str, commands: list[str], signature: str, soft: bool = False
    ) -> list[str]:
        """Bounded ddmin-style chunk removal keeping the same failure signature."""
        deadline = time.monotonic() + min(self.episode_timeout, 120)
        if self.minimization_deadline is not None:
            deadline = min(deadline, self.minimization_deadline)

        def reproduces(candidate: list[str]) -> bool:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            outcome, soft_signatures = self.replay(movie, candidate, timeout=remaining)
            if soft:
                return signature in soft_signatures
            found = outcome.signature or normalize(outcome.error) or outcome.kind
            return outcome.kind != "ok" and found == signature

        best, budget, chunk = commands, 48, max(1, len(commands) // 2)
        while budget > 0 and time.monotonic() < deadline:
            index = 0
            while index < len(best) and budget > 0 and time.monotonic() < deadline:
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


def load_case(path: Path, entry_movie: str = "START") -> tuple[str, list[str]]:
    if path.is_dir():
        report = json.loads((path / "report.json").read_text())
        best = path / "commands.min.txt"
        commands_file = best if best.exists() else path / "commands.txt"
        return report["movie"], commands_file.read_text().splitlines()
    return entry_movie, path.read_text().splitlines()


def main(game, argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="director64 fuzz", description="randomized input fuzzing"
    )
    parser.add_argument("--duration", type=float, default=300.0, help="seconds of fuzzing")
    parser.add_argument("--episode-timeout", type=float, default=600.0,
                        help="total wall-time bound per episode/replay, including its prelude")
    parser.add_argument("--episodes", type=int, help="stop after this many episodes")
    parser.add_argument("--actions", type=int, default=250, help="input actions per episode")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--seed", type=int, help="base seed (default: random)")
    parser.add_argument(
        "--movie", action="append", help="entry movie (repeatable, default START)"
    )
    parser.add_argument("--replay", type=Path, help="re-run a recorded case and exit")
    parser.add_argument("--no-minimize", action="store_true")
    parser.add_argument("--unguided", action="store_true",
                        help="disable retaining and replaying novel state prefixes")
    parser.add_argument("--corpus", type=Path,
                        help="persistent input corpus for this edition (default output/corpus)")
    parser.add_argument("--strict-alerts", action="store_true",
                        help="fail the run on recovered script alerts as well")
    parser.add_argument("--model", type=Path,
                        help="versioned conditional game invariant model JSON")
    parser.add_argument("--require-model-coverage", action="store_true",
                        help="fail if any requested invariant rule was never checked")
    parser.add_argument("--output", type=Path, help="artifact directory")
    parser.add_argument(
        "--probe", type=Path, help="probe executable (default the web build's, director64 web)"
    )
    parser.add_argument(
        "--prelude",
        type=Path,
        action="append",
        help="input commands or a journey replay.json (repeatable: one per episode)",
    )
    args = parser.parse_args(argv)
    if (not math.isfinite(args.duration) or args.duration <= 0 or
            not math.isfinite(args.episode_timeout) or args.episode_timeout <= 0 or
            args.actions <= 0 or args.jobs <= 0 or
            (args.episodes is not None and args.episodes <= 0)):
        parser.error("duration/timeouts must be finite and all limits must be positive")
    if args.require_model_coverage and not args.model:
        parser.error("--require-model-coverage needs --model")

    from .web import probe_path

    executable = args.probe or probe_path(game)
    env = os.environ | {
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=1",
    }
    stamp = datetime.now(UTC).strftime("%Y%m%dT%H%M%SZ")
    output = args.output or game.work / "fuzz" / stamp
    output.mkdir(parents=True, exist_ok=True)
    identity = {"game": game.slug, "source_sha256": game.source["sha256"],
                **game.image_identity}
    case_report = (json.loads((args.replay / "report.json").read_text())
                   if args.replay and args.replay.is_dir() else None)
    if case_report is not None:
        recorded_identity = case_report.get("provenance", {}).get("identity")
        if recorded_identity is not None and recorded_identity != identity:
            raise ValueError("failure case belongs to a different game or source edition")
    invariant_spec = json.loads(args.model.read_text()) if args.model else None
    if invariant_spec is None and case_report is not None:
        invariant_spec = case_report.get("invariant_model")
    if invariant_spec is not None:
        InvariantModel(invariant_spec)
    site = game.dist / "web"
    build = site / "build.json"
    provenance = {"identity": identity,
                  "build": (json.loads(build.read_text())
                            if args.probe is None and build.is_file() else None),
                  "fuzzer_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  "files_sha256": {}}
    candidates = [executable]
    if args.probe is None:
        candidates.extend([site / "director64.wasm", site / "director64.mjs",
                           game.root / "platforms/web/node/probe.mjs"])
    for path in candidates:
        if path.is_file():
            with path.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
                provenance["files_sha256"][str(path)] = digest
    runner = Runner(
        executable=executable,
        output=output,
        features=game_features(game),
        env=env,
        movies=args.movie or [game.data["port"].get("entry_movie", "START")],
        actions=args.actions,
        minimize=not args.no_minimize,
        preludes=[read_prelude(path) for path in args.prelude or []],
        guided=not args.unguided,
        corpus_dir=args.corpus or output / "corpus",
        corpus_identity=identity,
        invariant_spec=invariant_spec,
        provenance=provenance,
        episode_timeout=args.episode_timeout,
    )
    runner.load_corpus()
    if args.replay:
        movie, commands = load_case(args.replay, game.data["port"].get("entry_movie", "START"))
        commands = player_commands(commands, max_commands=65536, max_ticks=10_000_000)
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
        repeated_alert = (case_report is not None and case_report.get("kind") == "script-alert"
                          and case_report.get("signature") in soft)
        return int(outcome.kind != "ok" or repeated_alert or bool(args.strict_alerts and soft))

    base_seed = args.seed if args.seed is not None else random.randrange(1 << 32)
    deadline = time.monotonic() + args.duration
    runner.minimization_deadline = deadline + 120
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
    if not runner.episodes:
        internal_errors.append("no episodes ran before the duration limit")
    model_report = None
    unreached = []
    if invariant_spec is not None:
        unreached = [rule["id"] for rule in invariant_spec["rules"]
                     if not runner.invariant_checks[rule["id"]]]
        model_report = {"id": invariant_spec["id"],
                        "sha256": hashlib.sha256(json.dumps(
                            invariant_spec, sort_keys=True).encode()).hexdigest(),
                        "checks": dict(runner.invariant_checks), "unreached_rules": unreached}
    summary = {
        "game": game.slug,
        "executable": str(executable),
        "base_seed": base_seed,
        "episodes": runner.episodes,
        "commands": runner.commands,
        "episode_timeout_seconds": runner.episode_timeout,
        "unique_failures": len(runner.signatures),
        "internal_errors": internal_errors,
        "coverage": dict(sorted(runner.coverage.items(), key=lambda kv: -kv[1])),
        "observed_coverage": dict(sorted(runner.observed_coverage.items())),
        "coverage_kind": "sampled frames, transitions, handlers, sprite sets and save names",
        "input_counts": dict(sorted(runner.input_counts.items())),
        "corpus": {"entries": len(runner.corpus), "replays": runner.corpus_replays,
                   "directory": str(runner.corpus_dir), "guided": runner.guided},
        "invariant_model": model_report,
        "provenance": runner.provenance,
        "failures": runner.signatures,
    }
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    excluded = {"failures", "internal_errors", "coverage", "observed_coverage", "provenance"}
    console = {k: v for k, v in summary.items() if k not in excluded}
    print(json.dumps(console | {"internal_errors": len(internal_errors)}))
    for signature, info in runner.signatures.items():
        print(f"{info['kind']} x{info['count']}: {signature[:140]} -> {info['case']}")
    # Alert-and-continue is an engine behavior, not evidence that the alert
    # matches the original game. Retain every alert and optionally gate it.
    hard = [s for s, i in runner.signatures.items()
            if args.strict_alerts or i["kind"] != "script-alert"]
    return int(bool(hard or internal_errors or (args.require_model_coverage and unreached)))

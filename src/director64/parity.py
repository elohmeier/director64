"""Compare two host probes' state streams under the fuzzer's command streams.

Every Track A milestone of docs/roadmap.md must reproduce the previous
milestone's probe states byte for byte before it claims a speed-up. Side A
is a probe built from the reference generator (`build/<game>/<source>/
parity/probe-a`); side B is the sanitized probe of the working tree. Each
episode drives A with the fuzzer's own random walk, records every command
and the state it produced, replays the commands on B and compares.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import re
from pathlib import Path

from .fuzz import DEFAULT_FEATURES, PROBE_FEATURES, Probe, ProbeFailure, episode_commands

# Fields that describe the runtime rather than the game: the value heap's
# footprint and the number of live handles change with the value
# representation and the collector's cadence while every authored value
# stays the same, so these metrics are not part of parity.
VOLATILE: frozenset[str] = frozenset({"heap_bytes", "heap_high_water", "object_handles"})


INSTANCE = re.compile(r"<instance (\d+)>")


class RecordingProbe(Probe):
    """A probe that keeps the state every command produced.

    Instances are renumbered by first appearance in the state stream. The
    probe names them by allocation serial, which the collector's cadence
    cannot move but which every change to what the runtime allocates
    does; two runs that behave the same show the same instances in the
    same order, and that order is what the comparison sees.
    """

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.states: list[dict] = []
        self.ticks: list[int] = []
        self.instances: dict[str, int] = {}

    def send(self, command: str, ticks: int = 0) -> dict:
        if command.split(" ", 1)[0] == "reboot":
            # A fresh runtime numbers its allocations from zero again.
            self.instances = {}
        state = super().send(command, ticks)
        self.states.append(self.renumbered(state))
        self.ticks.append(ticks)
        return state

    def renumbered(self, value):
        if isinstance(value, dict):
            return {k: self.renumbered(v) for k, v in value.items()}
        if isinstance(value, list):
            return [self.renumbered(v) for v in value]
        if isinstance(value, str) and "<instance " in value:
            return INSTANCE.sub(
                lambda m: f"<instance #{self.instances.setdefault(m[1], len(self.instances) + 1)}>",
                value,
            )
        return value


def folded(value):
    """Text folded to lower case: Lingo compares strings without case, a
    symbol's identity is case-insensitive, and the two sides may have
    interned different spellings first (the reference generator folded
    every symbol; the compiler keeps the authored spelling)."""
    if isinstance(value, str):
        return value.lower()
    if isinstance(value, list):
        return [folded(v) for v in value]
    if isinstance(value, dict):
        return {k: folded(v) for k, v in value.items()}
    return value


def normalized(state: dict) -> dict:
    result = {k: folded(v) for k, v in state.items() if k not in VOLATILE}
    if isinstance(result.get("stack"), list):
        # Older probes printed a frame's self as its handle number.
        result["stack"] = [
            {k: v for k, v in frame.items() if not (k == "self" and isinstance(v, int))}
            if isinstance(frame, dict)
            else frame
            for frame in result["stack"]
        ]
    return result


def leaves(path: str, a, b, out: list) -> None:
    """The deepest differing values, one line each, so a divergence names
    the global or property that moved rather than the whole table."""
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b)):
            if a.get(key) != b.get(key):
                leaves(f"{path}.{key}" if path else key, a.get(key), b.get(key), out)
    elif isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        for i, (x, y) in enumerate(zip(a, b, strict=True)):
            if x != y:
                leaves(f"{path}[{i}]", x, y, out)
    else:
        out.append(f"  {path}: A={json.dumps(a)[:160]} B={json.dumps(b)[:160]}")


def difference(a: dict, b: dict) -> str:
    lines: list = []
    leaves("", normalized(a), normalized(b), lines)
    return "\n".join(lines[:40])


def run_episode(
    seed: int,
    movies: list[str],
    actions: int,
    features: dict,
    side_a: Path,
    side_b: Path,
    output: Path,
    env: dict,
) -> dict:
    rng = random.Random(seed)
    movie = rng.choice(movies)
    a = RecordingProbe(side_a, movie, output / f"{seed}-a.stderr", env)
    outcome_a = None
    try:
        a.receive(timeout=120.0)
        episode_commands(a, rng, actions, features)
        outcome_a = a.close()
    except ProbeFailure as failure:
        outcome_a = failure.outcome
        a.kill()
    b = RecordingProbe(side_b, movie, output / f"{seed}-b.stderr", env)
    outcome_b = None
    divergence = None
    try:
        b.receive(timeout=120.0)
        for index, (command, ticks) in enumerate(zip(a.commands, a.ticks, strict=True)):
            b.send(command, ticks)
            state = b.states[index]
            if normalized(state) != normalized(a.states[index]):
                divergence = {
                    "index": index,
                    "command": command,
                    "movie": a.states[index].get("movie"),
                    "diff": difference(a.states[index], state),
                }
                break
        outcome_b = b.close()
    except ProbeFailure as failure:
        outcome_b = failure.outcome
        b.kill()
    if divergence is None and len(b.states) != len(a.states) and outcome_a.kind == "ok":
        divergence = {
            "index": len(b.states),
            "command": a.commands[len(b.states)] if len(b.states) < len(a.commands) else "",
            "movie": a.states[-1].get("movie") if a.states else movie,
            "diff": f"  B produced {len(b.states)} states for {len(a.states)} commands",
        }
    same_outcome = (outcome_a.kind, outcome_a.error) == (outcome_b.kind, outcome_b.error)
    if divergence is None and not same_outcome:
        divergence = {
            "index": len(a.commands),
            "command": "",
            "movie": a.states[-1].get("movie") if a.states else movie,
            "diff": f"  outcome: A={outcome_a.kind} {outcome_a.error!r} "
            f"B={outcome_b.kind} {outcome_b.error!r}",
        }
    for path in (output / f"{seed}-a.stderr", output / f"{seed}-b.stderr"):
        if divergence is None:
            path.unlink(missing_ok=True)
    return {
        "seed": seed,
        "movie": movie,
        "commands": len(a.commands),
        "states": len(a.states),
        "movies_visited": a.movie_trail,
        "outcome": {"kind": outcome_a.kind, "error": outcome_a.error},
        "divergence": divergence,
    }


def main(game, argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="director64 parity", description="state-stream parity of two host probes"
    )
    parser.add_argument("--against", type=Path, help="side A probe (default parity/probe-a)")
    parser.add_argument("--probe", type=Path, help="side B probe (default the sanitized probe)")
    parser.add_argument("--episodes", type=int, default=4)
    parser.add_argument("--actions", type=int, default=120, help="input actions per episode")
    parser.add_argument("--seed", type=int, default=4130993319, help="base seed")
    parser.add_argument("--movie", action="append", help="entry movie (repeatable)")
    parser.add_argument("--output", type=Path, help="artifact directory")
    args = parser.parse_args(argv)
    side_a = args.against or game.work / "parity/probe-a"
    side_b = args.probe or game.work / "native/director-probe-sanitized"
    for side in (side_a, side_b):
        if not side.is_file():
            raise ValueError(f"missing probe {side}")
    output = args.output or game.work / "parity"
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ | {
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=1",
    }
    movies = args.movie or [game.data["port"].get("entry_movie", "START")]
    features = PROBE_FEATURES.get(game.slug, DEFAULT_FEATURES)
    episodes = [
        run_episode(seed, movies, args.actions, features, side_a, side_b, output, env)
        for seed in range(args.seed, args.seed + args.episodes)
    ]
    diverged = [e for e in episodes if e["divergence"]]
    summary = {
        "game": game.slug,
        "side_a": str(side_a),
        "side_b": str(side_b),
        "base_seed": args.seed,
        "episodes": len(episodes),
        "commands": sum(e["commands"] for e in episodes),
        "states_compared": sum(e["states"] for e in episodes),
        "movies_visited": sorted({m for e in episodes for m in e["movies_visited"]}),
        "diverged": len(diverged),
        "results": episodes,
    }
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    console = {k: v for k, v in summary.items() if k != "results"}
    print(json.dumps(console))
    for episode in diverged:
        d = episode["divergence"]
        print(
            f"seed {episode['seed']} diverged at command {d['index']} in {d['movie']}: "
            f"{d['command']}\n{d['diff']}"
        )
    return 1 if diverged else 0

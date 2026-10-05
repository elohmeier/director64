"""Record which images each scene draws: the port's working sets.

The score names the members its frames show, but the ports in the corpus
are Lingo-driven: a scene's sprites take their members from scripts, and
the score of a twelve-frame movie says almost nothing about the hundred
images it cycles. So the host probe records what the stage draws while the
port's journey and a run of fuzz episodes walk it (``DIRECTOR64_DRAW_RECORD``,
platforms/native/draw_record.inc), and this command folds those records
together with the score's references into ``host/working-sets.json``: per
movie, every (asset, ink) pair seen, with how many ticks drew it and how
many score frames reference it. The packer turns the file into one image
bundle per scene, in that priority order, and a pair the walk never saw
still loads through the pack's global index.
"""

from __future__ import annotations

import argparse
import inspect
import json
import os
import random
import time
from datetime import UTC, datetime
from pathlib import Path

from .fuzz import DEFAULT_FEATURES, PROBE_FEATURES, Probe, ProbeFailure, episode_commands

# Score channels below six are the frame's tempo, palette, transition and
# sound settings; the sprite channels start at six.
FIRST_SPRITE_CHANNEL = 6


def score_references(model: dict) -> dict[str, dict[tuple[str, int], int]]:
    """Per movie, the (asset, ink) pairs its score frames reference and the
    number of frames referencing each; film loops contribute every pose."""
    by_name = {movie["name"]: movie for movie in model["movies"]}
    members = {
        movie["name"]: {(m["cast"], m["number"]): m for m in movie["members"]}
        for movie in model["movies"]
    }
    result: dict[str, dict[tuple[str, int], int]] = {}
    for movie in model["movies"]:
        counts = result.setdefault(movie["name"], {})
        for frame in movie.get("score", {}).get("frames", []):
            for channel in frame["channels"]:
                c = channel["channel"]
                if c < FIRST_SPRITE_CHANNEL:
                    continue
                b = bytes(channel["bytes"])
                cast, number = int.from_bytes(b[4:6], "big"), int.from_bytes(b[6:8], "big")
                if not number or not 1 <= cast <= len(movie["casts"]):
                    continue
                lib = movie["casts"][cast - 1]
                target = by_name.get(lib["file"])
                if target is None:
                    continue
                target_cast = cast if target is movie else 1
                member = members[target["name"]].get((target_cast, number))
                if member is None:
                    continue
                ink = b[1] & 63
                bitmap = member.get("type") == 1 and member.get("asset")
                assets = [member["asset"]] if bitmap else []
                if member.get("type") == 2:
                    assets = [a for a in member.get("filmAssets", []) if a]
                for asset in assets:
                    counts[(asset, ink)] = counts.get((asset, ink), 0) + 1
    return result


def read_records(path: Path) -> dict[str, dict[tuple[str, int], int]]:
    """Fold the probe's appended record lines into per-movie tick counts."""
    result: dict[str, dict[tuple[str, int], int]] = {}
    if not path.exists():
        return result
    for line in path.read_text().splitlines():
        parts = line.split("\t")
        if len(parts) != 4:
            continue
        movie, asset, ink, ticks = parts
        counts = result.setdefault(movie, {})
        counts[(asset, int(ink))] = counts.get((asset, int(ink)), 0) + int(ticks)
    return result


def merged(
    drawn: dict[str, dict[tuple[str, int], int]],
    scored: dict[str, dict[tuple[str, int], int]],
) -> dict[str, list[dict]]:
    movies = {}
    for name in sorted(set(drawn) | set(scored)):
        rows = {}
        for (asset, ink), ticks in drawn.get(name, {}).items():
            rows.setdefault((asset, ink), {"asset": asset, "ink": ink, "ticks": 0, "frames": 0})
            rows[(asset, ink)]["ticks"] += ticks
        for (asset, ink), frames in scored.get(name, {}).items():
            rows.setdefault((asset, ink), {"asset": asset, "ink": ink, "ticks": 0, "frames": 0})
            rows[(asset, ink)]["frames"] += frames
        # Priority is what the stage drew most; the score's own references
        # follow, so a bundle that cannot hold everything keeps the images
        # every tick asks for.
        movies[name] = sorted(
            rows.values(), key=lambda r: (-r["ticks"], -r["frames"], r["asset"], r["ink"])
        )
    return movies


def walk(game, executable: Path, record: Path, args, output: Path) -> dict:
    """Drive the journey and the fuzz episodes with the recorder enabled."""
    env = os.environ | {
        "DIRECTOR64_WORK_DIR": str(game.work),
        "DIRECTOR64_DRAW_RECORD": str(record),
    }
    report = {"journey": None, "episodes": 0, "failed_episodes": []}
    if not args.no_journey:
        try:
            journey = game.host("full_journey")
        except Exception:  # the port has no scripted journey
            journey = None
        if journey is not None and hasattr(journey, "run"):
            # The journey's own session driver inherits the environment.
            previous = os.environ.get("DIRECTOR64_DRAW_RECORD")
            os.environ["DIRECTOR64_DRAW_RECORD"] = str(record)
            try:
                # A port whose journey has an activity mode (Mucklas) plays
                # the activities too: they are the scenes a session lives in.
                if "activity" in inspect.signature(journey.run).parameters:
                    journey.run(executable, output / "journey", activity=True)
                else:
                    journey.run(executable, output / "journey")
                report["journey"] = "passed"
            except Exception as failure:  # noqa: BLE001 - a failing journey still recorded its draws
                report["journey"] = f"failed: {failure}"
            finally:
                if previous is None:
                    del os.environ["DIRECTOR64_DRAW_RECORD"]
                else:
                    os.environ["DIRECTOR64_DRAW_RECORD"] = previous
    features = PROBE_FEATURES.get(game.slug, DEFAULT_FEATURES)
    movies = [game.data["port"].get("entry_movie", "START")]
    for seed in range(args.seed, args.seed + args.episodes):
        rng = random.Random(seed)
        probe = Probe(executable, rng.choice(movies), output / f"{seed}.stderr", env)
        try:
            probe.receive(timeout=120.0)
            episode_commands(probe, rng, args.actions, features)
            probe.close()
        except ProbeFailure as failure:
            report["failed_episodes"].append({"seed": seed, "outcome": failure.outcome.kind})
            probe.kill()
        report["episodes"] += 1
    return report


def main(game, argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="director64 working-sets",
        description="record the images each scene draws, from the host walks and the score",
    )
    parser.add_argument("--episodes", type=int, default=48, help="fuzz episodes to walk")
    parser.add_argument("--actions", type=int, default=300, help="input actions per episode")
    parser.add_argument("--seed", type=int, default=4130993319, help="base seed")
    parser.add_argument("--no-journey", action="store_true", help="skip the scripted journey")
    parser.add_argument("--output", type=Path, help="destination (default host/working-sets.json)")
    args = parser.parse_args(argv)
    executable = game.work / "native/director-probe"
    if not executable.is_file():
        raise ValueError(f"missing probe {executable}")
    scratch = game.work / "native/working-sets"
    scratch.mkdir(parents=True, exist_ok=True)
    record = scratch / "draw-record.tsv"
    if record.exists():
        record.unlink()
    started = time.monotonic()
    report = walk(game, executable, record, args, scratch)
    model = json.loads((game.work / "director/model.json").read_text())
    movies = merged(read_records(record), score_references(model))
    document = {
        "version": 1,
        "recorded": {
            "date": datetime.now(UTC).strftime("%Y-%m-%d"),
            "journey": report["journey"],
            "episodes": report["episodes"],
            "actions": args.actions,
            "seed": args.seed,
            "failed_episodes": report["failed_episodes"],
        },
        "movies": movies,
    }
    output = args.output or game.directory / "host/working-sets.json"
    output.write_text(json.dumps(document, indent=1) + "\n")
    drawn = sum(1 for rows in movies.values() for r in rows if r["ticks"])
    total = sum(len(rows) for rows in movies.values())
    print(
        f"{output}: {len(movies)} movies, {total} (asset, ink) pairs, {drawn} drawn by the "
        f"walk, journey {report['journey']}, {report['episodes']} episodes "
        f"({len(report['failed_episodes'])} failed) in {time.monotonic() - started:.0f}s"
    )
    return 0

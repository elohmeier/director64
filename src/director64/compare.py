"""Compare the port's rendering with the original projector under Wine.

One timed input replay drives both: the original capture (`director64
reference`, or an existing capture directory) and the port's web probe. Their
clocks start at different moments and the original runs in real time, so
frames never correspond exactly. The comparison therefore

1. samples both at FPS and finds the global time offset that matches best,
2. compares each port frame with every original frame within +/- WINDOW
   seconds and keeps, per pixel, the closest match: animation phase, rain and
   timing jitter drop out, while content the original never shows does not,
3. reports regions where that difference persists in most frames of a movie,
   with the port sprites drawn there (member, ink, blend, bounds) and images:
   port | closest original | persistent difference.

This is original-projector evidence under Wine; text uses the Node probe's
nominal glyphs, so text regions are expected to differ.

    uv run --locked director64 compare --game findus-ghosts \\
      --original build/findus-ghosts/<source>/reference/<capture> --start 60
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
from collections import deque
from datetime import UTC, datetime
from pathlib import Path

import numpy as np

from .fuzz import Probe
from .visual import write_png
from .web import probe_path

BLOCK = 4  # Persistence is judged per 4x4 block, ignoring isolated pixels.
MIN_FRAMES = 10  # Fewer sampled frames of a movie give no verdict.


def ticks_at(ms: float) -> int:
    return round(ms * 60 / 1000)


def run_original(game, replay: Path) -> Path:
    """A fresh original capture of REPLAY; its directory."""
    completed = subprocess.run(
        ["uv", "run", "--locked", "director64", "reference", "--game", game.slug,
         "--source", game.source["id"], "--replay", str(replay)],
        capture_output=True, text=True, check=False, cwd=game.root)
    lines = completed.stdout.strip().splitlines()
    if completed.returncode or not lines or not lines[-1].endswith("capture.json"):
        raise ValueError("original capture failed:\n" + completed.stdout + completed.stderr)
    return Path(lines[-1]).parent


def original_frames(capture: Path, fps: float, width: int, height: int,
                    scratch: Path) -> np.ndarray:
    """The original recording at FPS as (frames, height, width, 3) RGB."""
    raw = scratch / "original.rgb"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(capture / "original.mkv"),
                    "-vf", f"fps={fps},scale={width}:{height}:flags=neighbor",
                    "-f", "rawvideo", "-pix_fmt", "rgb24", str(raw)], check=True)
    frames = np.memmap(raw, dtype=np.uint8, mode="r")
    return frames.reshape(-1, height, width, 3)


STEP = re.compile(r"step (\d+) (-?\d+) (-?\d+) ([01])\Z")
# Macintosh virtual key codes of typed letters (platforms/web/keyboard.inc).
LETTERS = (0, 11, 8, 2, 14, 3, 5, 4, 34, 38, 40, 37, 46, 45, 31, 35, 12, 15, 1, 17, 32, 9, 13,
           7, 16, 6)


def replay_commands(replay: dict) -> list[str]:
    """A timed original-projector replay as probe commands."""
    commands, x, y, down, tick = [], 0, 0, 0, 0
    for action in replay["actions"]:
        due = ticks_at(action["at_ms"])
        if due > tick:
            commands.append(f"step {due - tick} {x} {y} {down}")
            tick = due
        if action["type"] == "pointer":
            x, y = action["x"], action["y"]
        elif action["type"] in ("button_down", "button_up") and action["button"] == "left":
            down = int(action["type"] == "button_down")
        elif action["type"] == "key":
            key = action["key"]
            if len(key) != 1 or not key.isascii() or not key.isalpha():
                raise ValueError(f"the comparison cannot replay key {key!r} yet")
            code = LETTERS[ord(key.upper()) - ord("A")]
            commands += [f"key {code} {ord(key)} 1", f"key {code} {ord(key)} 0"]
    end = ticks_at(replay["duration_ms"])
    if end > tick:
        commands.append(f"step {end - tick} {x} {y} {down}")
    return commands


def journey_replay(name: str, record: dict) -> tuple[dict, list[str]]:
    """A port journey's recorded probe commands and the same input as a timed
    original-projector replay. Text entry, which the TV's dialog delivers to
    the field at once, becomes letters typed just before that moment."""
    actions, x, y, down, tick = [], None, None, 0, 0
    for command in record["commands"]:
        step = STEP.match(command)
        if step:
            ticks, nx, ny, ndown = (int(v) for v in step.groups())
            at = round(tick * 1000 / 60)
            if (nx, ny) != (x, y):
                actions.append({"at_ms": at, "type": "pointer", "x": nx, "y": ny})
                x, y = nx, ny
            if ndown != down:
                kind = "button_down" if ndown else "button_up"
                actions.append({"at_ms": at, "type": kind, "button": "left"})
                down = ndown
            tick += ticks
        elif command.startswith("text "):
            text = command.split(" ", 2)[2]
            if not text.isascii() or not text.isalnum():
                raise ValueError(f"cannot type {text!r} into the original")
            at = round(tick * 1000 / 60)
            for i, character in enumerate(text):
                actions.append({"at_ms": max(0, at - 150 * (len(text) - i)), "type": "key",
                                "key": character})
        else:
            raise ValueError(f"journey command has no original equivalent: {command}")
    states = record["states"]
    for checkpoint in record["checkpoints"]:
        at = round(states[checkpoint["state"]]["tick"] * 1000 / 60)
        actions.append({"at_ms": at, "type": "checkpoint", "id": checkpoint["label"]})
    actions.sort(key=lambda a: a["at_ms"])
    duration = round(tick * 1000 / 60) + 1000
    if duration > 600_000:
        raise ValueError("the journey exceeds the original runner's ten-minute limit")
    replay = {"schema_version": 1, "id": re.sub(r"[^A-Za-z0-9_-]", "-", name)[:48],
              "clock": "host_monotonic_ms", "duration_ms": duration, "actions": actions}
    return replay, [*record["commands"], f"step 60 {x or 0} {y or 0} 0"]


def port_frames(game, commands: list[str], duration_ms: int, fps: float, scratch: Path,
                timeout: float) -> tuple[np.ndarray, list[dict]]:
    """Run COMMANDS in the web probe, capturing the stage every 1/FPS s. Steps
    are split at sample ticks; a split step is the same input."""
    frames_dir = scratch / "port"
    frames_dir.mkdir()
    env = os.environ | {"DIRECTOR64_PROBE_CAPTURE": str(frames_dir)}
    probe = Probe(probe_path(game), game.data["port"]["entry_movie"],
                  scratch / "probe.stderr", env, timeout=timeout)
    state = probe.receive(timeout=300)  # the boot state precedes any command
    pending = deque(commands)
    tick, samples = 0, []
    count = int(duration_ms * fps / 1000)
    try:
        for index in range(count):
            target = ticks_at(index * 1000 / fps)
            while pending:
                step = STEP.match(pending[0])
                if not step:
                    probe.send(pending.popleft())
                    continue
                ticks, x, y, down = (int(v) for v in step.groups())
                if tick + ticks <= target:
                    pending.popleft()
                elif tick < target:
                    pending[0] = f"step {tick + ticks - target} {x} {y} {down}"
                    ticks = target - tick
                else:
                    break
                state = probe.send(f"step {ticks} {x} {y} {down}", ticks)
                tick += ticks
            if tick < target:
                break  # the commands end before the recording does
            state = probe.send(f"capture {index:05}.rgba")
            sprites = probe.send("sprites").get("sprites", [])
            samples.append({"index": index, "tick": tick, "movie": state.get("movie"),
                            "frame": state.get("frame"), "sprites": sprites})
    finally:
        probe.close()
    stage = state["stage"]
    frames = np.stack([
        np.fromfile(frames_dir / f"{s['index']:05}.rgba", dtype=np.uint8)
        .reshape(stage[1], stage[0], 4)[:, :, :3] for s in samples])
    return frames, samples


def small(frames: np.ndarray) -> np.ndarray:
    """8x8-block grey means for the offset search."""
    n, h, w, _ = frames.shape
    grey = frames[:, : h // 8 * 8, : w // 8 * 8].mean(axis=3, dtype=np.float32)
    return grey.reshape(n, h // 8, 8, w // 8, 8).mean(axis=(2, 4))


def best_offset(port: np.ndarray, original: np.ndarray, indices: range | list[int],
                search: range) -> int:
    """Frames to add to a port index to reach its original counterpart: the
    offset whose single frames match best (a window would tie neighbours)."""
    p, o = small(port), small(original)
    indices = list(indices)
    best, score = 0, float("inf")
    for offset in search:
        errors = []
        for i in indices[::2]:
            if not 0 <= i + offset < len(o):
                continue
            errors.append(np.abs(o[i + offset] - p[i]).mean())
        if len(errors) >= max(2, len(indices) // 8) and np.median(errors) < score:
            best, score = offset, float(np.median(errors))
    if score == float("inf"):
        raise ValueError("the recordings do not overlap; lengthen the replay or --search")
    return best


def closest(port: np.ndarray, candidates: np.ndarray) -> tuple[np.ndarray, int]:
    """Per pixel, the smallest max-channel difference to any candidate, and
    the candidate that matches the whole frame best."""
    diff = np.abs(candidates.astype(np.int16) - port.astype(np.int16)).max(axis=3)
    return diff.min(axis=0), int(diff.mean(axis=(1, 2)).argmin())


def regions(mask: np.ndarray, minimum: int) -> list[tuple[int, int, int, int, int]]:
    """Connected block components: (left, top, right, bottom, blocks) in pixels."""
    seen = np.zeros_like(mask)
    found = []
    for by, bx in zip(*np.nonzero(mask), strict=True):
        if seen[by, bx]:
            continue
        queue, blocks = deque([(by, bx)]), 0
        seen[by, bx] = True
        left, top, right, bottom = bx, by, bx, by
        while queue:
            cy, cx = queue.popleft()
            blocks += 1
            left, top = min(left, cx), min(top, cy)
            right, bottom = max(right, cx), max(bottom, cy)
            for ny, nx in ((cy + 1, cx), (cy - 1, cx), (cy, cx + 1), (cy, cx - 1)):
                if (0 <= ny < mask.shape[0] and 0 <= nx < mask.shape[1]
                        and mask[ny, nx] and not seen[ny, nx]):
                    seen[ny, nx] = True
                    queue.append((ny, nx))
        if blocks >= minimum:
            found.append((int(left) * BLOCK, int(top) * BLOCK, (int(right) + 1) * BLOCK,
                          (int(bottom) + 1) * BLOCK, blocks))
    return sorted(found, key=lambda region: -region[4])


def overlaps(rect: list[int], region: tuple) -> bool:
    return rect[0] < region[2] and rect[2] > region[0] and rect[1] < region[3] and \
        rect[3] > region[1]


def rgba(image: np.ndarray) -> bytes:
    alpha = np.full(image.shape[:2] + (1,), 255, np.uint8)
    return np.concatenate([image, alpha], axis=2).tobytes()


def panel(port: np.ndarray, original: np.ndarray, persistent: np.ndarray,
          box: tuple | None) -> np.ndarray:
    """port | closest original | persistent difference (red), box outlined."""
    heat = (original // 3).astype(np.uint8)
    heat[persistent] = (255, 40, 40)
    images = [port.copy(), original.copy(), heat]
    if box:
        x0, y0, x1, y1 = (int(v) for v in box[:4])
        for image in images:
            for edge in (image[y0:y1, x0:min(x0 + 2, x1)], image[y0:y1, max(x1 - 2, x0):x1],
                         image[y0:min(y0 + 2, y1), x0:x1], image[max(y1 - 2, y0):y1, x0:x1]):
                edge[...] = (255, 255, 0)
        images = [image[max(0, y0 - 40):y1 + 40, max(0, x0 - 40):x1 + 40] for image in images]
    gap = np.zeros((images[0].shape[0], 6, 3), np.uint8)
    return np.concatenate([images[0], gap, images[1], gap, images[2]], axis=1)


def compare(game, capture: Path, output: Path, *, fps: float, window: float,
            tolerance: int, persist: float, start: float, timeout: float,
            commands: list[str] | None = None) -> dict:
    replay = json.loads((capture / "replay.json").read_text())
    scratch = output / "frames"
    scratch.mkdir(parents=True)
    port, samples = port_frames(game, commands or replay_commands(replay),
                                replay["duration_ms"], fps, scratch, timeout)
    height, width = port.shape[1:3]
    original = original_frames(capture, fps, width, height, scratch)
    reach = round(window * fps)
    first = round(start * fps)
    # The whole recording pins a first estimate (looping scenes alone are
    # ambiguous). Input-driven games then resynchronize at every click, so each
    # movie gets its own offset near that estimate.
    offset = best_offset(port, original, range(len(port)),
                         range(-round(5 * fps), round(30 * fps)))
    segments: dict[str, list[int]] = {}
    for sample in samples:
        segments.setdefault(sample["movie"], []).append(sample["index"])
    offsets = {name: best_offset(port, original, indices,
                                 range(offset - round(15 * fps), offset + round(15 * fps) + 1))
               if len(indices) >= MIN_FRAMES else offset
               for name, indices in segments.items()}
    movies: dict[str, dict] = {}
    for sample in samples[first:]:
        i = sample["index"]
        offset_i = offsets[sample["movie"]]
        lo, hi = i + offset_i - reach, i + offset_i + reach + 1
        if lo < 0 or hi > len(original):
            continue
        difference, best = closest(port[i], original[lo:hi])
        bad = difference > tolerance
        blocks = bad[: height // BLOCK * BLOCK, : width // BLOCK * BLOCK].reshape(
            height // BLOCK, BLOCK, width // BLOCK, BLOCK).mean(axis=(1, 3)) > 0.5
        movie = movies.setdefault(sample["movie"], {
            "frames": 0, "counts": np.zeros(blocks.shape, np.int32), "samples": []})
        movie["frames"] += 1
        movie["counts"] += blocks
        movie["samples"].append((i, lo + best, int(blocks.sum())))
    report = {"schema_version": 1, "game": game.slug, "source": game.source["id"],
              "original": str(capture), "environment": "original-projector-under-Wine",
              "fps": fps, "window_s": window, "tolerance": tolerance, "persist": persist,
              "offset_s": offset / fps, "movies": []}
    for name, movie in movies.items():
        if movie["frames"] < MIN_FRAMES:
            # A movie seen for a moment (a launcher, the original still
            # starting) cannot show persistence.
            report["movies"].append({"movie": name, "frames": movie["frames"],
                                     "status": "too-few-frames", "regions": []})
            continue
        persistent_blocks = movie["counts"] >= max(1, persist * movie["frames"])
        found = regions(persistent_blocks, minimum=4)
        # The sample showing the most difference represents the movie.
        index, match, _ = max(movie["samples"], key=lambda s: s[2])
        sample = samples[index]
        persistent = np.kron(persistent_blocks, np.ones((BLOCK, BLOCK), bool))
        persistent = np.pad(persistent, ((0, height - persistent.shape[0]),
                                         (0, width - persistent.shape[1])))
        stem = name.replace("/", "_")
        overview = panel(port[index], original[match], persistent, None)
        write_png(output / f"{stem}.png", overview.shape[1], overview.shape[0], rgba(overview))
        entry = {"movie": name, "frames": movie["frames"], "status": "compared",
                 "offset_s": offsets[name] / fps,
                 "persistent_fraction": round(float(persistent_blocks.mean()), 4),
                 "representative": {"port_tick": sample["tick"], "port_frame": sample["frame"],
                                    "original_s": match / fps},
                 "image": f"{stem}.png", "regions": []}
        for number, region in enumerate(found[:20], 1):
            crop = panel(port[index], original[match], persistent, region)
            image = f"{stem}-region{number:02}.png"
            write_png(output / image, crop.shape[1], crop.shape[0], rgba(crop))
            entry["regions"].append({
                "rect": list(region[:4]), "pixels": region[4] * BLOCK * BLOCK, "image": image,
                "sprites": [s for s in reversed(sample["sprites"]) if overlaps(s["rect"], region)],
            })
        report["movies"].append(entry)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main(game, argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="director64 compare", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--replay", type=Path, help="capture the original with this replay first")
    source.add_argument("--journey", help="run this registered journey on the port, then "
                        "capture the original with the same input")
    parser.add_argument("--original", type=Path,
                        help="reuse this reference capture (made with the same replay)")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--fps", type=float, default=5)
    parser.add_argument("--window", type=float, default=1.5,
                        help="seconds of original around each port frame")
    parser.add_argument("--tolerance", type=int, default=48, help="max channel difference")
    parser.add_argument("--persist", type=float, default=0.8,
                        help="fraction of a movie's frames a difference must persist in")
    parser.add_argument("--start", type=float, default=0, help="seconds of replay to skip")
    parser.add_argument("--timeout", type=float, default=1800)
    args = parser.parse_args(argv)
    if not (args.replay or args.journey or args.original):
        parser.error("choose --replay, --journey or --original")
    output = (args.output or game.work / "compare" /
              datetime.now(UTC).strftime("%Y%m%dT%H%M%S")).resolve()
    if not output.is_relative_to(game.root / "build"):
        raise ValueError("comparisons contain game imagery; keep them under build/")
    output.mkdir(parents=True, exist_ok=False)
    options = {"fps": args.fps, "window": args.window, "tolerance": args.tolerance,
               "persist": args.persist, "start": args.start, "timeout": args.timeout}
    if args.journey == "all":
        if args.original:
            parser.error("--journey all captures the original once per journey")
        from .journeys import plans

        failed = 0
        for plan in plans(game):
            print(f"== {plan['name']}")
            try:
                run_one(game, output / plan["name"], plan["name"], None, None, options)
            except (OSError, ValueError, subprocess.CalledProcessError) as error:
                failed += 1
                print(f"  failed: {error}")
        return int(failed > 0)
    run_one(game, output, args.journey, args.replay, args.original, options)
    return 0


def run_one(game, output: Path, journey: str | None, replay_path: Path | None,
            original: Path | None, options: dict) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    commands = None
    if journey:
        from .journeys import plans, run_script

        plan = next((p for p in plans(game) if p["name"] == journey), None)
        if plan is None:
            raise ValueError(f"{game.slug} has no journey {journey}")
        run_script(game, plan, output / "journey")
        record = json.loads((output / "journey/replay.json").read_text())
        replay, commands = journey_replay(f"{game.slug}-{journey}", record)
        replay_path = output / "original-replay.json"
        replay_path.write_text(json.dumps(replay, indent=2) + "\n")
    if original:
        capture = original.resolve()
        if replay_path and (json.loads(replay_path.read_text()) !=
                            json.loads((capture / "replay.json").read_text())):
            raise ValueError("the reused capture was made with a different replay")
    else:
        capture = run_original(game, replay_path.resolve())
    if not (capture / "original.mkv").is_file():
        raise ValueError(f"no original recording in {capture}")
    report = compare(game, capture, output, commands=commands, **options)
    print(f"recording offset {report['offset_s']:+.1f} s; report {output / 'report.json'}")
    for movie in report["movies"]:
        if movie["status"] != "compared":
            print(f"{movie['movie']}: {movie['frames']} frames, {movie['status']}")
            continue
        print(f"{movie['movie']}: {movie['frames']} frames at {movie['offset_s']:+.1f} s, "
              f"{movie['persistent_fraction']:.1%} persistently different, "
              f"{len(movie['regions'])} regions")
        for region in movie["regions"][:8]:
            top = region["sprites"][0] if region["sprites"] else None
            who = (f"channel {top['channel']} {top['name'] or top['member']} ink {top['ink']}"
                   if top else "no sprite")
            print(f"  {region['rect']} {region['pixels']} px  top: {who}")
    return report

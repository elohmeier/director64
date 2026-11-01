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
import subprocess
from collections import deque
from datetime import UTC, datetime
from pathlib import Path

import numpy as np

from .fuzz import Probe
from .visual import write_png
from .web import probe_path

BLOCK = 4  # Persistence is judged per 4x4 block, ignoring isolated pixels.


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


def port_frames(game, replay: dict, fps: float, scratch: Path,
                timeout: float) -> tuple[np.ndarray, list[dict]]:
    """Render REPLAY in the web probe, capturing the stage every 1/FPS s."""
    frames_dir = scratch / "port"
    frames_dir.mkdir()
    env = os.environ | {"DIRECTOR64_PROBE_CAPTURE": str(frames_dir)}
    probe = Probe(probe_path(game), game.data["port"]["entry_movie"],
                  scratch / "probe.stderr", env, timeout=timeout)
    state = probe.receive(timeout=300)  # the boot state precedes any command
    events = deque(a for a in replay["actions"] if a["type"] != "checkpoint")
    if any(a["type"] == "key" for a in events):
        raise ValueError("key actions are not supported by the comparison yet")
    x, y, down, tick = 0, 0, 0, 0
    samples = []
    count = int(replay["duration_ms"] * fps / 1000)
    try:
        for index in range(count):
            target = ticks_at(index * 1000 / fps)
            while True:
                due = ticks_at(events[0]["at_ms"]) if events else None
                stop = target if due is None or due > target else due
                if stop > tick:
                    probe.send(f"step {stop - tick} {x} {y} {down}", stop - tick)
                    tick = stop
                if due is None or due > target:
                    break
                event = events.popleft()
                if event["type"] == "pointer":
                    x, y = event["x"], event["y"]
                elif event["type"] in ("button_down", "button_up") and event["button"] == "left":
                    down = int(event["type"] == "button_down")
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


def best_offset(port: np.ndarray, original: np.ndarray, first: int, search: range) -> int:
    """Frames to add to a port index to reach its original counterpart: the
    offset whose single frames match best (a window would tie neighbours)."""
    p, o = small(port), small(original)
    best, score = 0, float("inf")
    for offset in search:
        errors = []
        for i in range(first, len(p), 2):
            if not 0 <= i + offset < len(o):
                continue
            errors.append(np.abs(o[i + offset] - p[i]).mean())
        if len(errors) >= max(5, (len(p) - first) // 8) and np.median(errors) < score:
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
            tolerance: int, persist: float, start: float, timeout: float) -> dict:
    replay = json.loads((capture / "replay.json").read_text())
    scratch = output / "frames"
    scratch.mkdir(parents=True)
    port, samples = port_frames(game, replay, fps, scratch, timeout)
    height, width = port.shape[1:3]
    original = original_frames(capture, fps, width, height, scratch)
    reach = round(window * fps)
    first = round(start * fps)
    # The whole recording pins the offset: looping scenes alone are ambiguous.
    offset = best_offset(port, original, 0, range(-round(5 * fps), round(30 * fps)))
    movies: dict[str, dict] = {}
    for sample in samples[first:]:
        i = sample["index"]
        lo, hi = i + offset - reach, i + offset + reach + 1
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
        entry = {"movie": name, "frames": movie["frames"],
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
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--original", type=Path, help="an existing reference capture directory")
    source.add_argument("--replay", type=Path, help="capture the original with this replay first")
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
    capture = (args.original or run_original(game, args.replay)).resolve()
    if not (capture / "original.mkv").is_file():
        raise ValueError(f"no original recording in {capture}")
    output = (args.output or game.work / "compare" /
              datetime.now(UTC).strftime("%Y%m%dT%H%M%S")).resolve()
    if not output.is_relative_to(game.root / "build"):
        raise ValueError("comparisons contain game imagery; keep them under build/")
    output.mkdir(parents=True, exist_ok=False)
    report = compare(game, capture, output, fps=args.fps, window=args.window,
                     tolerance=args.tolerance, persist=args.persist, start=args.start,
                     timeout=args.timeout)
    print(f"offset {report['offset_s']:+.1f} s; report {output / 'report.json'}")
    for movie in report["movies"]:
        print(f"{movie['movie']}: {movie['frames']} frames, "
              f"{movie['persistent_fraction']:.1%} persistently different, "
              f"{len(movie['regions'])} regions")
        for region in movie["regions"][:8]:
            top = region["sprites"][0] if region["sprites"] else None
            who = (f"channel {top['channel']} {top['name'] or top['member']} ink {top['ink']}"
                   if top else "no sprite")
            print(f"  {region['rect']} {region['pixels']} px  top: {who}")
    return 0

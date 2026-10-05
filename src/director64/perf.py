"""Walk reachable game states on the native probe and report per-movie cost.

The sweep speaks the same probe RPC protocol as the fuzzer, but walks
deterministically: the game's scripted journey first (login, saves, the deep
states behind them), then a systematic crawl that clicks every live sprite
region it has not tried yet in each movie and dwells after every transition
so idle cost is sampled. The probe accounts CPU time per movie tick and
reports it through the ``perf`` command; the report flags movies whose mean
service cost exceeds the host budget, a proxy for the console's 60 Hz tick.
"""

from __future__ import annotations

import argparse
import json
import os
import time
from datetime import UTC, datetime
from pathlib import Path

from .fuzz import DEFAULT_FEATURES, PROBE_FEATURES, Probe, ProbeFailure

# Sprites parked far offstage are storage, not hotspots; the authored stages
# in the corpus are at most 1024 wide.
STAGE_LIMIT = 1024


def registry_movies(game) -> list[str]:
    """Playable .DXR scenes from the generated movie registry."""
    names = []
    for line in (game.work / "director/c/movie_names.inc").read_text().splitlines():
        stem = line.strip().strip('",')
        if stem.endswith("_dxr"):
            names.append(stem[:-4].upper() + ".DXR")
    return names


def degradation(windows: list[dict]) -> dict | None:
    """Early-versus-late mean service cost for one movie's report windows.

    A ratio well above 1.0 means the same movie got more expensive to
    service the longer the walk ran — the host-side signature of
    accumulating state (interned-forever objects, growing lists, collector
    pressure) that hardware sessions experience as progressive slowdown."""
    if len(windows) < 4:
        return None
    quarter = max(1, len(windows) // 4)

    def mean(chunk: list[dict]) -> float:
        ticks = sum(w["ticks"] for w in chunk)
        return sum(w["tick_us"] for w in chunk) / ticks if ticks else 0.0

    first, last = mean(windows[:quarter]), mean(windows[-quarter:])
    return {
        "first_mean_us": round(first, 2),
        "last_mean_us": round(last, 2),
        "ratio": round(last / first, 2) if first else None,
        "windows": len(windows),
    }


def sprite_points(state: dict) -> list[tuple[int, int, int]]:
    """(sprite id, x, y) centre points of the live sprites.

    Probe coordinates are the movie's authored space, not the console's
    physical screen: the ROM scales the pointer into it. Clamping to the
    hardware envelope here would move a click off every hotspot in the
    right or lower fifth of an 800x600 stage.
    """
    points = []
    for sprite in state.get("sprites", []):
        if not sprite.get("visible", True):
            continue
        left, top, right, bottom = sprite["bounds"]
        if left >= right or top >= bottom:
            continue
        x, y = (left + right) // 2, (top + bottom) // 2
        if 0 <= x < STAGE_LIMIT and 0 <= y < STAGE_LIMIT:
            points.append((sprite["id"], x, y))
    return points


class Crawl:
    """State shared across probe restarts: what was tried, seen and measured."""

    def __init__(self, dwell: int, limit: int):
        self.probe: Probe | None = None
        self.dwell = dwell
        self.limit = limit
        self.clicks: dict[str, int] = {}
        self.tried: dict[str, set[int]] = {}
        self.banned: dict[str, set[int]] = {}
        self.blocked: set[str] = set()
        self.edges: dict[str, set[str]] = {}
        self.rows: dict[str, dict] = {}
        self.last_click: tuple[str, int] | None = None
        # Peak Lingo value-heap use per movie: the exercises load their task
        # database into it, so headroom is a portability fact worth keeping.
        self.heap: dict[str, int] = {}
        # Where each click led, so an exhausted movie can navigate back
        # through the hub instead of ending the walk: the map is a hub, and
        # every room and exercise hangs off it.
        self.destination: dict[tuple[str, int], str] = {}
        self.points: dict[tuple[str, int], tuple[int, int]] = {}
        self.frontier: dict[str, set[int]] = {}
        self.checkpoints: dict[str, int] = {}
        self.navigations = 0
        # Every report window in walk order: the degradation check compares
        # a movie's early windows against its late ones, which the summed
        # totals alone can never show.
        self.windows: list[dict] = []
        self.window_seq = 0

    def merge(self, rows: list[dict]):
        # Cost tables from restarted probe processes accumulate per movie.
        self.window_seq += 1
        for row in rows:
            total = self.rows.setdefault(
                row["movie"],
                {"movie": row["movie"], "ticks": 0, "tick_us": 0, "stage_us": 0, "max_us": 0},
            )
            for key in ("ticks", "tick_us", "stage_us"):
                total[key] += row[key]
            total["max_us"] = max(total["max_us"], row["max_us"])
            # The probe reprints reset rows with zero ticks; only windows
            # with measured ticks join the series.
            if row["ticks"]:
                self.windows.append(
                    {
                        "seq": self.window_seq,
                        "movie": row["movie"],
                        "ticks": row["ticks"],
                        "tick_us": row["tick_us"],
                    }
                )

    def record(self):
        state = self.probe.state
        movie = state.get("movie", "")
        used = state.get("heap_bytes", 0)
        if movie and used > self.heap.get(movie, 0):
            self.heap[movie] = used

    def settle(self):
        movie = self.probe.state.get("movie", "")
        self.probe.send(f"step {self.dwell} 320 240 0", ticks=self.dwell)
        self.record()
        after = self.probe.state.get("movie", "")
        if after != movie and movie:
            self.edges.setdefault(movie, set()).add(after)
        # Snapshot the cost table so a later probe failure still reports the
        # states measured up to it; the failure itself is a sweep finding.
        # The perf reply is not a state line, so refresh the state after it.
        self.merge(self.probe.send("perf").get("perf", []))
        self.probe.send("step 1 320 240 0", ticks=1)

    def checkpoint(self, index: int, movie: str):
        """Remember the shortest spine prefix that reaches each movie."""
        if movie and movie not in self.checkpoints:
            self.checkpoints[movie] = index + 1

    def saturated(self, movie: str) -> bool:
        """Scores animate, so fresh sprite ids never stop appearing; a click
        budget per movie keeps the walk moving instead of re-exploring one."""
        return self.clicks.get(movie, 0) >= self.limit

    def resume_target(self, spine: list[str]) -> str | None:
        """The deepest checkpoint that still has ground to cover."""
        ranked = sorted(self.checkpoints, key=lambda m: self.checkpoints[m], reverse=True)
        for movie in ranked:
            if movie in self.blocked or self.saturated(movie):
                continue
            if movie not in self.tried or self.frontier.get(movie):
                return movie
        return None

    def prefix(self, spine: list[str]) -> list[str]:
        """Spine commands to replay: all of it first, then to a checkpoint."""
        target = self.resume_target(spine)
        if target is None:
            return spine
        return spine[: self.checkpoints[target]]

    def unexplored(self, movie: str) -> bool:
        """A movie worth walking to: never visited, or with untried hotspots."""
        if movie in self.blocked or not movie:
            return False
        if movie not in self.tried:
            return True
        return bool(self.frontier.get(movie))

    def reaches_unexplored(self, movie: str) -> bool:
        """Whether anything worth exploring lies beyond this movie."""
        seen, queue = {movie}, [movie]
        while queue:
            current = queue.pop()
            if self.unexplored(current):
                return True
            for destination in self.edges.get(current, ()):
                if destination not in seen:
                    seen.add(destination)
                    queue.append(destination)
        return False

    def route(self, movie: str) -> tuple[int, int, int] | None:
        """A tried hotspot here that leads toward somewhere still unexplored."""
        for (source, sprite), destination in self.destination.items():
            if source != movie or destination == movie:
                continue
            if not self.reaches_unexplored(destination):
                continue
            point = self.points.get((movie, sprite))
            if point:
                return sprite, point[0], point[1]
        return None

    def action(self) -> bool:
        """One crawl step; False when the current movie offers nothing new."""
        state = self.probe.state
        if state.get("quit"):
            self.probe.send("reboot", ticks=1200)
            self.settle()
            return True
        movie = state.get("movie", "")
        tried = self.tried.setdefault(movie, set())
        skip = tried | self.banned.get(movie, set())
        points = sprite_points(state)
        for sprite, x, y in points:
            self.points[(movie, sprite)] = (x, y)
        fresh = [] if self.saturated(movie) else [p for p in points if p[0] not in skip]
        self.frontier[movie] = {p[0] for p in fresh}
        if not fresh:
            # Exhausted here: retrace a known exit toward somewhere with work
            # left. Bounded, so a pair of movies cannot ping-pong forever.
            step = self.route(movie) if self.navigations < 64 else None
            if not step:
                return False
            self.navigations += 1
            sprite, x, y = step
            self.probe.send(f"step 4 {x} {y} 1", ticks=4)
            self.probe.send(f"step 45 {x} {y} 0", ticks=45)
            self.record()
            if self.probe.state.get("movie", "") != movie:
                self.settle()
                self.destination[(movie, sprite)] = self.probe.state.get("movie", "")
            return True
        sprite, x, y = fresh[0]
        tried.add(sprite)
        self.clicks[movie] = self.clicks.get(movie, 0) + 1
        self.frontier[movie].discard(sprite)
        self.last_click = (movie, sprite)
        self.probe.send(f"step 4 {x} {y} 1", ticks=4)
        self.probe.send(f"step 45 {x} {y} 0", ticks=45)
        self.record()
        after = self.probe.state.get("movie", "")
        self.destination[(movie, sprite)] = after
        if after != movie:
            self.edges.setdefault(movie, set()).add(after)
            if after in self.blocked:
                # A known-failing movie: several overlapping hotspots lead
                # here, so ban the click and restart rather than crash again.
                return "blocked"
            self.settle()
            # A transition movie plays between scenes; the destination worth
            # recording is where the click actually came to rest.
            settled = self.probe.state.get("movie", "")
            self.destination[(movie, sprite)] = settled
            if settled != after:
                self.edges.setdefault(after, set()).add(settled)
        return True


def soak(game, args, crawl: Crawl, probe: Probe, features, output: Path, started) -> int:
    """Hold the spine's final movie for hours of game time in minutes of
    wall time, sampling cost windows and live-heap censuses.

    Progressive slowdown is a function of elapsed session time, not of any
    single scene's mean: this is the deterministic reproduction of "the
    longer it runs, the slower it gets", judged by comparing early windows
    against late ones."""
    movie = probe.state.get("movie", "")
    heap_samples = []
    rounds = max(4, args.soak // 300)
    for round_index in range(rounds):
        probe.send("step 300 320 240 0", ticks=300)
        crawl.record()
        crawl.merge(probe.send("perf").get("perf", []))
        probe.send("step 1 320 240 0", ticks=1)
        if features.get("heap") and round_index % 10 == 9:
            census = probe.send("heap").get("heap")
            probe.send("step 1 320 240 0", ticks=1)
            heap_samples.append(
                {
                    "round": round_index + 1,
                    "used": census["used"],
                    "object_handles": census["object_handles"],
                    "collections": census["collections"],
                    "emergency_collections": census["emergency_collections"],
                }
            )
        current = probe.state.get("movie", "")
        if current != movie:
            break
    outcome = probe.close()
    series = [w for w in crawl.windows if w["movie"] == movie]
    trend = degradation(series)
    handle_growth = None
    if len(heap_samples) >= 2:
        first, last = heap_samples[0], heap_samples[-1]
        handle_growth = {
            "first_handles": first["object_handles"],
            "last_handles": last["object_handles"],
            "ratio": round(last["object_handles"] / first["object_handles"], 2)
            if first["object_handles"]
            else None,
        }
    # A soak is deterministic and long, so quartile means average the host
    # noise out; a quarter more service cost at hour's end than at its start
    # is the reported "gets slower the longer it runs" and fails truthfully.
    degrading = bool(
        (trend and trend["ratio"] is not None and trend["ratio"] > 1.25)
        or (handle_growth and handle_growth["ratio"] and handle_growth["ratio"] > 1.25)
    )
    report = {
        "status": "degrading" if degrading else "passing",
        "generated": datetime.now(UTC).isoformat(timespec="seconds"),
        "mode": "soak",
        "movie": movie,
        "probe_outcome": outcome.kind,
        "soak_ticks": args.soak,
        "duration_seconds": round(time.monotonic() - started, 1),
        "windows": series,
        "degradation": trend,
        "heap_samples": heap_samples,
        "handle_growth": handle_growth,
        "held_movie": probe.state.get("movie", "") == movie,
    }
    (output / "soak.json").write_text(json.dumps(report, indent=2) + "\n")
    if trend:
        print(
            f"soak {movie}: mean tick cost {trend['first_mean_us']} -> "
            f"{trend['last_mean_us']} µs across {trend['windows']} windows"
        )
    for sample in heap_samples:
        print(
            f"  round {sample['round']}: {sample['object_handles']} handles, "
            f"{sample['used']} bytes live, {sample['collections']}"
            f"+{sample['emergency_collections']} collections"
        )
    print(json.dumps({"status": report["status"], "report": str(output / "soak.json")}))
    return 1 if degrading else 0


def main(game, argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="director64 perf", description="systematic native state walk with cost report"
    )
    parser.add_argument("--actions", type=int, default=400, help="crawl clicks after the journey")
    parser.add_argument("--dwell", type=int, default=600, help="idle ticks after each transition")
    parser.add_argument(
        "--budget-us",
        type=float,
        default=12.0,
        # Calibrated 2026-09-12 by pairing this sweep's host means with the
        # ROM's NATIVE_COST service cost in the same movies of a Gopher64
        # capture (Deutsch): MAINSCR 11.0 us host vs 10,574 us guest, i.e.
        # ~1000x on the reference host (lighter scenes measured 570-700x).
        # 12 us of host mean is therefore about 12 ms of the 16.7 ms
        # console tick at the worst measured scale, leaving the rest for
        # rendering. Re-pair after CPU or compiler changes.
        help="host mean service budget per tick",
    )
    parser.add_argument(
        "--min-ticks",
        type=int,
        default=1000,
        # Boot-chain movies tick for a second or two of setup and never
        # again; their mean is startup cost, not a steady state to judge.
        help="movies sampled for fewer ticks report but never fail the budget",
    )
    parser.add_argument(
        "--restarts",
        type=int,
        default=40,
        help="probe runs: each resumes at the deepest checkpoint with work left",
    )
    parser.add_argument(
        "--hotspots", type=int, default=50, help="clicks per movie before moving on"
    )
    parser.add_argument(
        "--soak",
        type=int,
        default=0,
        # 216000 ticks = one hour of game time; a host session runs it in
        # minutes because probe ticks are not wall-paced.
        help="instead of crawling, idle this many ticks in the spine's final "
        "movie and fail on cost or live-heap growth",
    )
    parser.add_argument("--output", type=Path, help="report directory")
    args = parser.parse_args(argv)

    features = PROBE_FEATURES.get(game.slug, DEFAULT_FEATURES)
    if not features.get("perf"):
        raise SystemExit(
            f"the {game.slug} probe does not implement the perf RPC yet; wire "
            "platforms/native/probe_stats.inc into its tests/director_probe.c "
            "and declare it in PROBE_FEATURES"
        )
    executable = game.work / "native/director-probe"
    output = args.output or game.work / "native/perf"
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ | {"DIRECTOR64_WORK_DIR": str(game.work)}
    entry = game.data["port"].get("entry_movie", "START")
    started = time.monotonic()
    crawl = Crawl(args.dwell, args.hotspots)
    findings = []
    heap_census = None
    spine = list(getattr(game.host("full_journey"), "STEPS", []))
    remaining = args.actions
    run = 0
    outcome = None
    while remaining > 0 and run < args.restarts:
        run += 1
        probe = Probe(executable, entry, output / f"stderr-{run}.log", env)
        crawl.probe = probe
        restart = False
        try:
            probe.receive(timeout=60.0)
            # The journey doubles as the route to the game's interior: its
            # per-command movie trail gives checkpoints a later run can
            # replay to, so an exhausted branch resumes at the hub instead
            # of ending the walk. A ("click", sprite) entry resolves the
            # sprite's centre from the live state, so spines survive layouts
            # the authors animated.
            for index, command in enumerate(crawl.prefix(spine)):
                if isinstance(command, tuple):
                    kind, sprite = command
                    point = next(
                        ((x, y) for s, x, y in sprite_points(probe.state) if s == sprite),
                        None,
                    )
                    if kind != "click" or point is None:
                        raise SystemExit(
                            f"spine entry {command!r} has no live sprite in "
                            f"{probe.state.get('movie', '')}"
                        )
                    probe.send(f"step 4 {point[0]} {point[1]} 1", ticks=4)
                    probe.send(f"step 45 {point[0]} {point[1]} 0", ticks=45)
                else:
                    ticks = int(command.split()[1]) if command.startswith("step ") else 60
                    probe.send(command, ticks=ticks)
                crawl.checkpoint(index, probe.state.get("movie", ""))
            if args.soak:
                return soak(game, args, crawl, probe, features, output, started)
            while remaining > 0:
                remaining -= 1
                result = crawl.action()
                if result == "blocked":
                    restart = True
                    break
                if not result:
                    # Nothing untried here: let the score run, then retry; a
                    # changed scene re-fills the frontier, a static one ends.
                    before = probe.state.get("movie", "")
                    crawl.settle()
                    state = probe.state
                    movie = state.get("movie", "")
                    skip = crawl.tried.get(movie, set()) | crawl.banned.get(movie, set())
                    if movie == before and (
                        crawl.saturated(movie) or all(p[0] in skip for p in sprite_points(state))
                    ):
                        # Resume from the best checkpoint if one still has
                        # untried hotspots; otherwise the walk is complete.
                        restart = crawl.resume_target(spine) is not None
                        break
            if not restart:
                crawl.merge(probe.send("perf").get("perf", []))
                if features.get("heap"):
                    heap_census = probe.send("heap").get("heap")
                    probe.send("step 1 320 240 0", ticks=1)
                outcome = probe.close()
                break
            crawl.merge(probe.send("perf").get("perf", []))
            probe.kill()
        except ProbeFailure as caught:
            probe.kill()
            finding = {
                "kind": caught.outcome.kind,
                "error": caught.outcome.error,
                "movie": probe.state.get("movie", ""),
                "frame": probe.state.get("frame", 0),
                "commands": probe.commands[-6:],
            }
            if not any(
                f["kind"] == finding["kind"]
                and f["error"] == finding["error"]
                and f["movie"] == finding["movie"]
                for f in findings
            ):
                findings.append(finding)
            outcome = caught.outcome
            if finding["movie"]:
                crawl.blocked.add(finding["movie"])
            if crawl.last_click is None:
                break  # the scripted spine itself failed; nothing to ban
            restart = True
        # A fresh probe replays the spine and continues, avoiding the click
        # that led into the failing state. Cleared tried sets let the
        # replayed run re-navigate through already-seen movies.
        if restart and crawl.last_click is not None:
            crawl.banned.setdefault(crawl.last_click[0], set()).add(crawl.last_click[1])
            crawl.last_click = None
            crawl.tried = {}
    rows = [row for row in crawl.rows.values() if row["ticks"]]
    series: dict[str, list[dict]] = {}
    for window in crawl.windows:
        series.setdefault(window["movie"], []).append(window)
    movies = []
    over = []
    degrading = []
    for row in sorted(rows, key=lambda r: r["tick_us"] / r["ticks"], reverse=True):
        mean = row["tick_us"] / row["ticks"]
        stage = row["stage_us"] / row["ticks"]
        entry_row = {
            "movie": row["movie"],
            "ticks": row["ticks"],
            "mean_tick_us": round(mean, 2),
            "mean_stage_us": round(stage, 2),
            "max_us": row["max_us"],
            "peak_value_heap": crawl.heap.get(row["movie"], 0),
            "over_budget": mean > args.budget_us and row["ticks"] >= args.min_ticks,
            "degradation": degradation(series.get(row["movie"], [])),
        }
        movies.append(entry_row)
        if entry_row["over_budget"]:
            over.append(row["movie"])
        trend = entry_row["degradation"]
        if (
            trend
            and trend["ratio"] is not None
            and trend["ratio"] > 1.5
            and row["ticks"] >= args.min_ticks
        ):
            degrading.append(row["movie"])
    visited = {row["movie"] for row in rows}
    status = "passing"
    if over:
        status = "over-budget"
    if findings:
        status = "failing"
    report = {
        "status": status,
        "generated": datetime.now(UTC).isoformat(timespec="seconds"),
        "probe_outcome": outcome.kind if outcome else "not-run",
        "findings": findings,
        "budget_us": args.budget_us,
        "duration_seconds": round(time.monotonic() - started, 1),
        "movies": movies,
        # Advisory until per-game budgets are measured: host quartile means
        # carry scheduler noise, so the threshold errs high.
        "degrading": degrading,
        "windows": crawl.windows,
        "heap_census": heap_census,
        "edges": {k: sorted(v) for k, v in sorted(crawl.edges.items())},
        "unvisited": sorted(set(registry_movies(game)) - visited),
        "script_alerts": [event[0] for event in probe.soft_events] if run else [],
    }
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    width = max((len(m["movie"]) for m in movies), default=5)
    print(
        f"{'movie':{width}}  {'ticks':>8}  {'tick µs':>8}  {'stage µs':>9}  "
        f"{'max µs':>8}  {'heap KiB':>8}"
    )
    for m in movies:
        flag = "  OVER" if m["over_budget"] else ""
        print(
            f"{m['movie']:{width}}  {m['ticks']:>8}  {m['mean_tick_us']:>8.1f}  "
            f"{m['mean_stage_us']:>9.1f}  {m['max_us']:>8}  "
            f"{m['peak_value_heap'] / 1024:>8.0f}{flag}"
        )
    if report["unvisited"]:
        print("unvisited:", " ".join(report["unvisited"]))
    for movie in degrading:
        trend = next(m["degradation"] for m in movies if m["movie"] == movie)
        print(
            f"degrading: {movie} mean tick cost {trend['first_mean_us']} -> "
            f"{trend['last_mean_us']} µs across {trend['windows']} windows"
        )
    if heap_census:
        largest = ", ".join(
            f"{entry['type']}:{entry['bytes']}" for entry in heap_census["largest"][:3]
        )
        print(
            f"heap census: {heap_census['object_handles']}/{heap_census['object_limit']} "
            f"handles, {heap_census['used']}/{heap_census['limit']} bytes, "
            f"{heap_census['collections']}+{heap_census['emergency_collections']} "
            f"collections, largest {largest}"
        )
    for finding in findings:
        print(f"finding: {finding['kind']} in {finding['movie']}: {finding['error']}")
    print(json.dumps({"status": report["status"], "report": str(output / "report.json")}))
    return 1 if report["status"] != "passing" else 0

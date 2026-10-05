"""Guest pacing analysis over the ROM's periodic NATIVE_* markers.

Every game's platform loop reports once per 300 service ticks (5 s of game
time):

  NATIVE_TICK  movie= frame= tick= free= cache= heap= depth= objects= high= frag= paused=
  NATIVE_COST  ticks_us= render_us= audio_us= renders= steps= gc= egc= gc_us= dropped_us= wall=
  NATIVE_AUDIO_POS channel= pos= rate=          (one line per playing channel)

tick/free/cache/heap/objects/frag are instantaneous, paused is a counter, ticks_us/render_us/
audio_us/renders cover the interval since the previous report, and steps/gc/
egc/dropped_us/wall are monotonic counters that this module diffs.  wall is
the guest hardware counter in microseconds, so on the emulator and on the
console alike `tick_delivery` answers the question the service clock poses:
did the loop deliver 60 ticks per guest wall second?  Everything the score
runs — animation, Lingo timers, film loops — slides together when it did
not, while streamed audio keeps real time; sustained delivery below 1.0 IS
the audio desync and the slow-motion gameplay observed on hardware.

Older captures lack the extended fields; rows then carry None for the
derived metrics instead of failing, so the analyzer also reads historical
logs.
"""

from __future__ import annotations

import re
from typing import Any

MARKER = re.compile(
    r"^DIRECTOR64 (NATIVE_TICK|NATIVE_COST|NATIVE_AUDIO_POS|"
    r"NATIVE_TIMING_OVERRUN|NATIVE_IMAGE_REPACK)\b(.*)$",
    re.M,
)
FIELD = re.compile(r"(\w+)=(-?\d+\b|\S+)")

# A 300-tick report interval is five seconds of game time.
INTERVAL_TICKS = 300

# Default judgement thresholds; assert_pacing callers can override any of
# them. Delivery is judged on sustained windows: a single slow interval is a
# scene load, which the original also never repaid.
DEFAULT_BUDGET = {
    "min_tick_delivery": 0.90,
    "sustained_windows": 3,
    "max_object_growth": 1.25,
    "max_heap_growth": 1.25,
    "min_growth_windows": 8,
}


def _fields(text: str) -> dict[str, Any]:
    return {
        key: int(value) if re.fullmatch(r"-?\d+", value) else value
        for key, value in FIELD.findall(text)
    }


def parse_windows(log: str) -> list[dict[str, Any]]:
    """Report windows in emission order, with the events since the previous
    report (overruns, repacks) attributed to the window that follows them."""
    windows: list[dict[str, Any]] = []
    overruns = repacks = 0
    pending: dict[str, Any] | None = None
    current: dict[str, Any] | None = None
    for match in MARKER.finditer(log):
        kind, values = match.group(1), _fields(match.group(2))
        if kind == "NATIVE_TIMING_OVERRUN":
            overruns += 1
        elif kind == "NATIVE_IMAGE_REPACK":
            repacks += 1
        elif kind == "NATIVE_TICK":
            pending = {"tick": values, "overruns": overruns, "repacks": repacks, "audio": []}
            overruns = repacks = 0
        elif kind == "NATIVE_COST" and pending is not None:
            pending["cost"] = values
            windows.append(pending)
            current = pending
            pending = None
        elif kind == "NATIVE_AUDIO_POS" and current is not None:
            current["audio"].append(values)
    return windows


def _delta(previous: dict, window: dict, key: str) -> int | None:
    a, b = previous["cost"].get(key), window["cost"].get(key)
    # A field a corrupted line left non-numeric reads as absent.
    if not isinstance(a, int) or not isinstance(b, int) or b < a:
        return None
    return b - a


def _tick_delta(previous: dict, window: dict, key: str) -> int | None:
    """As _delta, over the instantaneous NATIVE_TICK fields that are counters.
    Absent in older captures, which then read as no pauses at all."""
    a, b = previous["tick"].get(key), window["tick"].get(key)
    if not isinstance(a, int) or not isinstance(b, int) or b < a:
        return None
    return b - a


def _drift(previous: dict, window: dict, guest_seconds: float) -> float | None:
    """Guest-time progress over real audio-time progress, for channels that
    kept playing the same-rate stream across the interval. 1.0 = in sync;
    below 1.0 the score ran late against the audio."""
    ratios = []
    before = {entry["channel"]: entry for entry in previous["audio"] if entry.get("rate")}
    for entry in window["audio"]:
        rate = entry.get("rate")
        prior = before.get(entry.get("channel"))
        if not rate or not prior or prior["rate"] != rate or entry["pos"] <= prior["pos"]:
            continue
        audio_seconds = (entry["pos"] - prior["pos"]) / rate
        # A restarted stream can also show a small forward position; a
        # plausible interval is one comparable to the guest interval.
        if audio_seconds > 4 * guest_seconds:
            continue
        ratios.append(guest_seconds / audio_seconds)
    return min(ratios) if ratios else None


def analyze(log: str) -> dict[str, Any]:
    windows = parse_windows(log)
    rows: list[dict[str, Any]] = []
    if not windows:
        # A log too short to carry one complete report window -- a run that
        # failed closed before it got going, or an older capture. Summarize
        # nothing rather than failing the caller that asked.
        return {"windows": rows, "summary": summarize(rows)}
    for previous, window in zip([None, *windows[:-1]], windows, strict=True):
        tick, cost = window["tick"], window["cost"]
        row: dict[str, Any] = {
            "movie": tick.get("movie"),
            "tick": tick.get("tick"),
            "free": tick.get("free"),
            "heap": tick.get("heap"),
            "objects": tick.get("objects"),
            "frag": tick.get("frag"),
            "overruns": window["overruns"],
            "repacks": window["repacks"],
            "renders": cost.get("renders"),
            "work_us": sum(cost.get(key) or 0 for key in ("ticks_us", "render_us", "audio_us")),
            "service_us": cost.get("ticks_us"),
            "wall_delta_us": None,
            "tick_delivery": None,
            "fps": None,
            "work_ratio": None,
            "gc": None,
            "egc": None,
            "gc_us": None,
            "dropped_us": None,
            "paused_steps": None,
            "steps": None,
            "audio_sync": None,
        }
        if previous is not None:
            wall = _delta(previous, window, "wall")
            ticks = None
            if isinstance(tick.get("tick"), int) and isinstance(previous["tick"].get("tick"), int):
                ticks = tick["tick"] - previous["tick"]["tick"]
            # Steps the loop consumed without ticking the score, because the
            # port was holding it on purpose: a modal notice, the controller
            # keyboard. Guest wall time passes and the score's clock does not,
            # so counting them as undelivered reads a deliberate hold exactly
            # like a console too slow to keep up. Willy's login screen scores
            # 0.497 without them and 0.970 with.
            paused = _tick_delta(previous, window, "paused") or 0
            row["paused_steps"] = paused
            if wall and ticks and ticks > 0:
                row["wall_delta_us"] = wall
                row["tick_delivery"] = ((ticks + paused) / 60) * 1_000_000 / wall
                if isinstance(cost.get("renders"), int):
                    row["fps"] = cost["renders"] * 1_000_000 / wall
                row["work_ratio"] = row["work_us"] / wall
                row["audio_sync"] = _drift(previous, window, ticks / 60)
            row["gc"] = _delta(previous, window, "gc")
            row["egc"] = _delta(previous, window, "egc")
            row["gc_us"] = _delta(previous, window, "gc_us")
            row["dropped_us"] = _delta(previous, window, "dropped_us")
            row["steps"] = _delta(previous, window, "steps")
        rows.append(row)
    return {"windows": rows, "summary": summarize(rows)}


def _quartile_growth(rows: list[dict], key: str) -> dict[str, Any] | None:
    values = [row[key] for row in rows if isinstance(row.get(key), int)]
    if len(values) < 4:
        return None
    quarter = max(1, len(values) // 4)
    first = sum(values[:quarter]) / quarter
    last = sum(values[-quarter:]) / quarter
    return {
        "first_quartile_mean": first,
        "last_quartile_mean": last,
        "ratio": last / first if first else None,
        "samples": len(values),
    }


def sustained_slow_run(rows: list[dict], limit: float) -> int:
    """Longest run of consecutive windows delivering ticks below `limit`."""
    sustained = worst = 0
    for row in rows:
        below = row["tick_delivery"] is not None and row["tick_delivery"] < limit
        sustained = sustained + 1 if below else 0
        worst = max(worst, sustained)
    return worst


def _service_us_per_step(rows: list[dict]) -> float | None:
    """Interpreter cost per Lingo step across the windows that report both.

    Tick delivery hides interpreter regressions wherever a scene has slack:
    a runtime layout change once cost Mucklas's train 17% per step while its
    delivery did not move. Steps are the authored work, so their cost is
    comparable across builds as long as the scenario replays the same input.
    """
    measured = [
        row
        for row in rows
        if isinstance(row.get("steps"), int) and row["steps"] > 0 and row["service_us"] is not None
    ]
    steps = sum(row["steps"] for row in measured)
    return sum(row["service_us"] for row in measured) / steps if steps else None


def summarize(rows: list[dict]) -> dict[str, Any]:
    """Judgement fields over analyzed windows; a caller that filters the
    windows to one movie summarizes the filtered list."""
    deliveries = [row["tick_delivery"] for row in rows if row["tick_delivery"] is not None]
    syncs = [row["audio_sync"] for row in rows if row["audio_sync"] is not None]
    slow = [row for row in rows if row["tick_delivery"] is not None and row["tick_delivery"] < 1]
    worst_run = sustained_slow_run(rows, DEFAULT_BUDGET["min_tick_delivery"])
    per_movie: dict[str, dict[str, Any]] = {}
    for row in rows:
        movie = row.get("movie")
        if not movie:
            continue
        entry = per_movie.setdefault(
            movie, {"windows": 0, "min_tick_delivery": None, "max_work_ratio": None}
        )
        entry["windows"] += 1
        if row["tick_delivery"] is not None:
            current = entry["min_tick_delivery"]
            entry["min_tick_delivery"] = (
                row["tick_delivery"] if current is None else min(current, row["tick_delivery"])
            )
        if row["work_ratio"] is not None:
            current = entry["max_work_ratio"]
            entry["max_work_ratio"] = (
                row["work_ratio"] if current is None else max(current, row["work_ratio"])
            )
    return {
        "windows": len(rows),
        "measurable_windows": len(deliveries),
        "min_tick_delivery": min(deliveries) if deliveries else None,
        "mean_tick_delivery": sum(deliveries) / len(deliveries) if deliveries else None,
        "slow_windows": len(slow),
        "worst_sustained_slow_run": worst_run,
        "min_audio_sync": min(syncs) if syncs else None,
        "total_overruns": sum(row["overruns"] for row in rows),
        "total_repacks": sum(row["repacks"] for row in rows),
        "total_dropped_us": sum(row["dropped_us"] or 0 for row in rows),
        "total_gc": sum(row["gc"] or 0 for row in rows),
        "total_egc": sum(row["egc"] or 0 for row in rows),
        "total_gc_us": sum(row["gc_us"] or 0 for row in rows),
        "service_us_per_step": _service_us_per_step(rows),
        "object_growth": _quartile_growth(rows, "objects"),
        "heap_growth": _quartile_growth(rows, "heap"),
        "movies": per_movie,
    }


def assert_pacing(report: dict[str, Any], budget: dict[str, Any] | None = None) -> None:
    """Raise ValueError when the analyzed run violates its pacing budget.

    Callers gate what their scenario has been measured to sustain; recording
    the report without asserting is the first step for a new scenario."""
    limits = DEFAULT_BUDGET | (budget or {})
    summary = report["summary"]
    failures = []
    # Recounted against the caller's threshold: the summary's own run is
    # measured at the default, so trusting it would silently ignore an
    # override and judge every scenario by the same delivery floor.
    worst_run = sustained_slow_run(report["windows"], limits["min_tick_delivery"])
    if worst_run >= limits["sustained_windows"]:
        failures.append(
            f"tick delivery below {limits['min_tick_delivery']} for "
            f"{worst_run} consecutive windows "
            f"(min {summary['min_tick_delivery']:.3f})"
        )
    growth_limits = (("object_growth", "max_object_growth"), ("heap_growth", "max_heap_growth"))
    for key, limit_key in growth_limits:
        growth = summary.get(key)
        if (
            growth
            and growth["samples"] >= limits["min_growth_windows"]
            and growth["ratio"] is not None
            and growth["ratio"] > limits[limit_key]
        ):
            failures.append(
                f"{key.replace('_', ' ')} {growth['ratio']:.2f}x across the run "
                f"({growth['first_quartile_mean']:.0f} -> {growth['last_quartile_mean']:.0f})"
            )
    ceiling = limits.get("max_service_us_per_step")
    per_step = summary.get("service_us_per_step")
    if ceiling is not None and per_step is not None and per_step > ceiling:
        failures.append(
            f"service cost {per_step:.2f} us per Lingo step exceeds {ceiling:.2f} "
            "(an interpreter or runtime-layout regression; re-measure deliberately)"
        )
    if failures:
        raise ValueError("pacing budget violated: " + "; ".join(failures))

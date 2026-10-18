"""Judge Android worker pacing from logcat's active-time report windows.

Run ``uv run python -m director64.android_pacing LOGFILE`` after capturing
``DIRECTOR64 ANDROID_PACING`` lines. A package build cannot qualify device
performance; this command deliberately rejects empty or incomplete captures.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

MARKER = "DIRECTOR64 ANDROID_PACING "
FIELDS = re.compile(r"(\w+)=(\S+)")
NUMBERS = (
    "sequence",
    "frame",
    "window_us",
    "ticks",
    "renders",
    "service_us",
    "render_us",
    "present_us",
    "dropped_us",
    "alerts",
)


def analyze(log: str) -> dict[str, Any]:
    """Read complete windows, retaining malformed reports as explicit errors.

    Each report covers active worker time; intentional holds reset its window.
    Costs and ticks are interval counts. Dropped time is cumulative and may
    reset when an activity restarts. No inferred difference spans a restart.
    """
    rows = []
    errors = []
    previous = None
    for line_number, line in enumerate(log.splitlines(), 1):
        if MARKER not in line:
            continue
        fields = dict(FIELDS.findall(line.split(MARKER, 1)[1]))
        try:
            row = {key: int(fields[key]) for key in NUMBERS}
            row["movie"] = fields["movie"]
            if any(row[key] < 0 for key in NUMBERS) or row["window_us"] < 1_000_000:
                raise ValueError("negative field or incomplete active-time window")
        except (KeyError, ValueError) as error:
            errors.append(f"line {line_number}: invalid pacing report ({error})")
            continue
        elapsed = row["window_us"]
        row["tick_delivery"] = row["ticks"] * 1_000_000 / (60 * elapsed)
        row["fps"] = row["renders"] * 1_000_000 / elapsed
        row["work_ratio"] = (
            sum(
                row[key]
                for key in (
                    "service_us",
                    "render_us",
                    "present_us",
                )
            )
            / elapsed
        )
        row["dropped_delta_us"] = None
        if (
            previous is not None
            and row["sequence"] > previous["sequence"]
            and row["dropped_us"] >= previous["dropped_us"]
        ):
            row["dropped_delta_us"] = row["dropped_us"] - previous["dropped_us"]
        rows.append(row)
        previous = row
    elapsed = sum(row["window_us"] for row in rows)
    return {
        "windows": rows,
        "errors": errors,
        "summary": {
            "windows": len(rows),
            "active_seconds": elapsed / 1_000_000,
            "tick_delivery": (
                sum(row["ticks"] for row in rows) * 1_000_000 / (60 * elapsed) if elapsed else None
            ),
            "min_tick_delivery": min((row["tick_delivery"] for row in rows), default=None),
            "render_fps": (
                sum(row["renders"] for row in rows) * 1_000_000 / elapsed if elapsed else None
            ),
            "observed_dropped_us": sum(row["dropped_delta_us"] or 0 for row in rows),
            "max_alerts": max((row["alerts"] for row in rows), default=0),
        },
    }


def assert_pacing(
    report: dict[str, Any],
    *,
    min_delivery: float = 0.95,
    sustained_windows: int = 3,
    min_windows: int = 10,
) -> None:
    """Reject insufficient evidence, script alerts and sustained service loss.

    Rendering need not run at 60 Hz: authored films can run more slowly or
    remain still. Service delivery is measured independently of redraws.
    A fresh activity sequence breaks a sustained run; scene changes do not.
    """
    if not 0 < min_delivery <= 1 or sustained_windows < 1 or min_windows < 1:
        raise ValueError("invalid pacing budget")
    failures = list(report["errors"])
    rows = report["windows"]
    if len(rows) < min_windows:
        failures.append(f"insufficient pacing measurements: {len(rows)}; need {min_windows}")
    run = worst = 0
    previous_sequence = -1
    for row in rows:
        if row["sequence"] <= previous_sequence:
            run = 0
        run = run + 1 if row["tick_delivery"] < min_delivery else 0
        previous_sequence = row["sequence"]
        worst = max(worst, run)
    if worst >= sustained_windows:
        failures.append(f"tick delivery below {min_delivery:.3f} for {worst} consecutive windows")
    if report["summary"]["max_alerts"]:
        failures.append(f"script alerts: {report['summary']['max_alerts']}")
    if failures:
        raise ValueError("Android pacing qualification failed: " + "; ".join(failures))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--min-delivery", type=float, default=0.95)
    parser.add_argument("--sustained-windows", type=int, default=3)
    parser.add_argument("--min-windows", type=int, default=10)
    args = parser.parse_args(argv)
    try:
        report = analyze(args.log.read_text())
        print(json.dumps(report, indent=2))
        assert_pacing(
            report,
            min_delivery=args.min_delivery,
            sustained_windows=args.sustained_windows,
            min_windows=args.min_windows,
        )
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Run normal rendered journeys at Android's cache budget and audit raster work.

All media stays local. Host timing ranks expensive scenes; deterministic work
limits catch known algorithmic faults even on a desktop that renders them fast.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
from datetime import UTC, datetime
from pathlib import Path

from director64 import android, journeys, web
from director64.project import GameSpec, repository


def fresh_build(game: GameSpec) -> dict:
    site = web.site_path(game)
    receipt = json.loads((site / "build.json").read_text())
    if (receipt["runtime"]["inputs_sha256"] != web.runtime_inputs(game) or
            receipt["runtime"]["wasm_sha256"] != web.sha256(site / "director64.wasm") or
            receipt["package"]["sha256"] != web.sha256(site / "game.d64p") or
            receipt["profile"]["source"] != game.source["id"]):
        raise ValueError(f"{game.slug}: stale web runtime; rebuild before qualifying")
    return receipt


def harness_inputs(root: Path) -> dict[str, str]:
    paths = [*root.glob("src/director64/*.py"), *root.glob("platforms/web/node/*.mjs"),
             *root.glob("games/*/journeys.json"), *root.glob("games/*/host/*.py"), Path(__file__),
             root / ".mise-tasks/test-journeys"]
    return {str(path.resolve().relative_to(root)): web.sha256(path) for path in paths}


def audit(summary: dict, expected: set[tuple[str, str]] | None = None) -> dict:
    findings, hotspots = [], []
    for row in summary.get("results", []):
        source = {"game": row.get("game"), "journey": row.get("journey")}
        work = row.get("performance", {}).get("render_work", {})
        if row.get("status") != "passing":
            findings.append(source | {"error": row.get("error", "journey did not pass")})
        if not work.get("available"):
            findings.append(source | {"error": "missing renderer work metrics; rebuild runtime"})
        if not work.get("totals", {}).get("scene_frames"):
            findings.append(source | {
                "error": "no composited scenes; state-only runs cannot qualify"})
        findings.extend(source | failure for failure in work.get("findings", []))
        for sample in work.get("hotspots", []):
            if sample.get("image_budget") != android.IMAGE_BUDGET:
                findings.append(source | {"error": "probe did not use Android's image budget"})
            hotspots.append(source | sample)
    if not summary.get("results"):
        findings.append({"error": "no rendered journeys ran"})
    if summary.get("unavailable"):
        findings.append({"error": "some requested ports were unavailable"})
    if expected is not None:
        actual = {(row.get("game"), row.get("journey")) for row in summary.get("results", [])}
        if actual != expected:
            findings.append({"error": "journey coverage differs from the current catalog",
                             "missing": sorted(expected - actual),
                             "unexpected": sorted(actual - expected)})
    hotspots.sort(key=lambda row: row["peak_render_ms"], reverse=True)
    return {"status": "passed" if not findings else "failed", "findings": findings,
            "hotspots": hotspots[:50], "journeys": len(summary.get("results", [])),
            "games": len({row.get("game") for row in summary.get("results", [])}),
            "image_budget": android.IMAGE_BUDGET, "hardware_qualified": False,
            "scope": "normal rendered input routes; bitmap scans, transforms, cache and host costs"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("build/validation/performance") /
                        datetime.now(UTC).strftime("%Y%m%dT%H%M%SZ"))
    parser.add_argument("--no-build", action="store_true", help="require fresh existing web builds")
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--baseline", type=Path, help="previous performance run on the same host")
    parser.add_argument("--max-slowdown", type=float, default=1.25)
    args = parser.parse_args()
    if (not math.isfinite(args.timeout) or args.timeout <= 0 or
            not math.isfinite(args.max_slowdown) or args.max_slowdown < 1):
        parser.error("timeout must be positive and slowdown must be finite and >= 1")
    root = repository()
    output = (root / args.output).resolve()
    if not output.is_relative_to(root / "build"):
        parser.error("output must remain under ignored build/")
    output.mkdir(parents=True, exist_ok=False)
    games = [GameSpec.load(path.parent.name) for path in sorted(root.glob("games/*/game.toml"))]
    games = [game for game in games if game.data["status"] != "catalogued"]
    expected = {(game.slug, plan["name"]) for game in games for plan in journeys.plans(game)}
    if not args.no_build:
        subprocess.run([str(root / ".mise-tasks/build-web-all"), "--output",
                        str(output / "builds")], cwd=root, check=True)
    builds = {game.slug: fresh_build(game) for game in games}
    harness = harness_inputs(root)
    baseline = (["--baseline", str(args.baseline.resolve() / "journeys"),
                 "--max-slowdown", str(args.max_slowdown)] if args.baseline else [])
    result = subprocess.run([str(root / ".mise-tasks/test-journeys"), "--output",
                             str(output / "journeys"), "--timeout", str(args.timeout), *baseline],
                            cwd=root, env=os.environ | {
                                "DIRECTOR64_PROBE_IMAGE_BUDGET": str(android.IMAGE_BUDGET)},
                            check=False)
    summary = json.loads((output / "journeys/summary.json").read_text())
    report = audit(summary, expected)
    report["harness_sha256"] = harness
    for game in games:
        try:
            if fresh_build(game) != builds[game.slug]:
                raise ValueError("build changed during profiling")
            if any(row.get("build") != builds[game.slug] for row in summary["results"]
                   if row["game"] == game.slug):
                raise ValueError("journey used a different build")
        except (ValueError, KeyError, OSError) as error:
            report["findings"].append({"game": game.slug,
                                       "error": f"final source freshness: {error}"})
    if harness_inputs(root) != harness:
        report["findings"].append({"error": "profiling harness or input plans changed during run"})
    if report["findings"]:
        report["status"] = "failed"
    if result.returncode:
        report["status"] = "failed"
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: value for key, value in report.items()
                      if key not in ("hotspots", "harness_sha256")}))
    for row in report["hotspots"][:10]:
        print(f"{row['game']}/{row['journey']} {row['peak_render_movie']} "
              f"frame {row['peak_render_frame']}: {row['peak_render_ms']:.2f} host ms, "
              f"{row['peak_frame_pixels']} bitmap / "
              f"{row['peak_frame_inverse_pixels']} inverse-map pixels")
    return int(report["status"] != "passed")


if __name__ == "__main__":
    raise SystemExit(main())

"""Opt-in static rendering preflight for a recovered, normalized model.

This inventories authored requirements, not reachability. Strict mode rejects
score features the selected renderer cannot draw. It does not certify visuals.
"""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path


def audit_model(model: dict, target: str) -> dict:
    if target not in {"web", "android", "n64"}:
        raise ValueError(f"unknown renderer: {target}")
    movies = {movie["name"].upper(): movie for movie in model["movies"]}
    versions = sorted({movie["directorVersion"] for movie in movies.values()})
    if not versions or any(version not in {500, 600, 700, 800, 850, 1000} for version in versions):
        raise ValueError(f"unsupported or absent Director versions: {versions}")
    # The D8.5 ports use the D10 runtime family (family.h).
    family = 10 if max(versions) >= 850 else max(versions) // 100
    modern = family != 6 or model.get("extendedD6", False)
    inks = {0, 8, 36}
    if modern:
        inks |= {32, 39}
        if target != "n64":
            inks |= {1, 5, 35}
    if family == 10:
        inks.add(9)
    findings: dict[tuple, dict] = {}
    unresolved = 0

    def record(code: str, movie: dict, frame: dict, channel: dict, **detail) -> None:
        key = (code, *detail.items())
        finding = findings.setdefault(key, {"code": code, **detail, "records": 0, "examples": []})
        finding["records"] += 1
        if len(finding["examples"]) < 3:
            finding["examples"].append(
                {"movie": movie["name"], "frame": frame["number"], "sprite": channel["channel"] - 5}
            )

    for movie in movies.values():
        members = {(member.get("cast", 1), member["number"]): member for member in movie["members"]}
        for cast in movie.get("casts", []):
            external_name = cast.get("file", movie["name"]).upper()
            if external_name == movie["name"].upper():
                continue
            external = movies.get(external_name)
            if external is None:
                raise ValueError(f"{movie['name']}: missing external cast {external_name}")
            for member in external["members"]:
                # scene.rs binds an external cast to that file's cast 1.
                if member.get("cast", 1) == 1:
                    members[cast["number"], member["number"]] = member
        for frame in movie.get("score", {}).get("frames", []):
            for channel in frame["channels"]:
                if channel["channel"] < 6:
                    continue
                data = channel["bytes"]
                if len(data) != (48 if family >= 7 else 24):
                    raise ValueError(f"{movie['name']}: invalid normalized sprite record length")
                if not data[0]:
                    continue
                cast = data[4] * 256 + data[5]
                number = data[6] * 256 + data[7]
                member = members.get((cast, number))
                if not member:
                    # QuickDraw shapes need no member, and scripts can supply
                    # dynamic members. Unresolved records need runtime review.
                    if number and data[0] >= 16:
                        unresolved += 1
                    continue
                kind = member["type"]
                ink = data[1] & 63
                if (
                    kind in {1, 2}
                    and member.get("width")
                    and member.get("height")
                    and ink not in inks
                ):
                    record("unsupported-bitmap-ink", movie, frame, channel, ink=ink)
                if kind == 9:
                    record("linked-movie-not-rendered", movie, frame, channel)

    approximations = Counter(
        note.get("kind", "unknown") for note in model.get("approximations", [])
    )
    return {
        "target": target,
        "director_versions": versions,
        "runtime_family": family,
        "movies": len(movies),
        "problems": model.get("problems", []),
        "findings": list(findings.values()),
        "unresolved_member_records": unresolved,
        "approximation_counts": dict(sorted(approximations.items())),
        "scope": "Authored score records; scripts, visibility and reachability are not simulated.",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("--target", choices=("web", "android", "n64"), required=True)
    parser.add_argument(
        "--strict", action="store_true", help="exit 1 on findings or conversion problems"
    )
    args = parser.parse_args(argv)
    try:
        report = audit_model(json.loads(args.model.read_text()), args.target)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, f"audit input error: {error}\n")
    print(json.dumps(report, indent=2))
    return int(args.strict and bool(report["findings"] or report["problems"]))


if __name__ == "__main__":
    raise SystemExit(main())

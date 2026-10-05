"""Connect every recovered media script to generated native handlers.

Compilation is not an execution/fidelity claim. Legacy missing-source calls and
presentation approximations remain explicit in the generated report.
"""

from __future__ import annotations

import hashlib
import json
from collections import Counter
from pathlib import Path

from director64.lingo import walk
from director64.project import work_dir


def build(source: dict, program: dict, aot: dict) -> dict:
    compiled = {m["movie"]: m for m in aot["movies"]}
    result = []
    for movie in source["files"]:
        members = {m["id"]: m for m in movie["members"]}
        libraries = {c["library_id"]: c["name"] for c in movie["libraries"]}
        handlers = [h for h in program["handlers"] if h["movie"] == movie["name"]]
        if len(handlers) != compiled[movie["name"]]["handlers"]:
            raise ValueError("generated native handler count differs from recovered source")
        for script in movie["scripts"]:
            member = members[script["member_id"]]
            cast = libraries[member["library_id"]]
            native = [
                h
                for h in handlers
                if h["member"] == member["member_id"] and h["cast"].casefold() == cast.casefold()
            ]
            if Counter(h["name"].casefold() for h in script["handlers"]) != Counter(
                h["name"] for h in native
            ):
                raise ValueError(
                    f"missing/duplicated native source mapping: "
                    f"{movie['name']}:{cast}:{member['member_id']}"
                )
            result.append(
                {
                    "script_id": script["id"],
                    "source_sha256": script["content_sha256"],
                    "movie": movie["name"],
                    "cast": cast,
                    "member": member["member_id"],
                    "native_file": compiled[movie["name"]]["file"],
                    "handlers": [h["id"] for h in script["handlers"]],
                    "disposition": "compiled" if native else "empty-script",
                }
            )
    start = next(m for m in source["files"] if m["name"] == "START.DXR")
    same_text = {s["lingo"]: s["id"] for s in start["scripts"]}
    launcher = []
    for script in source["supplemental_projector"]["scripts"]:
        if script["lingo"] not in same_text:
            raise ValueError("projector startup differs from native START; review required")
        launcher.append(
            {
                "script_id": script["id"],
                "equivalent_start_script": same_text[script["lingo"]],
                "disposition": "identical-recovered-source-in-START",
            }
        )
    unresolved = []
    for handler in program["handlers"]:
        for node in walk(handler["body"]):
            if (
                node
                and node[0] == "call"
                and node[1] in {"getruta", "startfjader", "up", "play_movie", "play_done"}
            ):
                unresolved.append(
                    {
                        "movie": handler["movie"],
                        "cast": handler["cast"],
                        "member": handler["member"],
                        "handler": handler["name"],
                        "call": node[1],
                        "disposition": "retained-fail-closed-legacy-call",
                    }
                )
    return {
        "version": 1,
        "status": "compiled",
        "native_execution_verified": False,
        "scripts": result,
        "projector": launcher,
        "legacy_calls": unresolved,
        "counts": {
            "media_scripts": len(result),
            "native_handlers": len(program["handlers"]),
            "projector_scripts_equivalent_to_start": len(launcher),
        },
    }


def main():
    paths = [
        Path(p)
        for p in (
            f"{work_dir()}/analysis/source/manifest.json",
            f"{work_dir()}/aot/program.json",
            f"{work_dir()}/aot/c/manifest.json",
        )
    ]
    report = build(*(json.loads(p.read_text()) for p in paths))
    for movie in json.loads(paths[2].read_text())["movies"]:
        if (
            hashlib.sha256((paths[2].parent / movie["file"]).read_bytes()).hexdigest()
            != movie["sha256"]
        ):
            raise ValueError("generated native C differs from compiler manifest")
    report["input_sha256"] = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    Path(f"{work_dir()}/director/accountability.json").write_text(
        json.dumps(report, indent=2) + "\n"
    )
    print(json.dumps(report["counts"]))


if __name__ == "__main__":
    main()

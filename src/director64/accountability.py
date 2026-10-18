"""Account for recovered scripts in ports using the shared Rust pipeline."""

import hashlib
import json
from collections import Counter


def build(source, program, generated, launcher_movie=None):
    compiled = {m["movie"]: m for m in generated["movies"]}
    units = [(unit["name"], unit) for unit in source["files"]]
    if launcher_movie:
        units.append((launcher_movie, source["supplemental_projector"]))
    result = []
    for movie, unit in units:
        members = {m["id"]: m for m in unit["members"]}
        libraries = {c["library_id"]: c["name"] for c in unit["libraries"]}
        handlers = [h for h in program["handlers"] if h["movie"] == movie]
        if movie not in compiled or len(handlers) != compiled[movie]["handlers"]:
            raise ValueError(f"native handler count mismatch: {movie}")
        accounted = 0
        for script in unit["scripts"]:
            member = members[script["member_id"]]
            cast = libraries[member["library_id"]]
            native = [h for h in handlers if h["member"] == member["member_id"]
                      and h["cast"].casefold() == cast.casefold()]
            if Counter(h["name"].casefold() for h in script["handlers"]) != Counter(
                    h["name"].casefold() for h in native):
                raise ValueError(
                    f"native source mapping mismatch: {movie}:{cast}:{member['member_id']}"
                )
            accounted += len(native)
            result.append({"script_id": script["id"], "source_sha256": script["content_sha256"],
                           "movie": movie, "cast": cast, "member": member["member_id"],
                           "native_file": compiled[movie]["file"],
                           "handlers": [h["id"] for h in script["handlers"]],
                           "disposition": "compiled" if native else "empty-script"})
        if accounted != len(handlers):
            raise ValueError(f"native handlers without recovered script: {movie}")
    if set(compiled) != {name for name, _ in units}:
        raise ValueError("native units differ from recovered movies")
    return {"version": 1, "status": "compiled", "native_execution_verified": False,
            "compatibility_dispositions": program.get("compatibility_fixes", []),
            "scripts": result,
            "projector_archive_dispositions": source.get("projector_archive_dispositions", []),
            "counts": {"scripts": len(result), "native_handlers": len(program["handlers"]),
                       "native_units": len(compiled)}}


def write(game, program_path):
    source = json.loads((game.work / "analysis/source/manifest.json").read_text())
    program = json.loads(program_path.read_text())
    generated = json.loads((game.work / "aot/c/manifest.json").read_text())
    report = build(source, program, generated, game.data["port"].get("launcher_movie"))
    for movie in generated["movies"]:
        digest = hashlib.sha256((game.work / "aot/c" / movie["file"]).read_bytes()).hexdigest()
        if digest != movie["sha256"]:
            raise ValueError(f"generated C hash mismatch: {movie['movie']}")
    (game.work / "director/accountability.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["counts"]))

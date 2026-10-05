"""Map every D5 media/projector handler to its generated native unit."""

import hashlib
import json
from collections import Counter

from director64.project import selected_game


def main():
    game = selected_game()
    source = json.loads((game.work / "analysis/source/manifest.json").read_text())
    program = json.loads((game.work / "aot/native-program.json").read_text())
    aot = json.loads((game.work / "aot/c/manifest.json").read_text())
    compiled = {m["movie"]: m for m in aot["movies"]}
    result = []
    for unit in [*source["files"], source["supplemental_projector"]]:
        movie = "START.DXR" if unit is source["supplemental_projector"] else unit["name"]
        members = {m["id"]: m for m in unit["members"]}
        libraries = {c["library_id"]: c["name"] for c in unit["libraries"]}
        handlers = [h for h in program["handlers"] if h["movie"] == movie]
        if len(handlers) != compiled[movie]["handlers"]:
            raise ValueError(f"native handler count mismatch: {movie}")
        for script in unit["scripts"]:
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
                    f"native source mapping mismatch: {movie}:{cast}:{member['member_id']}"
                )
            result.append(
                {
                    "script_id": script["id"],
                    "source_sha256": script["content_sha256"],
                    "movie": movie,
                    "cast": cast,
                    "member": member["member_id"],
                    "native_file": compiled[movie]["file"],
                    "handlers": [h["id"] for h in script["handlers"]],
                    "disposition": "compiled" if native else "empty-script",
                }
            )
    for movie in compiled.values():
        if (
            hashlib.sha256((game.work / "aot/c" / movie["file"]).read_bytes()).hexdigest()
            != movie["sha256"]
        ):
            raise ValueError("generated C hash mismatch")
    report = {
        "version": 1,
        "status": "compiled",
        "native_execution_verified": False,
        "scripts": result,
        "counts": {
            "scripts": len(result),
            "native_handlers": len(program["handlers"]),
            "native_units": len(compiled),
        },
    }
    (game.work / "director/accountability.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["counts"]))

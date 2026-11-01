#!/usr/bin/env python3
"""Prove regression oracles reject historical classes of silent faults.

Only isolated copies under build/ are mutated; the worktree and game data are
never modified. Controls must pass and each injected defect must be detected.
"""

from __future__ import annotations

import hashlib
import json
import os
import resource
import shutil
import subprocess
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    output = root / "build/validation/mutations"
    output.mkdir(parents=True, exist_ok=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    flags = ["-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-DDIRECTOR64_DIRECTOR_VERSION=10", "-DDIRECTOR64_EXTENDED_D6=1",
             "-DDIRECTOR64_NATIVE_STAGE=1", "-DDIRECTOR64_D85=1"]
    flags += [f"-I{root / path}" for path in (
        "runtime/director", "runtime/lingo", "runtime/interaction",
        "runtime/storage", "platforms/web")]
    cases = [
        {"id": "old-stage-bounds", "test": "tests/native/test_probe_state.c",
         "source": "platforms/web/probe_state.inc", "compositor": False,
         "replace": [("<= WC_WIDTH - 10", "< 630"), ("<= WC_HEIGHT - 10", "< 470")]},
        {"id": "stale-scene", "test": "tests/native/test_web_metamorphic.c",
         "source": "platforms/web/compositor.c", "compositor": True,
         "replace": [("c->revision == d->visual_revision &&", "")]},
        {"id": "opaque-popup", "test": "tests/native/test_director_d85_blend.c",
         "source": "runtime/director/director.c", "compositor": False,
         "replace": [("s->auto_mask |= DG_BLEND;", "")]},
        {"id": "full-film-scan", "test": "tests/native/test_web_film_work.c",
         "source": "platforms/web/compositor.c", "compositor": True,
         "replace": [("if(!c->film_clip_disabled) {cl=tl;ct=tt;cr=tr;cb=tb;}",
                      "if(false) {cl=tl;ct=tt;cr=tr;cb=tb;}")]},
    ]
    results = []
    for case in cases:
        case_dir = output / case["id"]
        case_dir.mkdir(exist_ok=True)
        source = (root / case["source"]).read_text()
        changed = source
        for before, after in case["replace"]:
            if before not in changed:
                raise ValueError(f"mutation no longer applies: {case['id']}")
            changed = changed.replace(before, after)
        runs = {}
        for kind, content in (("control", source), ("mutant", changed)):
            tree = case_dir / kind
            for name in (case["source"], case["test"]):
                (tree / name).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(root / name, tree / name)
            (tree / case["source"]).write_text(content)
            binary = tree / "contract"
            director = (tree if case["source"] == "runtime/director/director.c" else root)
            sources = [tree / case["test"], director / "runtime/director/director.c",
                       root / "runtime/lingo/lingo_runtime.c"]
            if case["compositor"]:
                sources += [tree / case["source"], root / "runtime/director/shape.c"]
            subprocess.run([os.environ.get("CC", "cc"), *flags, *map(str, sources),
                            "-lm", "-o", str(binary)], cwd=root, check=True)
            with (tree / "result.log").open("w") as log:
                result = subprocess.run([binary], stdout=log, stderr=log, timeout=60, check=False)
            runs[kind] = result.returncode
        result = {"id": case["id"], "control_exit": runs["control"], "mutant_exit": runs["mutant"],
                  "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
                  "passing": runs["control"] == 0 and runs["mutant"] != 0}
        results.append(result)
        print(json.dumps(result), flush=True)
    report = {"schema_version": 1, "status": "passing" if all(r["passing"] for r in results)
              else "failed", "results": results}
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    return int(report["status"] != "passing")


if __name__ == "__main__":
    raise SystemExit(main())

"""Build and run the local-disc, input-only full-game native integration suite."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

from director64.project import work_dir


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitizers", action="store_true")
    parser.add_argument("--soak", action="store_true")
    parser.add_argument("--build-only", action="store_true", help="build the native input probe")
    args = parser.parse_args()
    root = Path.cwd()
    suffix = "-sanitized" if args.sanitizers else ""
    output = root / f"{work_dir()}/director/host{suffix}"
    output.mkdir(parents=True, exist_ok=True)
    executable = root / f"{work_dir()}/native/director-probe{suffix}"
    executable.parent.mkdir(parents=True, exist_ok=True)
    source = [
        "games/findus-workshop/tests/director_probe.c",
        "platforms/native/image.c",
        "runtime/lingo/lingo_runtime.c",
        "runtime/director/director.c",
        "runtime/interaction/input.c",
        "runtime/interaction/pointer.c",
        "games/findus-workshop/runtime/virtual_files.c",
        "games/findus-workshop/runtime/archive.c",
        "games/findus-workshop/runtime/save.c",
    ]
    source += [
        str(p)
        for pattern in (f"{work_dir()}/aot/c/*.c", f"{work_dir()}/director/c/*_scene.c")
        for p in sorted(root.glob(pattern))
    ]
    source += [f"{work_dir()}/director/c/registry.c"]
    if not (root / f"{work_dir()}/aot/c/manifest.json").exists():
        raise SystemExit(
            "run director64 assets --game findus-workshop first (requires the supported local ISO)"
        )
    compiler = os.environ.get("CC", "clang" if args.sanitizers else "cc")
    command = [
        compiler,
        "-std=c17",
        "-O1",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pedantic",
        *[
            "-I" + p
            for p in (
                "runtime/director",
                "runtime/lingo",
                "runtime/interaction",
                "runtime/storage",
                "platforms/native",
                "games/findus-workshop/runtime",
            )
        ],
        f"-I{work_dir()}/aot/c",
    ]
    if args.sanitizers:
        command += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    subprocess.run([*command, *source, "-lm", "-o", str(executable)], check=True)
    if args.build_only:
        print(executable)
        return
    env = os.environ | {
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=1",
    }
    reports = []
    cases = [
        (mode, "START")
        for mode in (
            "activities",
            "construction",
            "routes",
            "hubs",
            "profiles",
            "treasure",
            "exit",
            "garden",
        )
    ]
    cases.append(("credits", "HALLTYST"))
    if args.soak:
        cases += [
            ("fuzz", name)
            for name in (
                "PINTRO",
                "SNIKBOD",
                "VEMORY",
                "FINNDUNK",
                "FOTING",
                "BRODER",
                "HYVEL",
                "VERKORK",
                "MALAR",
                "SAGOR",
                "BREDHOGN",
                "MOSSEN",
                "PLOCKSPL",
                "LUFFSPEL",
            )
        ]
    for mode, movie in cases:
        path = output / f"{mode}-{movie}.json"
        subprocess.run(
            [
                sys.executable,
                "games/findus-workshop/tests/full_probe.py",
                mode,
                movie,
                "--executable",
                str(executable),
                "--output",
                str(path),
            ],
            check=True,
            env=env,
        )
        reports += json.loads(path.read_text())
    inputs = source + [
        "runtime/director/director.h",
        "runtime/lingo/lingo_runtime.h",
        "runtime/interaction/input.h",
        "runtime/interaction/pointer.h",
        "games/findus-workshop/tests/full_probe.py",
        f"{work_dir()}/director/model.json",
        f"{work_dir()}/aot/c/manifest.json",
    ]
    manifest = {
        "status": "passing",
        "platform": "host-native",
        "sanitizers": args.sanitizers,
        "scenarios": len(reports),
        "steps": sum(len(r["commands"]) for r in reports),
        "hardware_qualified": False,
        "source_sha256": {
            str(Path(p).relative_to(root) if Path(p).is_absolute() else p): hashlib.sha256(
                Path(p).read_bytes()
            ).hexdigest()
            for p in inputs
        },
    }
    (output / "validation.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({k: v for k, v in manifest.items() if k != "source_sha256"}))


if __name__ == "__main__":
    main()

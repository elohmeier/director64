"""Build and execute the recovered D8 startup with native runtime checks."""

import argparse
import hashlib
import json
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor

from director64.project import selected_game


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitizers", action="store_true")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--journey", action="store_true")
    parser.add_argument("--activity", action="store_true")
    parser.add_argument("--regressions", action="store_true")
    parser.add_argument("--ticks", type=int, default=18000)
    args = parser.parse_args()
    game = selected_game()
    executable = (
        game.work / "native" / ("director-probe" + ("-sanitized" if args.sanitizers else ""))
    )
    executable.parent.mkdir(parents=True, exist_ok=True)
    source = [game.directory / "tests/director_probe.c"]
    source += [
        game.root / p
        for p in (
            "platforms/native/image.c",
            "runtime/lingo/lingo_runtime.c",
            "runtime/director/director.c",
            "runtime/interaction/input.c",
            "runtime/interaction/pointer.c",
        )
    ]
    source += sorted(
        p for p in (game.directory / "runtime").glob("*.c") if p.name != "director_replay.c"
    )
    source += sorted((game.work / "aot/c").glob("*.c"))
    source += sorted((game.work / "director/c").glob("*.c"))
    command = [
        os.environ.get("CC", "clang" if args.sanitizers else "cc"),
        "-std=c17",
        "-DDIRECTOR64_POINTER_GENERATED=1",
        "-O1",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pedantic",
        "-DDIRECTOR64_DIRECTOR_VERSION=8",
    ]
    for directory in (
        "runtime",
        "runtime/director",
        "runtime/lingo",
        "runtime/interaction",
        "runtime/storage",
        "platforms/native",
        "games/findus-mucklas/runtime",
        "games/findus-mucklas/probe",
    ):
        command += ["-I" + directory]
    command += ["-I" + str(game.work / "aot/c")]
    if args.sanitizers:
        command += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    headers = [
        p
        for base in (
            game.root / "runtime",
            game.root / "platforms/native",
            game.directory / "runtime",
            game.directory / "probe",
            game.work / "aot/c",
            game.work / "director/c",
        )
        for p in sorted(base.rglob("*"))
        if p.suffix in {".h", ".inc"}
    ]
    identity = hashlib.sha256(
        json.dumps(command).encode()
        + subprocess.check_output([command[0], "--version"])
        + b"".join(p.read_bytes() for p in headers)
    ).digest()
    objects = executable.parent / (executable.name + "-objects")
    objects.mkdir(exist_ok=True)

    def compile_unit(path):
        digest = hashlib.sha256(identity + str(path).encode() + path.read_bytes()).hexdigest()
        obj = objects / (digest + ".o")
        if not obj.exists():
            temporary = obj.with_suffix(".tmp.o")
            subprocess.run([*command, "-c", str(path), "-o", str(temporary)], check=True)
            temporary.rename(obj)
        return str(obj)

    with ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 1)) as workers:
        compiled = list(workers.map(compile_unit, source))
    subprocess.run([*command, *compiled, "-lm", "-o", str(executable)], check=True)
    if args.build_only:
        print(executable)
        return
    if args.regressions:
        from .full_regressions import run

        output = executable.parent / "regressions"
        report = run(executable, output)
        report["sanitizers"] = args.sanitizers
        (output / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report))
        if report["status"] != "passing":
            raise ValueError(f"activity regression failed; inspect {output}")
        return
    if args.journey or args.activity:
        from .full_journey import run

        output = executable.parent / "activity" if args.activity else executable.parent
        report = run(executable, output, args.activity)
        report["sanitizers"] = args.sanitizers
        report["executable_sha256"] = hashlib.sha256(executable.read_bytes()).hexdigest()
        (output / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report))
        return
    result = subprocess.run(
        [str(executable), "START", str(args.ticks)],
        text=True,
        capture_output=True,
        env=os.environ
        | {
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
            "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=1",
        },
    )
    report = executable.parent / "startup.json"
    report.write_text(result.stdout)
    (executable.parent / "startup.log").write_text(result.stderr)
    result.check_returncode()
    state = json.loads(result.stdout)
    if state["error"] or state["quit"] or state["movie"] not in {"POFINTRO.DXR", "LO.DXR"}:
        raise ValueError(f"startup did not reach the original introduction/player chooser: {state}")
    print(
        json.dumps(
            {
                "status": "passing",
                "platform": "host-native",
                "sanitizers": args.sanitizers,
                "movie": state["movie"],
                "ticks": state["tick"],
                "hardware_qualified": False,
            }
        )
    )

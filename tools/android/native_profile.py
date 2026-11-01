#!/usr/bin/env python3
"""Sample one game's native CPU time on Android with simpleperf.

Installs the current production APK (unless --no-install), restarts the game,
waits until the pacing log reports MOVIE, then records SECONDS of CPU-clock
samples. The unstripped library kept beside the build symbolizes them; the
report ranks functions and source lines within the game library, next to the
pacing windows of the same interval.

    uv run --locked python tools/android/native_profile.py --game findus-ghosts \\
      --emulator --movie LOGIN.DXR --output build/profiling/ghosts-emulator

Emulator samples rank hot paths; they do not predict projector timings.
simpleperf, its host reporter and llvm-symbolizer come from the Android
toolchain image (docker/android) and are copied once under build/tools/.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

from director64 import android
from director64.android_pacing import FIELDS, MARKER
from director64.android_validation import Adb
from director64.project import GameSpec

TOOLS = ("simpleperf/bin/android/arm/simpleperf", "simpleperf/bin/android/arm64/simpleperf",
         "simpleperf/bin/android/x86_64/simpleperf", "simpleperf/bin/linux/x86_64/simpleperf",
         "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-symbolizer")
DEVICE_ABI = {"armeabi-v7a": "arm", "arm64-v8a": "arm64", "x86_64": "x86_64"}
# "share%  dso  symbol"; unsymbolized symbols read "libname.so[+offset]".
SAMPLE = re.compile(r"^\s*([0-9.]+)%\s+(\S+)\s+(.*?)\s*$")
OFFSET = re.compile(r"\[\+([0-9a-f]+)\]$")


def local_name(name: str) -> str:
    """simpleperf/bin/android/arm/simpleperf -> android-arm/simpleperf."""
    parts = Path(name).parts
    if parts[0] == "simpleperf":
        return f"{parts[2]}-{parts[3]}/simpleperf"
    return parts[-1]


def tools(root: Path) -> Path:
    """The NDK's profiling binaries, copied out of the toolchain image once."""
    folder = root / "build/tools/android-profiling"
    if all((folder / local_name(name)).is_file() for name in TOOLS):
        return folder
    folder.mkdir(parents=True, exist_ok=True)
    script = " && ".join(
        f'mkdir -p "/out/{Path(local_name(name)).parent}" && '
        f'cp "$ANDROID_NDK/{name}" "/out/{local_name(name)}"' for name in TOOLS)
    subprocess.run([os.environ.get("DOCKER", "docker"), "run", "--rm", "--network=none",
                    "--user", f"{os.getuid()}:{os.getgid()}", "-v", f"{folder}:/out",
                    android.IMAGE, "sh", "-c", script], check=True)
    return folder


def pacing(text: str) -> list[dict]:
    windows = []
    for line in text.splitlines():
        if MARKER in line:
            windows.append(dict(FIELDS.findall(line.split(MARKER, 1)[1])))
    return windows


def wait_for_movie(adb: Adb, movie: str, timeout: float) -> None:
    limit = time.monotonic() + timeout
    while time.monotonic() < limit:
        log = adb.run("logcat", "-d", "-s", "director64").stdout
        windows = pacing(log)
        if windows and windows[-1].get("movie") == movie:
            return
        if "FATAL EXCEPTION" in log:
            break
        time.sleep(2)
    raise ValueError(f"the pacing log did not reach {movie} within {timeout:.0f} s")


def symbolize(symbolizer: Path, library: Path, offsets: list[str]) -> list[tuple[str, str]]:
    """Function and file:line per offset; simpleperf prints ELF virtual addresses."""
    result = subprocess.run(
        [str(symbolizer), f"--obj={library}", "--inlining=false", "--output-style=GNU"],
        input="\n".join("0x" + o for o in offsets) + "\n",
        capture_output=True, text=True, check=True)
    lines = result.stdout.splitlines()
    return [(lines[i], lines[i + 1].split(" ")[0]) for i in range(0, len(lines) - 1, 2)]


def report(perf: Path, library: Path, folder: Path) -> dict:
    output = subprocess.run([str(folder / "linux-x86_64/simpleperf"), "report", "-i", str(perf),
                             "--sort", "dso,symbol"], capture_output=True, text=True,
                            check=True).stdout
    total = collections.Counter()
    offsets: dict[str, float] = {}
    name = library.name
    for line in output.splitlines():
        match = SAMPLE.match(line)
        if not match:
            continue
        share, dso, symbol = float(match.group(1)), match.group(2), match.group(3)
        total[dso.rsplit("/", 1)[-1]] += share
        offset = OFFSET.search(symbol)
        if dso.endswith(name) and offset:
            offsets[offset.group(1)] = offsets.get(offset.group(1), 0) + share
    functions, lines = collections.Counter(), collections.Counter()
    symbols = symbolize(folder / "llvm-symbolizer", library, list(offsets))
    for share, (function, where) in zip(offsets.values(), symbols, strict=True):
        functions[function] += share
        lines[where.removeprefix("/workdir/")] += share

    def top(counter: collections.Counter, n: int) -> list:
        return [[k, round(v, 2)] for k, v in counter.most_common(n)]

    return {"shared_objects": top(total, 12), "functions": top(functions, 30),
            "lines": top(lines, 40)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", required=True)
    parser.add_argument("--source")
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--emulator", action="store_true")
    target.add_argument("--serial")
    parser.add_argument("--movie", required=True, help="start sampling once this movie plays")
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--settle", type=float, default=5,
                        help="seconds to wait after the movie appears")
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--no-install", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    game = GameSpec.load(args.game, args.source)
    output = args.output.resolve()
    if not output.is_relative_to(game.root / "build"):
        parser.error("profiles stay under ignored build/")
    output.mkdir(parents=True, exist_ok=False)
    folder = tools(game.root)
    adb = Adb(game.root, emulator=args.emulator, serial=args.serial)
    package = android.package_name(game)
    abi = adb.shell("getprop", "ro.product.cpu.abi").stdout.strip()
    apk = android.apk_path(game)
    if not args.no_install:
        android.verify_apk(game)
        target_apk = (f"/workdir/{apk.relative_to(game.root).as_posix()}"
                      if args.emulator else str(apk))
        adb.run("install", "-r", target_apk, timeout=600)
    # The process's library ABI decides which unstripped library matches.
    flags = adb.shell("pm", "dump", package).stdout
    found = re.search(r"primaryCpuAbi=(\S+)", flags)
    library_abi = found.group(1) if found and found.group(1) != "null" else abi
    library = (game.root / "build/android" / game.slug / game.source["id"] / "symbols" /
               library_abi / f"lib{android.library(game)}.so")
    if not library.is_file():
        raise ValueError(f"no unstripped {library_abi} library; rebuild the APK: {library}")
    device_tool = folder / f"android-{DEVICE_ABI[abi]}" / "simpleperf"
    adb.run("push", f"/workdir/{device_tool.relative_to(game.root).as_posix()}"
            if args.emulator else str(device_tool), "/data/local/tmp/simpleperf")
    adb.shell("chmod", "755", "/data/local/tmp/simpleperf")
    adb.shell("am", "force-stop", package)
    adb.run("logcat", "-c")
    adb.shell("am", "start", "-W", "-n", package + "/org.director64.tv.GameActivity")
    wait_for_movie(adb, args.movie, args.timeout)
    time.sleep(args.settle)
    adb.run("logcat", "-c")
    adb.shell("/data/local/tmp/simpleperf", "record", "--app", package, "-e", "cpu-clock",
              "-f", "2000", "--duration", str(args.seconds), "-o", "/data/local/tmp/d64.perf",
              timeout=args.seconds + 60)
    log = adb.run("logcat", "-d", "-s", "director64").stdout
    (output / "pacing.log").write_text(log)
    perf = output / "perf.data"
    adb.run("pull", "/data/local/tmp/d64.perf",
            f"/workdir/{perf.relative_to(game.root).as_posix()}" if args.emulator else str(perf))
    windows = [w for w in pacing(log) if w.get("movie") == args.movie]
    summary = {"game": game.slug, "source": game.source["id"], "abi": library_abi,
               "device": adb.identity(), "movie": args.movie, "seconds": args.seconds,
               "pacing": {key: round(sum(int(w[key]) for w in windows) / len(windows), 1)
                          for key in ("ticks", "renders", "service_us", "render_us",
                                      "present_us")} if windows else {},
               "render_us_per_redraw": round(
                   sum(int(w["render_us"]) for w in windows) /
                   max(1, sum(int(w["renders"]) for w in windows)), 1) if windows else None,
               **report(perf, library, folder)}
    (output / "profile.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps({k: summary[k] for k in ("pacing", "render_us_per_redraw")}))
    for function, share in summary["functions"][:12]:
        print(f"{share:6.2f}%  {function}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Build a source-free renderer ROM and check bounded emulator pixel readbacks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default="director64-toolchain:local")
    parser.add_argument("--emulator", default=str(Path.home() / ".cargo/bin/gopher64"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = root / "build/tests/n64-render-probe"
    output.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root / "tests/n64/render_probe.c", output / "main.c")
    (output / "globals.inc").write_text('"synthetic",\n')
    (output / "movie_names.inc").write_text('"synthetic_dxr",\n')
    (output / "movie_casts.inc").write_text(
        "static const unsigned movie_cast_first[] = {0};\n"
        "static const unsigned movie_cast_files[] = {0};\n"
        "static const unsigned char movie_shared_cast[] = {0};\n"
    )
    (output / "Makefile").write_text(r'''BUILD_DIR=objects
include $(N64_INST)/include/n64.mk
N64_CFLAGS += -std=gnu17 -Werror -DDIRECTOR64_N64=1 -DDIRECTOR64_DIRECTOR_VERSION=10 -DDIRECTOR64_EXTENDED_D6=1 -DDIRECTOR64_D85=1 -DDIRECTOR64_ENTRY_MOVIE=\"synthetic_dxr\" -I. -I/work/runtime -I/work/runtime/director -I/work/runtime/lingo -I/work/runtime/storage -I/work/runtime/interaction -I/work/platforms/n64
all: probe.z64
$(BUILD_DIR)/director.o: /work/runtime/director/director.c
	@mkdir -p $(dir $@)
	$(N64_CC) $(N64_CFLAGS) -MMD -MP -c $< -o $@
$(BUILD_DIR)/shape.o: /work/runtime/director/shape.c
	$(N64_CC) $(N64_CFLAGS) -MMD -MP -c $< -o $@
$(BUILD_DIR)/lingo.o: /work/runtime/lingo/lingo_runtime.c
	$(N64_CC) $(N64_CFLAGS) -MMD -MP -c $< -o $@
$(BUILD_DIR)/probe.elf: $(BUILD_DIR)/main.o $(BUILD_DIR)/director.o $(BUILD_DIR)/shape.o $(BUILD_DIR)/lingo.o
probe.z64: N64_ROM_TITLE="Synthetic RGBA probe"
-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all
''')
    # Qualification always rebuilds the small probe, including explicit runtime
    # rules. Dependency files additionally protect manual incremental builds.
    with (output / "build.log").open("w") as log:
        subprocess.run(
            ["docker", "run", "--rm", "--network=none", "-v", f"{root}:/work", "-w",
             "/work/build/tests/n64-render-probe", args.image, "make", "-B", "-j4"],
            stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180,
        )
    emulator = Path(args.emulator).resolve()
    rom = output / "probe.z64"
    env = os.environ.copy()
    for kind in ("CONFIG", "DATA", "CACHE"):
        directory = output / kind.lower()
        directory.mkdir(exist_ok=True)
        env[f"XDG_{kind}_HOME"] = str(directory)
    command = [str(emulator), str(rom), "--overclock", "false",
               "--disable-expansion-pak", "false", "--capture-output", str(output / "run.mp4"),
               "--capture-start-marker", "DIRECTOR64 FILM_DONE", "--capture-start-timeout", "20",
               "--capture-duration", "1", "--capture-wall-timeout", "30"]
    started = time.monotonic()
    log_path = output / "emulator.log"
    with log_path.open("w") as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            while process.poll() is None and time.monotonic() - started < 35:
                if "DIRECTOR64 FILM_DONE" in log_path.read_text(errors="replace"):
                    break
                time.sleep(0.2)
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    observations = [line for line in log_path.read_text(errors="replace").splitlines()
                    if "DIRECTOR64 FILM_" in line]
    passed = "DIRECTOR64 FILM_DONE failed=0" in observations
    result = {"passed": passed, "seconds": round(time.monotonic() - started, 2),
              "rom_sha256": hashlib.sha256(rom.read_bytes()).hexdigest(),
              "emulator_sha256": hashlib.sha256(emulator.read_bytes()).hexdigest(),
              "emulator": subprocess.check_output([str(emulator), "--version"], text=True).strip(),
              "observations": observations}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("\n".join(observations), flush=True)
    raise SystemExit(0 if passed else "N64 pixel probe failed; inspect ignored build/tests/n64-render-probe logs")


if __name__ == "__main__":
    main()

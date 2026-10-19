"""Lingo source run end to end: parsed, compiled into a game package by the
converter, loaded by the runtime's package loader and driven by a C harness."""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

import pytest

from director64.lingo import parse_movie
from director64.project import converter, repository

# What a harness's `body` sees: `fixture`, the movie's code; `global_names`
# and `global_count`, the program's globals; and the runtime's headers.
PRELUDE = r"""
#include "package.h"
#include "lingo_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const lv_movie_t *fixture;
static const char *const *global_names;
static unsigned global_count;
static void load_fixture(const char *path) {
  static dg_package_t package;
  FILE *file = fopen(path, "rb");
  assert(file && !fseek(file, 0, SEEK_END));
  long length = ftell(file);
  assert(length > 32 && !fseek(file, 0, SEEK_SET));
  uint8_t *bytes = malloc((size_t)length);
  assert(bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length);
  fclose(file);
  char error[256];
  if (!dg_package_load(&package, bytes, (size_t)length, bytes + 12, error, sizeof(error))) {
    fprintf(stderr, "%s\n", error);
    exit(1);
  }
  lv_use_symbols(package.symbols);
  fixture = package.registry[0]->code;
  global_names = package.globals;
  global_count = package.global_count;
}
"""

FAMILY = {500: "5", 600: "6", 700: "7", 800: "8", 850: "10", 1000: "10"}


def run_lingo(tmp_path: Path, source: str, body: str, *, version: int = 600,
              extended: bool = False, movie: str = "FIXTURE.DXR", director: bool = False) -> None:
    """Compiles `source` as `movie` and runs `body`, C whose main() calls
    load_fixture(argv[1]); `director` links the score runtime too."""
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("native C compiler unavailable")
    root = repository()
    program = {"files": [{"name": f"{movie}.lingo"}], "handlers": parse_movie(source, movie)}
    model = {"problems": [], "extendedD6": extended, "movies": [{
        "id": 1, "name": movie, "directorVersion": version, "tempo": 15,
        "palette": [i * 0x010101 for i in range(256)],
        "casts": [{"number": 1, "name": "Internal", "file": movie}], "members": [],
        "score": {"frames": [{"channels": []}], "labels": []}}]}
    meta = {"director_version": version, "extended_d6": extended}
    for name, value in (("program.json", program), ("model.json", model), ("meta.json", meta)):
        (tmp_path / name).write_text(json.dumps(value))
    package = tmp_path / "game.d64p"
    subprocess.run([*converter(), "package", tmp_path / "program.json",
                    tmp_path / "model.json", package,
                    "--bytecode", root / "runtime/lingo/lingo_bytecode.h",
                    "--meta", tmp_path / "meta.json"], check=True, capture_output=True)
    harness = tmp_path / "harness.c"
    harness.write_text(PRELUDE + body)
    defines = [f"-DDIRECTOR64_DIRECTOR_VERSION={FAMILY[version]}",
               f"-DDIRECTOR64_EXTENDED_D6={int(extended)}"]
    if version == 850:
        defines.append("-DDIRECTOR64_D85=1")
    executable = tmp_path / "harness"
    subprocess.run([cc, "-std=c17", "-Wall", "-Wextra", "-Werror", "-pedantic", *defines,
                    *(f"-I{root / d}" for d in ("runtime/director", "runtime/lingo",
                                                "runtime/interaction", "runtime/package")),
                    harness, root / "runtime/package/package.c",
                    *([root / "runtime/director/director.c"] if director else []),
                    root / "runtime/lingo/lingo_runtime.c", "-lm", "-o", executable],
                   check=True)
    subprocess.run([executable, package], check=True)

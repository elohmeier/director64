"""The stored tile layout is a contract between three implementations.

The converter writes a wide image's planes in tile order, the runtime reads
them back through `bitmap_tile_index` without rearranging anything, and the
asset quantiser walks the same plane extent. Nothing records the choice in
the file — all three derive it from the stored dimensions — so the only thing
keeping them together is agreement, and a disagreement would render every
image past the texture limit scrambled rather than fail. This holds the
packer to the runtime; compiler/tests/image_tiles.rs holds the converter.
"""

import shutil
import subprocess

import pytest

from director64.full_assets import plane_pixels

CASES = [(1100, 40), (40, 1100), (1786, 149), (1182, 127), (1025, 1025), (640, 480)]


def test_plane_extent_agrees_across_runtime_and_packer(tmp_path):
    compiler = shutil.which("cc")
    if not compiler:
        pytest.skip("native compiler unavailable")

    harness = tmp_path / "extent.c"
    rows = "".join(f"{{{w},{h}}}," for w, h in CASES)
    harness.write_text(
        '#include "bitmap_tiles.h"\n'
        "#include <stdio.h>\n"
        f"static const unsigned cases[][2] = {{{rows}}};\n"
        "int main(void) {\n"
        "  for (unsigned i = 0; i < sizeof(cases)/sizeof(*cases); i++)\n"
        '    printf("%llu\\n", (unsigned long long)'
        "bitmap_plane_pixels(cases[i][0], cases[i][1]));\n"
        "}\n"
    )
    executable = tmp_path / "extent"
    subprocess.run(
        [compiler, "-std=c17", "-Wall", "-Wextra", "-Werror", "-pedantic",
         "-Iruntime/director", str(harness), "-o", str(executable)],
        check=True,
    )
    runtime = subprocess.run(
        [str(executable)], capture_output=True, text=True, check=True
    ).stdout.split()

    for (width, height), pixels in zip(CASES, runtime, strict=True):
        assert plane_pixels(width, height) == int(pixels), (width, height)

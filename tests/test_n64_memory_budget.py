"""Impossible hosted memory pools must fail before making a console ROM."""
import os
import subprocess
from pathlib import Path


def test_hosted_d85_pool_is_rejected_for_console(tmp_path):
    root = Path(__file__).resolve().parents[1]
    source = tmp_path / "memory.c"
    source.write_text('#include "director.h"\n#include "memory_budget.h"\n')
    result = subprocess.run([
        os.environ.get("CC", "cc"), "-std=c17", "-fsyntax-only",
        "-DDIRECTOR64_DIRECTOR_VERSION=10", "-DDIRECTOR64_EXTENDED_D6=1",
        "-DDIRECTOR64_D85=1", "-DDIRECTOR64_N64=1",
        "-DLV_HEAP_BYTES=4194304", "-DLV_OBJECTS=32768",
        "-Iruntime/director", "-Iruntime/lingo", "-Iruntime/interaction",
        "-Iruntime/storage", "-Iplatforms/n64", str(source),
    ], cwd=root, text=True, capture_output=True)
    assert result.returncode != 0
    assert "N64 runtime pools leave insufficient RDRAM" in result.stderr

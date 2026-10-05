"""Report build prerequisites without installing tools or modifying dependencies."""

from __future__ import annotations

import os
import platform
import shlex
import shutil
import subprocess
import tomllib
from pathlib import Path

from .toolchain import RECIPE_LABEL, container_engine, inspect_image, source_revision


def diagnose(root: Path) -> list[dict]:
    results = []

    def report(name, ok, detail, *, required=True):
        results.append({"name": name, "ok": ok, "detail": detail, "required": required})

    expected = tomllib.loads((root / "mise.toml").read_text())["tools"]["python"]
    actual = platform.python_version()
    report("Python", actual == expected, f"{actual}; mise requires {expected}")
    commands = [
        ("Git", "git", True),
        ("uv", "uv", True),
        ("Node", "node", True),
        ("npm", "npm", True),
        ("C compiler", shlex.split(os.environ.get("CC", "cc"))[0], True),
        ("C++ compiler", shlex.split(os.environ.get("CXX", "c++"))[0], True),
        ("Make", "make", True),
        ("container engine", container_engine(), True),
        ("CMake (optional host tools)", "cmake", False),
        ("FFmpeg (video conversion/capture)", "ffmpeg", False),
        ("FFprobe (video conversion/capture)", "ffprobe", False),
        ("Gopher64 (emulator capture)", "gopher64", False),
        ("Ares (additional emulator validation)", "ares", False),
    ]
    for name, command, required in commands:
        path = shutil.which(command)
        report(name, path is not None, path or f"missing executable: {command}", required=required)
    try:
        report("libdragon", True, source_revision(root))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        report("libdragon", False, str(error))
        return results
    try:
        image = inspect_image(
            root, os.environ.get("DIRECTOR64_TOOLCHAIN_IMAGE", "director64-toolchain:local")
        )
        report("toolchain image", True, image["Id"])
        report("toolchain recipe", True, image["Config"]["Labels"][RECIPE_LABEL])
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        report("toolchain image", False, f"{error}; build with: mise run toolchain")
    return results


def main(root: Path) -> int:
    results = diagnose(root)
    for item in results:
        status = "ok" if item["ok"] else "missing" if item["required"] else "optional"
        print(f"{status:8} {item['name']}: {item['detail']}")
    return int(any(item["required"] and not item["ok"] for item in results))

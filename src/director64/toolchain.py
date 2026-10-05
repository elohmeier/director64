"""Build and verify the pinned SDK, compiler base, recipe and container platform."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tarfile
import tempfile
import tomllib
from contextlib import contextmanager
from pathlib import Path

SDK_PATH = "third_party/libdragon"
RECIPE_LABEL = "org.director64.toolchain-recipe"
PLATFORM_LABEL = "org.director64.toolchain-platform"


class ToolchainError(ValueError):
    pass


def container_engine() -> str:
    """DOCKER selects one executable, including a Docker-compatible wrapper."""
    return os.environ.get("DOCKER") or "docker"


def source_revision(root: Path) -> str:
    root = root.resolve()
    sdk = root / SDK_PATH
    if os.environ.get("LIBDRAGON_SOURCE"):
        raise ToolchainError("LIBDRAGON_SOURCE is unsupported; use vendored third_party/libdragon")
    if sdk.resolve() != sdk or not (sdk / ".git").exists():
        raise ToolchainError(
            "initialize the vendored SDK with: git submodule update --init --recursive"
        )

    def git(path, *args):
        return subprocess.check_output(["git", "-C", str(path), *args], text=True).strip()

    entry = git(root, "ls-files", "--stage", "--", SDK_PATH).split()
    if len(entry) != 4 or entry[0] != "160000" or entry[2] != "0":
        raise ToolchainError("third_party/libdragon must have one recorded submodule gitlink")
    revision = git(sdk, "rev-parse", "HEAD")
    config = tomllib.loads((root / "config/provenance.toml").read_text())
    if revision != entry[1] or revision != config["libdragon"]["commit"]:
        raise ToolchainError("vendored SDK revision differs from gitlink or config/provenance.toml")
    if git(sdk, "status", "--porcelain"):
        raise ToolchainError("vendored libdragon checkout must be clean")
    if any(line.startswith("160000 ") for line in git(sdk, "ls-tree", "-r", "HEAD").splitlines()):
        raise ToolchainError("nested SDK submodules require explicit archive staging support")
    return revision


def image_recipe(root: Path, revision: str) -> dict:
    dockerfile = (root / "docker/toolchain/Dockerfile").read_bytes()
    bases = re.findall(rb"(?m)^FROM\s+(\S+)\s*$", dockerfile)
    if len(bases) != 1 or not re.fullmatch(rb"\S+@sha256:[0-9a-f]{64}", bases[0]):
        raise ToolchainError("toolchain Dockerfile must use one digest-pinned compiler base")
    base = bases[0].decode()
    provenance = tomllib.loads((root / "config/provenance.toml").read_text())
    if base != provenance["libdragon"].get("container_base"):
        raise ToolchainError("compiler base differs from config/provenance.toml")
    platform = os.environ.get("DIRECTOR64_TOOLCHAIN_PLATFORM", "linux/amd64")
    if not re.fullmatch(r"linux/[a-z0-9_]+(?:/[a-z0-9_]+)?", platform):
        raise ToolchainError("DIRECTOR64_TOOLCHAIN_PLATFORM must select one Linux platform")
    return {
        "compiler_base": base,
        "platform": platform,
        "build_args": {"LIBDRAGON_COMMIT": revision, "TOOLCHAIN_PLATFORM": platform},
        "files": {
            name: hashlib.sha256((root / name).read_bytes()).hexdigest()
            for name in (
                "docker/toolchain/Dockerfile",
                ".mise-tasks/toolchain",
                "src/director64/toolchain.py",
            )
        },
    }


def recipe_fingerprint(recipe: dict) -> str:
    return hashlib.sha256(json.dumps(recipe, sort_keys=True).encode()).hexdigest()


def inspect_image(root: Path, image: str) -> dict:
    revision = source_revision(root)
    recipe = image_recipe(root, revision)
    result = json.loads(subprocess.check_output([container_engine(), "image", "inspect", image]))[0]
    labels = result.get("Config", {}).get("Labels") or {}
    if labels.get("org.director64.libdragon-commit") != revision:
        raise ToolchainError(
            "toolchain image differs from vendored libdragon; run: mise run toolchain"
        )
    if labels.get(RECIPE_LABEL) != recipe_fingerprint(recipe):
        raise ToolchainError("toolchain image differs from build recipe; run: mise run toolchain")
    platform = recipe["platform"].split("/")
    if (
        labels.get(PLATFORM_LABEL) != recipe["platform"]
        or [result.get("Os"), result.get("Architecture")] != platform[:2]
        or (len(platform) == 3 and result.get("Variant") != platform[2])
    ):
        raise ToolchainError(
            "toolchain image differs from selected platform; run: mise run toolchain"
        )
    return result


def build_image(root: Path, image: str) -> dict:
    root = root.resolve()
    revision = source_revision(root)
    recipe = image_recipe(root, revision)
    arguments = {**recipe["build_args"], "TOOLCHAIN_RECIPE": recipe_fingerprint(recipe)}
    with tempfile.TemporaryDirectory(prefix="director64-toolchain-") as directory:
        staging = Path(directory)
        archive = staging / "sdk.tar"
        subprocess.run(
            ["git", "-C", str(root / SDK_PATH), "archive", "--format=tar",
             f"--output={archive}", revision],
            check=True,
        )
        with tarfile.open(archive) as source:
            source.extractall(staging / "libdragon-src", filter="data")
        archive.unlink()
        shutil.copyfile(root / "docker/toolchain/Dockerfile", staging / "Dockerfile")
        print(f"Building {image} from libdragon {revision} ({recipe['platform']})", flush=True)
        subprocess.run(
            [
                container_engine(), "build", "--platform", recipe["platform"],
                *[
                    arg
                    for key, value in arguments.items()
                    for arg in ("--build-arg", f"{key}={value}")
                ],
                "--tag", image, str(staging),
            ],
            check=True,
        )
    return inspect_image(root, image)


@contextmanager
def selected_image(root: Path):
    """Keep every converter/validator in this operation on the inspected image ID."""
    previous = os.environ.get("DIRECTOR64_TOOLCHAIN_IMAGE")
    image = inspect_image(root, previous or "director64-toolchain:local")
    os.environ["DIRECTOR64_TOOLCHAIN_IMAGE"] = image["Id"]
    try:
        yield image
    finally:
        if previous is None:
            os.environ.pop("DIRECTOR64_TOOLCHAIN_IMAGE", None)
        else:
            os.environ["DIRECTOR64_TOOLCHAIN_IMAGE"] = previous


def record_build_context(work: Path, image: str, arguments: list[str]) -> None:
    """Make recompiles cached objects when the SDK image or build options change."""
    path = work / "n64/build-context.json"
    content = json.dumps({"image": image, "make": arguments}, sort_keys=True, indent=2) + "\n"
    if not path.exists() or path.read_text() != content:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--source-only", action="store_true")
    mode.add_argument("--build", action="store_true")
    parser.add_argument(
        "--image",
        default=os.environ.get("DIRECTOR64_TOOLCHAIN_IMAGE", "director64-toolchain:local"),
    )
    args = parser.parse_args()
    try:
        if args.source_only:
            result = source_revision(args.root)
        else:
            operation = build_image if args.build else inspect_image
            result = operation(args.root, args.image)["Id"]
        print(result)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()

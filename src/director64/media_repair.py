"""Make a separately pinned disc copy from audited, same-length file repairs.

Original images and cached extractions are never edited. A repair is an explicit
source candidate, with its own full image hash and a source-free recipe.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from .iso import Layout, file_sha256, layout_inventory


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_image(source: Path, output: Path, patches: list[tuple[int, bytes]], *,
                source_sha256: str, output_sha256: str) -> None:
    """Atomically replace only the listed ranges of a verified cooked image."""
    if source.resolve() == output.resolve():
        raise ValueError("a repair must use a separate output image")
    if source.is_symlink() or output.is_symlink():
        raise ValueError("repair image paths must not be symlinks")
    if file_sha256(source) != source_sha256:
        raise ValueError("original image SHA-256 differs from the repair recipe")
    size = source.stat().st_size
    end = 0
    for at, data in sorted(patches):
        if at < end or at < 0 or at > size or not data or len(data) > size - at:
            raise ValueError("overlapping or out-of-bounds image repair")
        end = at + len(data)
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=".media-repair-", dir=output.parent)
    os.close(descriptor)
    pending = Path(temporary)
    try:
        shutil.copyfile(source, pending)
        with pending.open("r+b") as handle:
            for at, data in patches:
                handle.seek(at)
                handle.write(data)
        if file_sha256(pending) != output_sha256:
            raise ValueError("repaired image SHA-256 differs from the pinned result")
        # Prevent replacing a valid result if the original changed during work.
        if file_sha256(source) != source_sha256:
            raise ValueError("original image changed during repair")
        pending.replace(output)
    finally:
        pending.unlink(missing_ok=True)


def validate_mpeg(root: Path, path: Path, expected: dict) -> dict:
    """Require the normal importer and strict decoder to accept the derivative."""
    aot = root / "compiler/target/release/director64-aot"
    if not aot.is_file():
        raise ValueError("build the compiler with mise run compiler before repairing media")
    parsed = subprocess.run([aot, "director", "mpeg-info", path], capture_output=True, text=True)
    if parsed.returncode:
        raise ValueError(f"{path.name}: package importer rejects repaired MPEG: {parsed.stderr}")
    info = json.loads(parsed.stdout)
    if any(info.get(key) != value for key, value in expected.items()):
        raise ValueError(f"{path.name}: repaired MPEG timeline/geometry differs: {info}")
    checked = subprocess.run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-xerror", "-err_detect", "explode",
        "-i", path, "-map", "0:v:0", "-map", "0:a:0?", "-f", "null", "-",
    ], capture_output=True)
    if checked.returncode or checked.stderr:
        raise ValueError(f"{path.name}: strict repaired MPEG decode failed: "
                         f"{checked.stderr.decode(errors='replace')[-4096:]}")
    return info


def prepare(game, replacements: list[Path]) -> Path:
    repairs = game.source.get("repairs", [])
    if len(repairs) != 1:
        raise ValueError(f"{game.slug}: expected one audited media repair recipe")
    spec = repairs[0]
    recipe_path = game.directory / spec["recipe"]
    recipe_bytes = recipe_path.read_bytes()
    recipe_sha256 = digest(recipe_bytes)
    recipe = json.loads(recipe_bytes)
    if recipe.get("schema_version") != 1 or recipe.get("source_sha256") != game.source["sha256"]:
        raise ValueError("media repair recipe does not match the selected source")
    if recipe.get("output") != {key: spec[key] for key in ("file", "sha256", "bytes")}:
        raise ValueError("media repair recipe and catalogued image disagree")
    directory = game.root / "media" / game.slug / game.source["id"]
    original = directory / game.source["file"]
    output = directory / spec["file"]
    if not original.is_file() or original.stat().st_size != game.source["bytes"]:
        raise ValueError(f"{original}: the original selected image is required")
    if file_sha256(original) != game.source["sha256"]:
        raise ValueError("original image differs from the selected source")
    if output.exists():
        if output.stat().st_size != spec["bytes"] or file_sha256(output) != spec["sha256"]:
            raise ValueError(f"{output}: existing repair was modified; remove it before rebuilding")
        try:
            receipt = json.loads(output.with_suffix(output.suffix + ".json").read_text())
            if (receipt["source_sha256"] == game.source["sha256"]
                    and receipt["output_sha256"] == spec["sha256"]
                    and receipt["recipe_sha256"] == recipe_sha256):
                return output
        except (OSError, ValueError, KeyError, TypeError):
            pass
    supplied = {}
    for path in replacements:
        if not path.is_file():
            raise ValueError(f"missing recovered media input: {path}")
        supplied[file_sha256(path)] = path
    with original.open("rb") as handle:
        layout, inventory = layout_inventory(handle)
    if layout != Layout():
        raise ValueError("this repair recipe requires the selected cooked ISO image")
    entries = {entry.path: entry for entry in inventory}
    work = game.root / "build/media-repair" / game.slug
    work.mkdir(parents=True, exist_ok=True)
    patches, records = [], []
    seen = set()
    with original.open("rb") as handle:
        for index, patch in enumerate(recipe["patches"]):
            name = patch["path"]
            if name in seen:
                raise ValueError(f"duplicate media repair: {name}")
            seen.add(name)
            entry = entries.get(name)
            if not entry or entry.length != patch["bytes"]:
                raise ValueError(f"{name}: file extent/length differs from repair recipe")
            handle.seek(entry.offset)
            before = handle.read(entry.length)
            if len(before) != entry.length or digest(before) != patch["source_sha256"]:
                raise ValueError(f"{name}: original file differs from repair recipe")
            method = patch["method"]
            details = {}
            if method == "recovered-copy":
                path = supplied.get(patch["replacement_sha256"])
                if path is None:
                    raise ValueError(f"{name}: supply --replacement PATH with SHA-256 "
                                     f"{patch['replacement_sha256']}")
                after = path.read_bytes()
                details = {"replacement_sha256": digest(after), "reconstructed": False}
            elif method == "hold-b-picture":
                from .mpeg_picture import HoldPicture, reconstruct_hold

                after, details = reconstruct_hold(
                    before, video_sha256=patch["video_sha256"],
                    picture=HoldPicture(**patch["picture"]),
                )
            elif method == "salvage":
                from .mpeg_salvage import reconstruct

                source = work / f"{index}-original.mpg"
                target = work / f"{index}-reconstructed.mpg"
                receipt = work / f"{index}-reconstructed.json"
                source.write_bytes(before)
                reconstruct(source, target, patch["recipe"], receipt)
                after = target.read_bytes()
                details = json.loads(receipt.read_text())
            else:
                raise ValueError(f"{name}: unsupported repair method {method}")
            if len(after) > entry.length:
                raise ValueError(f"{name}: repaired stream exceeds its original file extent")
            padded = entry.length - len(after)
            after += bytes(padded)
            if digest(after) != patch["output_sha256"]:
                raise ValueError(f"{name}: repaired file differs from the pinned result")
            target = work / f"{index}-validated.mpg"
            target.write_bytes(after)
            info = validate_mpeg(game.root, target, patch["expected_info"])
            patches.append((entry.offset, after))
            records.append({"path": name, "method": method, "offset": entry.offset,
                            "bytes": entry.length, "source_sha256": digest(before),
                            "output_sha256": digest(after), "zero_padding": padded,
                            "info": info, "details": details})
    if spec["bytes"] != original.stat().st_size:
        raise ValueError("same-extent repairs must retain the original image length")
    if file_sha256(recipe_path) != recipe_sha256:
        raise ValueError("media repair recipe changed during validation")
    write_image(original, output, patches, source_sha256=game.source["sha256"],
                output_sha256=spec["sha256"])
    report = {"schema_version": 1, "game": game.slug,
              "source_sha256": game.source["sha256"], "output_sha256": spec["sha256"],
              "recipe_sha256": recipe_sha256, "patches": records}
    receipt = json.dumps(report, indent=2) + "\n"
    (work / "receipt.json").write_text(receipt)
    output.with_suffix(output.suffix + ".json").write_text(receipt)
    return output


def main(game, argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--replacement", action="append", type=Path, default=[],
                        help="independently recovered source file (matched by full SHA-256)")
    args = parser.parse_args(argv)
    print(prepare(game, args.replacement))
    return 0

"""Verified ISO/ZIP source import, preserving roots and associated ISO streams."""

from __future__ import annotations

import hashlib
import shutil
import stat
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

from .iso import extract, file_sha256

MAX_EXPANDED = 4 * 1024**3
MAX_FILES = 100_000


def zip_entries(archive):
    entries = archive.infolist()
    if len(entries) > MAX_FILES or sum(i.file_size for i in entries) > MAX_EXPANDED:
        raise ValueError("ZIP exceeds import limits")
    paths = set()
    result = []
    for item in entries:
        name = item.filename
        path = PurePosixPath(name)
        if (
            not name
            or path.is_absolute()
            or any(p in {"..", "."} for p in name.split("/") if p)
            or "\\" in name
            or ":" in name
            or "\0" in name
            or "//" in name
            or stat.S_ISLNK(item.external_attr >> 16)
            or item.flag_bits & 1
        ):
            raise ValueError(f"unsafe/unsupported ZIP entry: {name}")
        key = str(path).casefold()
        if key in paths:
            raise ValueError(f"duplicate ZIP path: {name}")
        paths.add(key)
        if not item.is_dir():
            result.append((item, path))
    return result


def extract_zip(source: Path, output: Path, expected_sha256: str):
    if file_sha256(source) != expected_sha256:
        raise ValueError("source SHA-256 differs from selected manifest")
    if any(p.is_symlink() for p in (output, *output.parents)):
        raise ValueError("symlink in extraction path")
    with zipfile.ZipFile(source) as archive:
        entries = zip_entries(archive)
        if output.exists():
            expected = {str(path) for _, path in entries}
            actual = {str(p.relative_to(output)) for p in output.rglob("*") if p.is_file()}
            if actual != expected:
                raise ValueError("cached ZIP extraction has missing or extra files")
            for info, path in entries:
                target = output / path
                if any(p.is_symlink() for p in (target, *target.parents)):
                    raise ValueError("symlink in cached extraction")
                with archive.open(info) as handle:
                    digest = hashlib.file_digest(handle, "sha256").hexdigest()
                if file_sha256(target) != digest:
                    raise ValueError("cached ZIP extraction differs from source")
            return output
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="import-", dir=output.parent) as directory:
            pending = Path(directory) / "extracted"
            pending.mkdir()
            for info, path in entries:
                target = pending / path
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(info) as source_handle, target.open("xb") as target_handle:
                    shutil.copyfileobj(source_handle, target_handle, 1024 * 1024)
            pending.rename(output)
    return output


def extract_source(game):
    output = game.work / "extracted"
    if game.source["kind"] == "iso9660":
        extract(game.media, output, expected_sha256=game.source["sha256"])
    elif game.source["kind"] == "zip":
        extract_zip(game.media, output, game.source["sha256"])
    else:
        raise ValueError("unsupported source container")
    return output

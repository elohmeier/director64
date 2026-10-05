"""Read the supported disc's ISO 9660 data without mounting or modifying it."""

from __future__ import annotations

import hashlib
import json
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO

SECTOR = 2048


class IsoError(ValueError):
    """Unsupported, damaged, or unsafe disc/extraction."""


@dataclass(frozen=True)
class Entry:
    path: str
    offset: int
    length: int


def file_sha256(path: Path) -> str:
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def verify_iso(path: Path, expected_sha256: str) -> str:
    digest = file_sha256(path)
    if digest != expected_sha256:
        raise IsoError(f"unsupported ISO SHA-256: {digest}; expected {expected_sha256}")
    return digest


def inventory(handle: BinaryIO) -> list[Entry]:
    """Bounded, single-extent ISO 9660 files (no HFS or Rock Ridge writes),
    read by the Rust converter's reader (compiler/src/iso.rs), which the
    browser importer runs as well."""
    from .aot import generator_command, repository_root

    name = getattr(handle, "name", None)
    with tempfile.TemporaryDirectory(prefix="director64-iso-") as scratch:
        if isinstance(name, str) and Path(name).is_file():
            path = Path(name)
        else:
            path = Path(scratch) / "image.iso"
            handle.seek(0)
            path.write_bytes(handle.read())
        completed = subprocess.run(
            [generator_command(repository_root())[0], "iso-inventory", path],
            capture_output=True,
            text=True,
            check=False,
        )
    if completed.returncode:
        raise IsoError(completed.stderr.strip().removeprefix("director64-aot: "))
    return [Entry(p, offset, length) for p, offset, length in json.loads(completed.stdout)]


def extract(iso: Path, output: Path, *, expected_sha256: str) -> list[Entry]:
    """Verify every cached file against the disc; never accept a two-file cache hit."""
    verify_iso(iso, expected_sha256)
    if output.is_symlink():
        raise IsoError(f"extraction root is a symlink: {output}")
    with iso.open("rb") as handle:
        entries = inventory(handle)
        if output.exists():
            expected = {entry.path for entry in entries}
            actual = {str(p.relative_to(output)) for p in output.rglob("*") if p.is_file()}
            if actual != expected:
                raise IsoError("cached extraction has missing or extra files")
            for entry in entries:
                destination = output / entry.path
                if any(part.is_symlink() for part in (destination, *destination.parents)):
                    raise IsoError(f"symlink in extraction: {entry.path}")
                handle.seek(entry.offset)
                digest = hashlib.sha256(handle.read(entry.length)).hexdigest()
                if not destination.is_file() or file_sha256(destination) != digest:
                    raise IsoError(
                        f"cached extraction differs from ISO: {entry.path}; "
                        "use a fresh --output directory"
                    )
            return entries
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="director64-iso-", dir=output.parent) as staging:
            root = Path(staging) / "iso9660"
            root.mkdir()
            for entry in entries:
                destination = root / entry.path
                destination.parent.mkdir(parents=True, exist_ok=True)
                handle.seek(entry.offset)
                destination.write_bytes(handle.read(entry.length))
            root.rename(output)
    return entries

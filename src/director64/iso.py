"""Read the supported disc's ISO 9660 data without mounting or modifying it.

The image is cooked (2048-byte sectors, `.iso`) or a raw rip (2352-byte
sectors, a CloneCD `.img` or a `.bin` of a `.cue`); offsets are logical and
`Layout` places them in the image (compiler/src/iso.rs)."""

from __future__ import annotations

import hashlib
import json
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO

SECTOR = 2048
RAW_SECTOR = 2352
SYNC = b"\x00" + b"\xff" * 10 + b"\x00"  # a raw sector's first 12 bytes


class IsoError(ValueError):
    """Unsupported, damaged, or unsafe disc/extraction."""


@dataclass(frozen=True)
class Entry:
    path: str
    offset: int
    length: int


@dataclass(frozen=True)
class Layout:
    """Logical sector n's user data sits at n * stride + data."""

    stride: int = SECTOR
    data: int = 0

    def read(self, handle: BinaryIO, offset: int, length: int) -> bytes:
        """A file's bytes: each sector's user data as it is, since protected
        discs carry damaged sectors in decoy files (the Rust reader has
        checked the descriptors and directories). Past the image's end, as
        an overstated volume's decoys run, it reads as zeros."""
        if self.stride == SECTOR:
            handle.seek(offset)
            data = handle.read(length)
        else:
            first, end = offset // SECTOR, offset + length
            last = max(first, (end - 1) // SECTOR)
            handle.seek(first * self.stride)
            raw = handle.read((last - first + 1) * self.stride)
            frames = range(0, len(raw) - self.stride + 1, self.stride)
            start = offset - first * SECTOR
            data = b"".join(raw[i + self.data : i + self.data + SECTOR] for i in frames)
            data = data[start : start + length]
        return data.ljust(length, b"\0")


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
    return layout_inventory(handle)[1]


def layout_inventory(handle: BinaryIO) -> tuple[Layout, list[Entry]]:
    """The image's sector layout and its files at logical offsets."""
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
    result = json.loads(completed.stdout)
    entries = [Entry(p, offset, length) for p, offset, length in result["files"]]
    return Layout(*result["layout"]), entries


def extract(iso: Path, output: Path, *, expected_sha256: str) -> list[Entry]:
    """Verify every cached file against the disc; never accept a two-file cache hit."""
    verify_iso(iso, expected_sha256)
    if output.is_symlink():
        raise IsoError(f"extraction root is a symlink: {output}")
    with iso.open("rb") as handle:
        layout, entries = layout_inventory(handle)
        if output.exists():
            expected = {entry.path for entry in entries}
            actual = {str(p.relative_to(output)) for p in output.rglob("*") if p.is_file()}
            if actual != expected:
                raise IsoError("cached extraction has missing or extra files")
            for entry in entries:
                destination = output / entry.path
                if any(part.is_symlink() for part in (destination, *destination.parents)):
                    raise IsoError(f"symlink in extraction: {entry.path}")
                digest = hashlib.sha256(layout.read(handle, entry.offset, entry.length)).hexdigest()
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
                destination.write_bytes(layout.read(handle, entry.offset, entry.length))
            root.rename(output)
    return entries

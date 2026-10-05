"""Validate an existing ROM with the same pinned SDK that built it."""

from __future__ import annotations

import configparser
import hashlib
import io
import re
import struct
import zipfile
import zlib
from datetime import date
from pathlib import PurePosixPath

ROM_LIMIT = 64 * 1024 * 1024
# 56 MiB is not a preference: it is where the cartridge stops holding what was
# written there. The SDK's USB debug channel keeps its scratch area at
# `0x04000000 - DEBUG_ADDRESS_SIZE` (usb.c), and DEBUG_ADDRESS_SIZE is 8 MiB,
# so every debug line the ROM prints is written to cart offset 0x3800000 with
# SDRAM writes enabled. A SummerCart64 honours that, and so does the emulator;
# whatever the build packed from 56 MiB upward is overwritten while the game
# runs. The damage is a few hundred bytes and lands wherever the DragonFS
# layout happens to put a file, which is why a ROM over the ceiling can look
# fine for a long time: Loewenzahn's only showed up when it fell inside an
# H.264 stream and the decoder rejected the slice.
ROM_TARGET_MIB = 56
ROM_TARGET = ROM_TARGET_MIB * 1024 * 1024


class RomError(ValueError):
    """A ROM cannot satisfy its build contract."""


def validate_size(size: int, *, release_budget_mib: int = ROM_TARGET_MIB) -> None:
    """Apply a game's release budget without relaxing cartridge capacity."""
    if size > ROM_LIMIT:
        raise RomError("ROM exceeds hard 64 MiB capacity")
    if type(release_budget_mib) is not int or not 1 <= release_budget_mib <= ROM_TARGET_MIB:
        raise RomError(f"ROM release budget must be an integer from 1 to {ROM_TARGET_MIB} MiB")
    if size > release_budget_mib * 1024 * 1024:
        raise RomError(f"ROM exceeds release {release_budget_mib} MiB budget")


def metadata_fields(raw: bytes, *, title: str, controller_count: int = 1) -> dict[str, str]:
    """Check the text contract displayed by N64FlashcartMenu V0.3.3."""
    if len(raw) > 65536:
        raise RomError("metadata.ini exceeds the menu's 64 KiB limit")
    parser = configparser.ConfigParser(interpolation=None, delimiters=("=",))
    parser.optionxform = str
    try:
        parser.read_string(raw.decode("utf-8"))
        if parser.defaults() or parser.sections() != ["meta"]:
            raise ValueError("expected only a [meta] section")
        fields = dict(parser["meta"])
        for key, value in fields.items():
            if len(value.encode("utf-8")) > 255 or any(ord(c) < 32 for c in value):
                raise ValueError(f"invalid or overlong value for {key}")
        for key in ("name", "author", "release-date", "website", "num-players", "short-desc"):
            if not fields.get(key):
                raise ValueError(f"missing {key}")
        if fields["name"] != title:
            raise ValueError("name does not match the selected game")
        if date.fromisoformat(fields["release-date"]).isoformat() != fields["release-date"]:
            raise ValueError("release-date must use YYYY-MM-DD")
        if type(controller_count) is not int or not 1 <= controller_count <= 4:
            raise ValueError("controller count must be an integer from 1 to 4")
        if fields["num-players"] != str(controller_count):
            raise ValueError("num-players must match the configured runtime controllers")
        if len(fields["short-desc"].encode("utf-8")) > 120:
            raise ValueError("short-desc exceeds the menu's 120-byte limit")
        return fields
    except (UnicodeError, configparser.Error, ValueError) as error:
        raise RomError(f"invalid metadata.ini: {error}") from error


def embedded_metadata(
    data: bytes, *, title: str, expected: bytes | None = None, controller_count: int = 1
) -> dict[str, str]:
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            entries = archive.infolist()
            names = [entry.filename for entry in entries]
            if len(names) != len(set(names)) or names.count("metadata.ini") != 1:
                raise RomError("metadata ZIP needs one unique root metadata.ini")
            if len(entries) > 128 or sum(entry.file_size for entry in entries) > 4 * 1024 * 1024:
                raise RomError("metadata ZIP exceeds extraction budget")
            for entry in entries:
                path = PurePosixPath(entry.filename)
                if path.is_absolute() or ".." in path.parts or "\\" in entry.filename:
                    raise RomError("unsafe metadata ZIP entry name")
            if archive.getinfo("metadata.ini").file_size > 65536:
                raise RomError("metadata.ini exceeds the menu's 64 KiB limit")
            if archive.testzip() is not None:
                raise RomError("metadata ZIP CRC mismatch")
            raw = archive.read("metadata.ini")
    except (zipfile.BadZipFile, NotImplementedError, RuntimeError, EOFError, zlib.error) as error:
        raise RomError(f"invalid metadata ZIP: {error}") from error
    if expected is not None and raw != expected:
        raise RomError("embedded metadata differs from the selected build inputs")
    return metadata_fields(raw, title=title, controller_count=controller_count)


def validate_header(
    data: bytes,
    *,
    title: str,
    save_type: int = 0,
    controller_count: int = 1,
    expected_metadata: bytes | None = None,
    release_budget_mib: int = ROM_TARGET_MIB,
) -> dict[str, str]:
    if len(data) < 4096 or data[:4] != b"\x80\x37\x12\x40":
        raise RomError("invalid z64 header")
    validate_size(len(data), release_budget_mib=release_budget_mib)
    if len(data) % 16384:
        raise RomError("ROM must be padded to 16 KiB")
    if data[0x20:0x34].rstrip(b" \0") != title.encode("ascii"):
        raise RomError("unexpected ROM title")
    if data[0x3C:0x3E] != b"ED" or data[0x3F] & 0xF0 != save_type:
        raise RomError("save metadata does not match runtime backend")
    if data[0x3F] & 0x0F != 0x02:
        raise RomError("ROM must be region-free with RTC disabled and reserved flags clear")
    if type(controller_count) is not int or not 1 <= controller_count <= 4:
        raise RomError("controller count must be an integer from 1 to 4")
    if data[0x34:0x38] != b"\x00" * controller_count + b"\xff" * (4 - controller_count):
        raise RomError("controller metadata must match the configured N64 controllers")
    if data[0x38] != 1:
        raise RomError("embedded metadata flag missing or unsupported; rebuild the selected game")
    return embedded_metadata(
        data, title=title, expected=expected_metadata, controller_count=controller_count
    )


def validate_symbols(symbols: str, profile: str) -> None:
    names = [line.split()[-1] for line in symbols.splitlines() if line.split()]
    replay = [name for name in names if re.search("replay|autoplay", name, re.IGNORECASE)]
    if profile == "release" and replay:
        raise RomError(f"production ELF includes replay symbols: {', '.join(replay)}")
    if profile == "probe" and not replay:
        raise RomError("probe ELF missing deterministic replay entry points")


def dfs_files(data: bytes) -> dict[str, bytes]:
    """Bounded DragonFS 2.1 directory walk, matching the pinned SDK's dfs_internal.h."""
    # Version 2.1 repurposes next_entry/file_pointer for its lookup table.
    if len(data) < 26 or data[4:8] != b"\xff\xff\xff\xff":
        raise RomError("invalid DFS identifier")
    if data[12:25] != b"DragonFS 2.1\0" or data[25]:
        raise RomError("unsupported DFS version")
    pending = [(26, "", 0)]
    visited = set()
    files = {}
    while pending:
        offset, parent, depth = pending.pop()
        if depth > 100:
            raise RomError("DFS directory nesting exceeds SDK limit")
        while offset:
            if offset in visited or offset % 2 or offset > len(data) - 13:
                raise RomError("cyclic or out-of-bounds DFS entry")
            visited.add(offset)
            next_offset, flags, pointer = struct.unpack_from(">III", data, offset)
            end = data.find(b"\0", offset + 12, min(offset + 256, len(data)))
            if end < 0:
                raise RomError("unterminated DFS filename")
            try:
                name = data[offset + 12 : end].decode("utf-8")
            except UnicodeDecodeError as error:
                raise RomError("invalid DFS filename") from error
            if not name or name in {".", ".."} or "/" in name or "\\" in name:
                raise RomError("unsafe DFS filename")
            path = f"{parent}/{name}" if parent else name
            kind, length = flags >> 28, flags & 0x0FFFFFFF
            if kind == 1:
                pending.append((pointer, path, depth + 1))
            elif kind == 0:
                if path in files or pointer > len(data) or length > len(data) - pointer:
                    raise RomError("duplicate or out-of-bounds DFS file")
                files[path] = data[pointer : pointer + length]
            else:
                raise RomError("unknown DFS entry flags")
            offset = next_offset
    return files


def packed_dfs_names(packed: dict) -> set[str]:
    """The ROM filesystem names a pack manifest implies: every audio, font
    and video asset by name, and the one image pack that holds the images."""
    names = {name for name in packed["files"] if not name.startswith("images/")}
    if len(names) != len(packed["files"]):
        names.add("images.pack")
    return names


def validate_image_pack(files: dict[str, bytes], packed: dict) -> None:
    """The embedded pack is the one the manifest recorded and indexes every
    manifested image under its authored key."""
    from .image_pack import AUTHORED, image_key, read_pack

    images = [name[len("images/") :] for name in packed["files"] if name.startswith("images/")]
    if not images:
        return
    if "images.pack" not in files:
        raise RomError("ROM must embed the image pack")
    report = packed.get("pack", {})
    if hashlib.sha256(files["images.pack"]).hexdigest() != report.get("sha256"):
        raise RomError("image pack differs from the manifest")
    keys = {entry["key"] for entry in read_pack(files["images.pack"])["index"]}
    if keys != {image_key(name, AUTHORED) for name in images}:
        raise RomError("image pack does not index every manifested image")

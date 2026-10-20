"""Compact Android staging packs without re-encoding converted media.

Indexes retain every logical name and its metadata. Audio and playFile streams
share audio.bin; every other kind keeps its own blob. Duplicate Ogg Opus files
may differ only in page serials and checksums: keep the first complete original
file, including its valid checksums, rather than rewriting the stream.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from .convert import PACKS


def payload_digest(data: bytes, *, audio: bool) -> bytes:
    """Hash encoded bytes, ignoring only a single Opus stream's Ogg identity.

    Packet boundaries, headers, tags, sequence numbers, flags and granules all
    participate. Chained/multiplexed streams and header-only empty exports
    retain byte identity instead.
    """
    digest = hashlib.sha256()
    if not audio or data[:4] != b"OggS":
        return hashlib.sha256(data).digest()
    at, serial, sequence = 0, None, 0
    while at < len(data):
        if len(data) - at < 27 or data[at:at + 5] != b"OggS\0":
            raise ValueError("truncated or invalid Ogg page")
        segments = data[at + 26]
        body = at + 27 + segments
        if body > len(data):
            raise ValueError("truncated Ogg lacing table")
        end = body + sum(data[at + 27:body])
        if end > len(data):
            raise ValueError("truncated Ogg page payload")
        page_serial = data[at + 14:at + 18]
        page_sequence = int.from_bytes(data[at + 18:at + 22], "little")
        if serial is None:
            if not data[at + 5] & 2 or data[body:body + 8] != b"OpusHead":
                return hashlib.sha256(data).digest()
            serial = page_serial
        if page_serial != serial or page_sequence != sequence or at and data[at + 5] & 2:
            return hashlib.sha256(data).digest()
        digest.update(data[at:at + 14])
        digest.update(bytes(4))  # stream serial
        digest.update(data[at + 18:at + 22])
        digest.update(bytes(4))  # page checksum, which depends on the serial
        digest.update(data[at + 26:end])
        sequence += 1
        if data[at + 5] & 4 and end != len(data):
            return hashlib.sha256(data).digest()
        if end == len(data) and not data[at + 5] & 4:
            return hashlib.sha256(data).digest()
        at = end
    return digest.digest()


def stage_packs(source: Path, target: Path) -> dict:
    """Write version 1 Android indexes and unique encoded payloads to target."""
    input_bytes = sum((source / f"{kind}.bin").stat().st_size for kind in PACKS)
    stored_bytes = 0
    for blob in PACKS:
        if blob == "streams":
            continue
        kinds = ("audio", "streams") if blob == "audio" else (blob,)
        seen, offset = {}, 0
        with (target / f"{blob}.bin").open("wb") as output:
            for kind in kinds:
                index = json.loads((source / f"{kind}.json").read_text())
                if not isinstance(index, dict):
                    raise ValueError(f"{kind}.json is not a pack index")
                entries = {}
                with (source / f"{kind}.bin").open("rb") as original:
                    size = original.seek(0, 2)
                    for name, entry in sorted(index.items()):
                        if (not isinstance(entry, list) or len(entry) not in (2, 3)
                                or any(type(v) is not int or not 0 <= v <= 0xffffffff
                                       for v in entry)
                                or entry[0] + entry[1] > size):
                            raise ValueError(f"{kind}/{name}: invalid pack range or metadata")
                        start, length, *extra = entry
                        original.seek(start)
                        data = original.read(length)
                        if len(data) != length:
                            raise ValueError(f"{kind}/{name}: truncated pack payload")
                        try:
                            key = payload_digest(data, audio=blob == "audio")
                        except ValueError as error:
                            raise ValueError(f"{kind}/{name}: {error}") from error
                        if key not in seen:
                            seen[key] = offset
                            output.write(data)
                            offset += length
                        entries[name] = [seen[key], length, *extra]
                (target / f"{kind}.json").write_text(json.dumps(
                    {"version": 1, "blob": blob, "entries": entries}, separators=(",", ":")
                ) + "\n")
        stored_bytes += offset
    return {"input_bytes": input_bytes, "stored_bytes": stored_bytes,
            "saved_bytes": input_bytes - stored_bytes}

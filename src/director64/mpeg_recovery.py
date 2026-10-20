"""Hash-pinned conversion workarounds for damaged MPEG program streams.

Discard only audited broken packet spans, replacing them with padding packets.
The original media is never edited. Pixel concealment is a separate, explicit
FFmpeg option carried in the conversion receipt; it does not restore lost data.
"""

from __future__ import annotations

import hashlib


def prepare(source: bytes, spec: dict) -> tuple[bytes, dict]:
    def require(ok, message):
        if not ok:
            raise ValueError(f"MPEG recovery: {message}")

    require(spec.get("method") == "padding-and-decoder-concealment-v1", "unsupported method")
    require(
        len(source) == spec["bytes"] and hashlib.sha256(source).hexdigest() == spec["sha256"],
        "source differs from pinned damaged file",
    )
    output = bytearray(source)
    previous = 0
    for start, end in spec["padding_ranges"]:
        require(
            isinstance(start, int)
            and isinstance(end, int)
            and start >= 12
            and previous <= start < end <= len(source)
            and 6 < end - start <= 65541,
            "invalid packet range",
        )
        require(
            source[start - 12 : start - 8] == b"\0\0\1\xba"
            and source[end : end + 4] == b"\0\0\1\xba",
            "range is not bounded by MPEG-1 packs",
        )
        output[start:end] = (
            b"\0\0\1\xbe" + (end - start - 6).to_bytes(2, "big") + b"\xff" * (end - start - 6)
        )
        previous = end
    result = bytes(output)
    require(
        hashlib.sha256(result).hexdigest() == spec["output_sha256"], "repacked file hash differs"
    )
    return result, {
        "method": spec["method"],
        "source_sha256": spec["sha256"],
        "output_sha256": spec["output_sha256"],
        "padding_ranges": spec["padding_ranges"],
        "decoder_concealment": True,
        "restoration_claim": False,
    }

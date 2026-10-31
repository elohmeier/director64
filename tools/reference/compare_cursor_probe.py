#!/usr/bin/env python3
"""Compare recovered cursor masks with unchanged original Windows API renders."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path


def compare(recovered: Path, drive_c: Path) -> dict:
    verification = json.loads((recovered / "verification.json").read_text())
    owner = drive_c / "Original" / Path(verification["source"]["file"]).name
    if hashlib.sha256(owner.read_bytes()).hexdigest() != verification["source"]["sha256"]:
        raise ValueError("captured cursor owner differs from the pinned executable")
    log = (drive_c / "cursor-probe.txt").read_text()
    if "DIRECTOR64_CURSOR_PROBE_COMPLETE" not in log:
        raise ValueError("native cursor probe did not complete")
    records = []
    for entry in verification["source"]["entries"]:
        group = entry["group"].lower()
        if group not in ("crosscursor", "pluscursor"):
            raise ValueError("probe comparison has no captured group " + group)
        filename = "cross" if group == "crosscursor" else "plus"
        source = (recovered / "images" / f'd64-cursor-{entry["id"]}.dcur').read_bytes()
        if source[:4] != b"D64C" or len(source) < 16:
            raise ValueError("invalid recovered cursor")
        width, height, hot_x, hot_y, version, reserved = struct.unpack_from(">6H", source, 4)
        if version != 1 or reserved or len(source) != 16 + width * height:
            raise ValueError("invalid recovered cursor dimensions")
        expected_hotspot = f"hotspot={hot_x},{hot_y} DrawIconEx=1 error=0 saved=1"
        if not re.search(re.escape(entry["group"]) + r" LoadCursor=\d+ GetIconInfo=1 "
                         + re.escape(expected_hotspot), log, re.IGNORECASE):
            raise ValueError("native cursor probe failed or hotspot differs")
        bitmap = (drive_c / f"cursor-{filename}.bmp").read_bytes()
        if bitmap[:2] != b"BM" or len(bitmap) < 54:
            raise ValueError("native cursor capture is not a BMP")
        offset = struct.unpack_from("<I", bitmap, 10)[0]
        header, native_w, native_h, planes, bits, compression = struct.unpack_from(
            "<IiiHHI", bitmap, 14)
        if (header, native_w, abs(native_h), planes, bits, compression) != (
                40, width, height, 1, 32, 0) or offset < 54:
            raise ValueError("native cursor capture layout differs")
        if len(bitmap) != offset + width * height * 4:
            raise ValueError("native cursor capture length differs")
        actual = []
        for y in range(height):
            row = y if native_h < 0 else height - 1 - y
            actual.extend(struct.unpack_from("<I", bitmap, offset + (row * width + x) * 4)[0]
                          & 0xFFFFFF for x in range(width))
        outcomes = (0, 0xFFFFFF, 0x123456, 0xEDCBA9)
        if any(mask > 3 for mask in source[16:]):
            raise ValueError("invalid recovered cursor mask")
        expected = [outcomes[mask] for mask in source[16:]]
        differences = sum(a != b for a, b in zip(actual, expected, strict=True))
        records.append({"id": entry["id"], "group": entry["group"],
                        "pixels": len(expected), "changed_pixels": differences,
                        "passing": differences == 0,
                        "source_sha256": hashlib.sha256(source).hexdigest(),
                        "native_bmp_sha256": hashlib.sha256(bitmap).hexdigest()})
    return {"status": "passed" if all(row["passing"] for row in records) else "failed",
            "scope": "unchanged original PE cursor resources rendered by Windows APIs under Wine",
            "original_projector_gameplay_evidence": False, "comparisons": records}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recovered", type=Path)
    parser.add_argument("drive_c", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    report = compare(args.recovered, args.drive_c)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    return int(report["status"] != "passed")


if __name__ == "__main__":
    raise SystemExit(main())

from __future__ import annotations

import io
import json
import zipfile
from pathlib import Path

import pytest

from director64.evidence import REQUIRED_ARTIFACTS, sha256
from director64.project import GameSpec

GameSpec.load("findus-workshop").activate()


@pytest.fixture
def rom_factory():
    """Synthetic ROM containers, without guest code or recovered game assets."""

    def make(
        *,
        title="Findus Workshop",
        save_type=0x50,
        metadata=None,
        entries=None,
        code_bytes=16384,
        controller_count=1,
    ):
        data = bytearray(code_bytes)
        data[:4] = bytes.fromhex("80371240")
        data[0x20:0x34] = title.encode("ascii").ljust(20, b"\0")
        data[0x34:0x38] = b"\0" * controller_count + b"\xff" * (4 - controller_count)
        data[0x38] = 1
        data[0x3C:0x3E] = b"ED"
        data[0x3F] = save_type | 2
        if metadata is None:
            metadata = (
                f"[meta]\nname={title}\nauthor=Test fixture\nrelease-date=2026-09-06\n"
                f"website=https://example.com\nnum-players={controller_count}\n"
                "short-desc=v0.1.0+fixture - Requires 8 MiB RAM.\n"
            ).encode()
        buffer = io.BytesIO(data)
        with zipfile.ZipFile(buffer, "a") as archive:
            for name, raw in entries if entries is not None else [("metadata.ini", metadata)]:
                archive.writestr(zipfile.ZipInfo(name, (2026, 9, 6, 0, 0, 0)), raw)
        result = buffer.getvalue()
        return result + bytes(-len(result) % 16384)

    return make


@pytest.fixture
def evidence_factory(tmp_path):
    """Synthetic host fixtures only; these files are never repository qualification evidence."""

    def make_run(*, platform="host", assertion_ids=None):
        ids = assertion_ids or ["pointer", "audio", "scene:DEMO.DXR:logic"]
        events = [
            {
                "schema_version": 1,
                "run_id": "synthetic-run",
                "scenario_id": "demo",
                "sequence": 0,
                "tick": 0,
                "type": "SCENARIO_START",
            }
        ]
        assertions = []
        for index, assertion_id in enumerate(ids, 1):
            events.append(
                {
                    "schema_version": 1,
                    "run_id": "synthetic-run",
                    "scenario_id": "demo",
                    "sequence": index,
                    "tick": index,
                    "type": "CHECKPOINT",
                    "assertion_id": assertion_id,
                    "actual": index,
                }
            )
            assertions.append(
                {
                    "id": assertion_id,
                    "event_sequence": index,
                    "actual": index,
                    "expected": index,
                    "operator": "eq",
                    "status": "passing",
                }
            )
        events.append(
            {
                "schema_version": 1,
                "run_id": "synthetic-run",
                "scenario_id": "demo",
                "sequence": len(events),
                "tick": len(events),
                "type": "SCENARIO_OK",
            }
        )
        artifacts = []
        for role in sorted(REQUIRED_ARTIFACTS):
            path = tmp_path / f"synthetic-{role}.txt"
            path.write_text(
                "\n".join(json.dumps(event) for event in events) + "\n"
                if role == "guest_events"
                else f"synthetic {role} fixture\n"
            )
            artifacts.append({"role": role, "path": path.name, "sha256": sha256(path)})
        record = {
            "schema_version": 1,
            "run_id": "synthetic-run",
            "scenario_id": "demo",
            "scenario_version": 1,
            "catalog_version": "scenarios-v1",
            "recorded_at": "2026-09-05",
            "platform": {"id": platform, "version": "synthetic", "configuration": "test fixture"},
            "toolchain": {"revision": "fixture", "image_sha256": "a" * 64, "dirty": False},
            "profile": {"rdram_bytes": 8388608, "save_backend": "memory", "save_path": "memory"},
            "replay": {"seed": 1, "timestep_numerator": 1, "timestep_denominator": 60},
            "expected_assertions": ids,
            "artifacts": artifacts,
            "assertions": assertions,
        }
        return record, events

    return make_run


@pytest.fixture
def pin_evidence(tmp_path):
    def write(record, *, assertions=None, name="run.json"):
        path: Path = tmp_path / name
        path.write_text(json.dumps(record))
        return {
            "path": path.name,
            "sha256": sha256(path),
            "assertions": assertions or record["expected_assertions"],
        }

    return write

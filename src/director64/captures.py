"""Shared decoded-media validation and evidence comparison utilities."""

from __future__ import annotations

import hashlib
import json
import math
import subprocess
from fractions import Fraction
from pathlib import Path
from typing import Any

from director64.evidence import (
    check_artifact,
    load_record,
    sha256,
    validate_reference,
)

SEMANTIC_EVENTS = {"SCENARIO_START", "SCENE_LOADED", "SCENE_UNLOADED", "CHECKPOINT", "SCENARIO_OK"}


class CaptureError(ValueError):
    """Capture data fails a declared validation condition."""


def command(args: list[str], *, cwd: Path | None = None, timeout: int = 30) -> str:
    try:
        result = subprocess.run(
            args, cwd=cwd, check=True, capture_output=True, text=True, timeout=timeout
        )
    except (OSError, subprocess.SubprocessError) as error:
        detail = getattr(error, "stderr", "") or str(error)
        raise CaptureError(f"{args[0]} failed: {detail.strip()}") from error
    return result.stdout


def write_json(path: Path, data: Any) -> None:
    with path.open("x", encoding="utf-8") as handle:
        json.dump(data, handle, indent=2, sort_keys=True, allow_nan=False)
        handle.write("\n")


def pin(path: Path, root: Path, role: str | None = None) -> dict[str, str]:
    reference = {"path": str(path.resolve().relative_to(root.resolve())), "sha256": sha256(path)}
    if role:
        reference["role"] = role
    return reference


def validate_probe(
    probe: dict[str, Any], *, expected_frames: int = 720, expected_fps: int = 60
) -> dict[str, Any]:
    try:
        streams = [stream for stream in probe["streams"] if stream["codec_type"] == "video"]
        if len(streams) != 1:
            raise CaptureError(f"expected one video stream, found {len(streams)}")
        stream = streams[0]
        if int(stream["width"]) != 640 or int(stream["height"]) != 480:
            raise CaptureError("capture must decode to 640x480")
        if Fraction(stream["avg_frame_rate"]) != expected_fps:
            raise CaptureError("capture frame rate differs from declared rate")
        if int(stream["nb_read_frames"]) != expected_frames:
            raise CaptureError("decoded capture frame count differs from declared count")
        duration = float(probe["format"]["duration"])
        if (
            not math.isfinite(duration)
            or abs(duration - expected_frames / expected_fps) > 1 / expected_fps + 0.001
        ):
            raise CaptureError("capture duration differs from declared duration")
        return {
            "width": 640,
            "height": 480,
            "frames": expected_frames,
            "fps": expected_fps,
            "duration_seconds": duration,
            "codec": stream["codec_name"],
        }
    except (KeyError, TypeError, ValueError, ZeroDivisionError) as error:
        if isinstance(error, CaptureError):
            raise
        raise CaptureError(f"invalid FFprobe result: {error}") from error


def validate_media(
    video: Path, *, expected_frames: int = 720, expected_fps: int = 60
) -> tuple[dict[str, Any], dict[str, Any]]:
    # Read/count actual decoded frames, then independently decode all frames with
    # errors fatal. Supplied JSON metadata alone never establishes valid media.
    probe = json.loads(
        command(
            [
                "ffprobe",
                "-v",
                "error",
                "-count_frames",
                "-show_streams",
                "-show_format",
                "-of",
                "json",
                str(video),
            ]
        )
    )
    metrics = validate_probe(probe, expected_frames=expected_frames, expected_fps=expected_fps)
    command(
        [
            "ffmpeg",
            "-hide_banner",
            "-v",
            "error",
            "-xerror",
            "-i",
            str(video),
            "-map",
            "0:v:0",
            "-f",
            "null",
            "-",
        ]
    )
    metrics.update(sha256=sha256(video), decoded_without_errors=True)
    return metrics, probe


def semantic_trace(events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [
        {
            key: event[key]
            for key in ("type", "tick", "scene_id", "assertion_id", "actual")
            if key in event
        }
        for event in events
        if event["type"] in SEMANTIC_EVENTS
    ]


def compare_run_records(root: Path, references: Any) -> dict[str, Any]:
    if not isinstance(references, list) or len(references) != 2:
        raise CaptureError("determinism requires exactly two pinned run records")
    records = []
    traces = []
    for reference in references:
        result = validate_reference(reference, root)
        if not result.valid:
            raise CaptureError("; ".join(result.errors))
        records.append((reference, result.record))
        event_artifact = next(
            item for item in result.record["artifacts"] if item["role"] == "guest_events"
        )
        path = check_artifact(root, event_artifact)
        traces.append(semantic_trace([json.loads(line) for line in path.read_text().splitlines()]))
    capture_ids = [record.get("capture_id") for _, record in records]
    if not all(isinstance(value, str) and value for value in capture_ids) or (
        capture_ids[0] == capture_ids[1]
    ):
        raise CaptureError("determinism requires two independent capture identities")
    for key in ("scenario_id", "scenario_version", "catalog_version", "platform", "replay"):
        if records[0][1].get(key) != records[1][1].get(key):
            raise CaptureError(f"determinism requires identical {key} metadata")
    for role in ("rom", "elf", "replay", "asset_manifest", "converter"):
        digests = [
            next(item["sha256"] for item in record["artifacts"] if item["role"] == role)
            for _, record in records
        ]
        if digests[0] != digests[1]:
            raise CaptureError(f"determinism requires identical {role} inputs")
    if traces[0] != traces[1]:
        raise CaptureError(
            "independent captures disagree on ordered simulation events or guest ticks"
        )
    return {
        "schema_version": 1,
        "status": "passing",
        "scenario_id": records[0][1]["scenario_id"],
        "runs": [reference for reference, _ in records],
        "compared_event_types": sorted(SEMANTIC_EVENTS),
        "semantic_trace": traces[0],
        "semantic_sha256": hashlib.sha256(
            json.dumps(traces[0], sort_keys=True, separators=(",", ":")).encode()
        ).hexdigest(),
        "note": "Independent runs compared using simulation checkpoints and guest ticks",
    }


def compare_captures(root: Path, suite: Path) -> dict[str, Any]:
    result = compare_run_records(
        root, [pin(suite / name / "evidence.json", root) for name in ("run-1", "run-2")]
    )
    write_json(suite / "determinism.json", result)
    return result


def validate_comparison(reference: Any, root: Path) -> dict[str, Any]:
    record = load_record(check_artifact(root, reference))
    derived = compare_run_records(root, record.get("runs"))
    for key in (
        "schema_version",
        "status",
        "scenario_id",
        "compared_event_types",
        "semantic_trace",
        "semantic_sha256",
    ):
        if record.get(key) != derived[key]:
            raise CaptureError(f"recorded determinism {key} differs from evaluated runs")
    return derived

"""Version 1 immutable run evidence and ordered guest-event validation.

Evidence paths are relative to the project root, including ignored local artifacts.
A tracker reference pins the TOML/JSON run record itself by SHA-256. Every artifact
inside that record is pinned independently. A legacy record can be retained for
historical context but never establishes a passing assertion or platform claim.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
import tomllib
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 1
PLATFORMS = ("host", "native-replay", "rom", "gopher64", "ares", "m64", "original-n64")
SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")
REQUIRED_ARTIFACTS = {
    "iso",
    "converter",
    "rom",
    "elf",
    "map",
    "asset_manifest",
    "replay",
    "guest_events",
    "telemetry",
}
EVENT_TYPES = {
    "SCENARIO_START",
    "SCENE_LOADED",
    "SCENE_FIRST_FRAME",
    "CHECKPOINT",
    "SCENARIO_OK",
    "SCENARIO_FAIL",
    "MEMORY_SNAPSHOT",
    "MEMORY_OK",
    "CRASH",
    "TIMING_OVERRUN",
    "AUDIO_UNDERRUN",
    "SCENE_UNLOADED",
}


class EvidenceError(ValueError):
    """An evidence record is incomplete or cannot substantiate its claim."""


@dataclass
class EvidenceResult:
    record: dict[str, Any] = field(default_factory=dict)
    errors: list[str] = field(default_factory=list)
    assertions: list[str] = field(default_factory=list)

    @property
    def valid(self) -> bool:
        return not self.errors

    def as_dict(self) -> dict[str, Any]:
        platform = self.record.get("platform", {})
        return {
            "schema_version": SCHEMA_VERSION,
            "run_id": self.record.get("run_id"),
            "scenario_id": self.record.get("scenario_id"),
            "platform": platform.get("id") if isinstance(platform, dict) else None,
            "status": "passing" if self.valid else "failed",
            "passing_assertions": self.assertions if self.valid else [],
            "errors": self.errors,
        }


def sha256(path: Path) -> str:
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def artifact_path(root: Path, value: Any) -> Path:
    if not isinstance(value, str) or not value:
        raise EvidenceError("artifact path must be a non-empty relative path")
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        raise EvidenceError(f"artifact path escapes project root: {value}")
    resolved = (root / path).resolve()
    if not resolved.is_relative_to(root.resolve()):
        raise EvidenceError(f"artifact path escapes project root: {value}")
    if not resolved.is_file():
        raise EvidenceError(f"missing evidence artifact: {value}")
    return resolved


def check_artifact(root: Path, reference: Any) -> Path:
    if not isinstance(reference, dict):
        raise EvidenceError("evidence requires a path and SHA-256, not a test identifier")
    digest = reference.get("sha256")
    if not isinstance(digest, str) or not SHA256_RE.fullmatch(digest):
        raise EvidenceError("artifact requires a lowercase SHA-256")
    path = artifact_path(root, reference.get("path"))
    if sha256(path) != digest:
        raise EvidenceError(f"SHA-256 mismatch: {reference['path']}")
    return path


def load_record(path: Path) -> dict[str, Any]:
    try:
        if path.suffix == ".toml":
            with path.open("rb") as handle:
                record = tomllib.load(handle)
        else:
            record = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise EvidenceError(f"invalid evidence record {path}: {error}") from error
    if not isinstance(record, dict):
        raise EvidenceError("evidence record must be an object")
    return record


def _strings(value: Any) -> bool:
    return isinstance(value, list) and all(isinstance(item, str) and item for item in value)


def _integer(value: Any, *, minimum: int = 0) -> bool:
    return type(value) is int and value >= minimum


def assertion_passes(actual: Any, expected: Any, operator: Any) -> bool:
    """Evaluate a declared contract; stored status strings are not test results."""
    if operator == "eq":
        try:
            return type(actual) is type(expected) and json.dumps(
                actual, sort_keys=True, allow_nan=False
            ) == json.dumps(expected, sort_keys=True, allow_nan=False)
        except TypeError, ValueError:
            return False
    if isinstance(operator, str) and operator in {"le", "ge"}:
        if type(actual) not in {int, float} or type(expected) not in {int, float}:
            return False
        if not math.isfinite(actual) or not math.isfinite(expected):
            return False
        return actual <= expected if operator == "le" else actual >= expected
    return False


def validate_events(
    events: list[Any], record: dict[str, Any], errors: list[str]
) -> dict[int, dict[str, Any]]:
    by_sequence: dict[int, dict[str, Any]] = {}
    previous_tick = -1
    loaded: set[str] = set()
    first_frames: set[str] = set()
    for sequence, event in enumerate(events):
        if not isinstance(event, dict):
            errors.append(f"event {sequence}: expected an object")
            continue
        if type(event.get("schema_version")) is not int or event["schema_version"] != 1:
            errors.append(f"event {sequence}: unsupported schema_version")
        if event.get("run_id") != record.get("run_id"):
            errors.append(f"event {sequence}: wrong run identity")
        if event.get("scenario_id") != record.get("scenario_id"):
            errors.append(f"event {sequence}: wrong scenario identity")
        if type(event.get("sequence")) is not int or event["sequence"] != sequence:
            errors.append(f"event {sequence}: sequence must be contiguous and ordered from zero")
        tick = event.get("tick")
        if not _integer(tick) or tick < previous_tick:
            errors.append(f"event {sequence}: invalid or decreasing guest tick")
        else:
            previous_tick = tick
        kind = event.get("type")
        if not isinstance(kind, str):
            errors.append(f"event {sequence}: event type must be a string")
            continue
        if kind not in EVENT_TYPES:
            errors.append(f"event {sequence}: unknown event type {kind!r}")
        if kind in {"SCENARIO_FAIL", "CRASH", "TIMING_OVERRUN", "AUDIO_UNDERRUN"}:
            errors.append(f"event {sequence}: failure event {kind}")
        if kind == "SCENARIO_START" and sequence != 0:
            errors.append(f"event {sequence}: duplicate or late SCENARIO_START")
        if kind == "SCENARIO_OK" and sequence != len(events) - 1:
            errors.append(f"event {sequence}: premature or duplicate SCENARIO_OK")
        if kind in {"SCENE_LOADED", "SCENE_FIRST_FRAME", "SCENE_UNLOADED"}:
            scene_id = event.get("scene_id")
            if not isinstance(scene_id, str) or not scene_id:
                errors.append(f"event {sequence}: missing scene_id")
            elif kind == "SCENE_LOADED":
                if scene_id in loaded:
                    errors.append(f"event {sequence}: scene already loaded")
                loaded.add(scene_id)
            elif kind == "SCENE_FIRST_FRAME":
                if scene_id not in loaded or scene_id in first_frames:
                    errors.append(f"event {sequence}: first frame without a new scene load")
                first_frames.add(scene_id)
            elif scene_id not in loaded:
                errors.append(f"event {sequence}: unload before load")
            else:
                loaded.remove(scene_id)
                first_frames.discard(scene_id)
        if kind == "MEMORY_OK":
            actual, expected = event.get("free_bytes"), event.get("minimum_free_bytes")
            if not _integer(actual) or not _integer(expected, minimum=1) or actual < expected:
                errors.append(f"event {sequence}: MEMORY_OK without an evaluated passing budget")
        by_sequence[sequence] = event
    if not events or not isinstance(events[0], dict):
        errors.append("events: missing SCENARIO_START")
    elif events[0].get("type") != "SCENARIO_START":
        errors.append("events: first event must be SCENARIO_START")
    if not events or not isinstance(events[-1], dict):
        errors.append("events: missing final SCENARIO_OK")
    elif events[-1].get("type") != "SCENARIO_OK":
        errors.append("events: final event must be SCENARIO_OK")
    return by_sequence


def validate_record(record: dict[str, Any], root: Path) -> EvidenceResult:
    result = EvidenceResult(record=record)
    errors = result.errors
    if type(record.get("schema_version")) is not int or record["schema_version"] != SCHEMA_VERSION:
        errors.append("unsupported evidence schema_version; legacy evidence cannot pass")
        return result
    if record.get("legacy") is True:
        errors.append("legacy evidence cannot pass")
    for key in ("run_id", "scenario_id", "catalog_version", "recorded_at"):
        if not isinstance(record.get(key), str) or not record[key]:
            errors.append(f"missing evidence {key}")
    if not _integer(record.get("scenario_version"), minimum=1):
        errors.append("scenario_version must be a positive integer")
    platform = record.get("platform", {})
    if not isinstance(platform, dict):
        errors.append("platform must be an object")
        platform = {}
    if platform.get("id") not in PLATFORMS:
        errors.append("unknown evidence platform")
    for key in ("version", "configuration"):
        if not isinstance(platform.get(key), str) or not platform[key]:
            errors.append(f"missing platform {key}")
    for group, strings, integers in (
        ("toolchain", ("revision", "image_sha256"), ()),
        ("profile", ("save_backend", "save_path"), ("rdram_bytes",)),
        ("replay", (), ("seed", "timestep_numerator", "timestep_denominator")),
    ):
        data = record.get(group, {})
        if not isinstance(data, dict):
            errors.append(f"{group} must be an object")
            continue
        for key in strings:
            if not isinstance(data.get(key), str) or not data[key]:
                errors.append(f"missing {group}.{key}")
        for key in integers:
            minimum = 0 if key == "seed" else 1
            if not _integer(data.get(key), minimum=minimum):
                errors.append(f"invalid {group}.{key}")
    toolchain = record.get("toolchain")
    if isinstance(toolchain, dict):
        if type(toolchain.get("dirty")) is not bool:
            errors.append("toolchain.dirty must record a boolean")
        if not SHA256_RE.fullmatch(str(toolchain.get("image_sha256", ""))):
            errors.append("invalid toolchain image SHA-256")

    artifacts = record.get("artifacts")
    paths: dict[str, Path] = {}
    if not isinstance(artifacts, list):
        errors.append("artifacts must be a list")
        artifacts = []
    for artifact in artifacts:
        if not isinstance(artifact, dict):
            errors.append("artifact must be an object")
            continue
        role = artifact.get("role")
        if not isinstance(role, str) or not role:
            errors.append("artifact requires a role")
            continue
        if role in paths:
            errors.append(f"duplicate artifact role: {role}")
        try:
            paths[role] = check_artifact(root, artifact)
        except (EvidenceError, OSError) as error:
            errors.append(str(error))
    for role in sorted(REQUIRED_ARTIFACTS - paths.keys()):
        errors.append(f"missing validated artifact: {role}")

    events: list[Any] = []
    if "guest_events" in paths:
        try:
            events = [json.loads(line) for line in paths["guest_events"].read_text().splitlines()]
        except (OSError, ValueError) as error:
            errors.append(f"invalid guest events: {error}")
    by_sequence = validate_events(events, record, errors)
    required = record.get("expected_assertions")
    if not _strings(required) or not required or len(set(required)) != len(required):
        errors.append("expected_assertions must be non-empty unique ordered IDs")
        required = []
    assertions = record.get("assertions")
    if not isinstance(assertions, list) or not all(isinstance(item, dict) for item in assertions):
        errors.append("assertions must be a list of objects")
        assertions = []
    if [item.get("id") for item in assertions] != required:
        errors.append("assertion results do not exactly match the required order")
    previous_sequence = -1
    used_sequences: set[int] = set()
    for assertion in assertions:
        label = assertion.get("id", "<unknown>")
        sequence = assertion.get("event_sequence")
        if not _integer(sequence) or sequence <= previous_sequence:
            errors.append(f"assertion {label}: event order is invalid")
            continue
        previous_sequence = sequence
        used_sequences.add(sequence)
        event = by_sequence.get(sequence, {})
        if event.get("type") != "CHECKPOINT" or event.get("assertion_id") != label:
            errors.append(f"assertion {label}: does not identify its CHECKPOINT event")
        if (
            "actual" not in assertion
            or "actual" not in event
            or not assertion_passes(event.get("actual"), assertion.get("actual"), "eq")
        ):
            errors.append(f"assertion {label}: actual value differs from guest event")
        if "expected" not in assertion or not assertion_passes(
            assertion.get("actual"), assertion.get("expected"), assertion.get("operator")
        ):
            errors.append(f"assertion {label}: evaluated result failed")
        if assertion.get("status") != "passing":
            errors.append(f"assertion {label}: recorded result is not passing")
        result.assertions.append(label)
    checkpoints = {seq for seq, event in by_sequence.items() if event.get("type") == "CHECKPOINT"}
    if checkpoints != used_sequences:
        errors.append("every CHECKPOINT must have exactly one ordered assertion result")
    if not result.valid:
        result.assertions = []
    return result


def validate_reference(reference: Any, root: Path) -> EvidenceResult:
    try:
        path = check_artifact(root, reference)
        return validate_record(load_record(path), root)
    except (EvidenceError, OSError) as error:
        return EvidenceResult(errors=[str(error)])


def junit_report(result: EvidenceResult) -> str:
    suite = ET.Element(
        "testsuite", name="director64-evidence", tests="1", failures="0" if result.valid else "1"
    )
    case = ET.SubElement(suite, "testcase", name=str(result.record.get("run_id", "evidence")))
    if not result.valid:
        failure = ET.SubElement(case, "failure", message="invalid run evidence")
        failure.text = "\n".join(result.errors)
    return ET.tostring(suite, encoding="unicode") + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("record", type=Path)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--format", choices=("json", "junit"), default="json")
    args = parser.parse_args(argv)
    try:
        result = validate_record(load_record(args.record), args.root)
    except EvidenceError as error:
        result = EvidenceResult(errors=[str(error)])
    print(
        junit_report(result) if args.format == "junit" else json.dumps(result.as_dict(), indent=2),
        end="\n",
    )
    return 0 if result.valid else 2


if __name__ == "__main__":
    sys.exit(main())

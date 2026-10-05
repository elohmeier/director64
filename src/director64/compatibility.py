"""Measure static Director requirements, native implementation, and test evidence."""

from __future__ import annotations

import argparse
import json
import re
import sys
import tomllib
from collections import Counter
from pathlib import Path
from typing import Any

from director64.captures import CaptureError, validate_comparison
from director64.evidence import (
    PLATFORMS,
    EvidenceError,
    artifact_path,
    assertion_passes,
    validate_reference,
)
from director64.local_evidence import (
    LocalEvidenceError,
    apply_overlay,
    overlay_path,
    read_overlay,
)
from director64.project import work_dir

TRACKER_SCHEMA_VERSION = 1
CAPABILITY_STATUSES = ("missing", "partial", "implemented")
SCENARIO_STATUSES = ("missing", "blocked", "passing")
QUALIFICATION_GATES = (
    "none",
    "static-analysis",
    "host",
    "native-replay",
    "rom",
    "gopher64",
    "ares",
    "m64",
    "original-n64",
)
CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
HANDLER_RE = re.compile(r"^\s*on\s+([A-Za-z_][A-Za-z0-9_]*)", re.IGNORECASE | re.MULTILINE)
CALL_SYNTAX_WORDS = {
    "and",
    "else",
    "if",
    "not",
    "on",
    "or",
    "repeat",
    "return",
    "set",
    "then",
    "to",
    "while",
}


class TrackerError(Exception):
    """Raised when tracked compatibility data is inconsistent."""


def load_toml(path: Path) -> dict[str, Any]:
    try:
        with path.open("rb") as handle:
            return tomllib.load(handle)
    except FileNotFoundError as error:
        raise TrackerError(f"missing tracker: {path}") from error
    except tomllib.TOMLDecodeError as error:
        raise TrackerError(f"invalid TOML in {path}: {error}") from error


def load_inventory(path: Path, *, required: bool) -> dict[str, Any] | None:
    if not path.exists():
        if required:
            raise TrackerError(
                f"missing Director inventory: {path}; run `director64 assets --game <slug>` first"
            )
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise TrackerError(f"invalid Director inventory {path}: {error}") from error


def require_unique(items: list[dict[str, Any]], key: str, label: str, errors: list[str]) -> None:
    values = [item.get(key) for item in items]
    missing = sum(not isinstance(value, str) or not value for value in values)
    if missing:
        errors.append(f"{label}: {missing} entries have no {key!r}")
    duplicates = sorted(
        value
        for value, count in Counter(value for value in values if isinstance(value, str)).items()
        if count > 1
    )
    if duplicates:
        errors.append(f"{label}: duplicate {key} values: {', '.join(map(str, duplicates))}")


def validate_trackers(
    capabilities_data: dict[str, Any],
    scenes_data: dict[str, Any],
    scenarios_data: dict[str, Any],
    inventory: dict[str, Any] | None,
    *,
    evidence_root: Path | None = None,
) -> list[str]:
    errors: list[str] = []
    root = evidence_root or Path.cwd()
    for label, data in (
        ("capabilities", capabilities_data),
        ("scenes", scenes_data),
        ("scenarios", scenarios_data),
    ):
        if type(data.get("schema_version")) is not int or data["schema_version"] != 1:
            errors.append(f"{label}: unsupported schema_version")
        if not isinstance(data.get("catalog_version"), str) or not data["catalog_version"]:
            errors.append(f"{label}: missing catalog_version")
    capabilities = capabilities_data.get("capabilities", [])
    scenes = scenes_data.get("scenes", [])
    scenarios = scenarios_data.get("scenarios", [])

    if (
        not isinstance(capabilities, list)
        or not capabilities
        or not all(isinstance(item, dict) for item in capabilities)
    ):
        errors.append("capabilities: expected a non-empty [[capabilities]] list")
        capabilities = []
    if (
        not isinstance(scenes, list)
        or not scenes
        or not all(isinstance(item, dict) for item in scenes)
    ):
        errors.append("scenes: expected a non-empty [[scenes]] list")
        scenes = []
    if (
        not isinstance(scenarios, list)
        or not scenarios
        or not all(isinstance(item, dict) for item in scenarios)
    ):
        errors.append("scenarios: expected a non-empty [[scenarios]] list")
        scenarios = []

    require_unique(capabilities, "id", "capabilities", errors)
    require_unique(scenes, "file", "scenes", errors)
    require_unique(scenarios, "id", "scenarios", errors)
    if any(
        not isinstance(item.get(key), str) or not item[key]
        for items, key in ((capabilities, "id"), (scenes, "file"), (scenarios, "id"))
        for item in items
    ):
        return errors
    validated_runs: dict[tuple[str, str], Any] = {}

    def strings(value: Any, label: str) -> list[str]:
        if not isinstance(value, list) or not all(isinstance(item, str) and item for item in value):
            errors.append(f"{label}: expected a list of non-empty strings")
            return []
        if len(value) != len(set(value)):
            errors.append(f"{label}: duplicate values")
        return value

    def evidence_claims(
        references: Any,
        label: str,
        *,
        platform: str | None = None,
        scenario: dict[str, Any] | None = None,
    ) -> set[str]:
        if not isinstance(references, list):
            errors.append(f"{label}: evidence must be a list of pinned references")
            return set()
        passing: set[str] = set()
        for reference in references:
            cache_key = None
            if (
                isinstance(reference, dict)
                and isinstance(reference.get("path"), str)
                and isinstance(reference.get("sha256"), str)
            ):
                cache_key = (reference["path"], reference["sha256"])
            result = validated_runs.get(cache_key)
            if result is None:
                result = validate_reference(reference, root)
                if cache_key is not None:
                    validated_runs[cache_key] = result
            if not result.valid:
                errors.extend(f"{label}: {error}" for error in result.errors)
                continue
            record = result.record
            if platform is not None and record["platform"]["id"] != platform:
                errors.append(f"{label}: evidence platform does not match {platform}")
                continue
            if scenario is not None and (
                record["scenario_id"] != scenario["id"]
                or record["scenario_version"] != scenario.get("version")
                or record["catalog_version"] != scenarios_data.get("catalog_version")
            ):
                errors.append(f"{label}: evidence scenario/catalog identity or version mismatch")
                continue
            if scenario is not None:
                contracts = scenario.get("assertion_contracts", [])
                expected_contracts = [
                    {key: assertion.get(key) for key in ("id", "operator", "expected")}
                    for assertion in record["assertions"]
                ]
                if not assertion_passes(contracts, expected_contracts, "eq"):
                    errors.append(f"{label}: run expectations differ from the versioned catalog")
                    continue
            assertions = strings(reference.get("assertions", []), f"{label} assertions")
            if not assertions or not set(assertions) <= set(result.assertions):
                errors.append(f"{label}: evidence does not establish its selected assertions")
                continue
            passing.update(assertions)
        return passing

    def auxiliary_claims(
        item: dict[str, Any],
        label: str,
        allowed: set[str],
        scenario: dict[str, Any] | None = None,
    ) -> None:
        legacy = strings(item.get("legacy_evidence", []), f"{label} legacy_evidence")
        for path in legacy:
            try:
                artifact_path(root, path)
            except EvidenceError as error:
                errors.append(f"{label}: {error}")
        platforms = item.get("platforms", {})
        if not isinstance(platforms, dict):
            errors.append(f"{label}: platforms must be a table")
        else:
            for platform, references in platforms.items():
                if platform not in PLATFORMS:
                    errors.append(f"{label}: unknown platform {platform!r}")
                claims = evidence_claims(references, label, platform=platform, scenario=scenario)
                if not claims or not claims <= allowed:
                    errors.append(f"{label}: platform {platform} requires passing evidence")
        reference = item.get("reference", {"status": "missing"})
        if not isinstance(reference, dict):
            errors.append(f"{label}: reference must be a table")
            return
        reference_status = reference.get("status")
        if not isinstance(reference_status, str) or reference_status not in {
            "missing",
            "partial",
            "matched",
        }:
            errors.append(f"{label}: invalid reference status")
            return
        checks = strings(reference.get("required_checks", []), f"{label} reference checks")
        passing = evidence_claims(reference.get("evidence", []), f"{label} reference")
        if reference_status in {"partial", "matched"}:
            if not checks or not passing:
                errors.append(f"{label}: reference matching requires passing checks")
            if reference_status == "matched" and not set(checks) <= passing:
                errors.append(f"{label}: reference matching has incomplete checks")
            # Reference capture schema/tolerances are a separate gate; a target run alone
            # cannot establish agreement with the original Director runtime.
            errors.append(f"{label}: original-runtime comparison evidence schema is not yet frozen")

    for capability in capabilities:
        capability_id = capability.get("id", "<unknown>")
        status = capability.get("status")
        patterns = capability.get("patterns", [])
        calls = capability.get("calls", [])
        label = f"capability {capability_id}"
        required_cases = strings(capability.get("required_cases", []), f"{label} required_cases")
        auxiliary_claims(capability, label, set(required_cases))
        passing = evidence_claims(capability.get("evidence", []), label)
        if status not in CAPABILITY_STATUSES:
            errors.append(f"capability {capability_id}: invalid status {status!r}")
        if not patterns and not calls:
            errors.append(f"capability {capability_id}: needs patterns and/or calls")
        for pattern in strings(patterns, f"{label} patterns"):
            try:
                re.compile(pattern)
            except re.error as error:
                errors.append(f"capability {capability_id}: invalid pattern {pattern!r}: {error}")
        strings(calls, f"{label} calls")
        if passing - set(required_cases):
            errors.append(f"{label}: evidence claims undeclared contract cases")
        if status == "implemented" and (not required_cases or not set(required_cases) <= passing):
            errors.append(f"{label}: implemented requires evidence for every declared case")
        if status == "partial" and not passing and not capability.get("legacy_evidence"):
            errors.append(f"{label}: partial requires passing cases or explicitly legacy evidence")

    requirements = scenes_data.get("requirements", {})
    milestones = scenes_data.get("milestones", {})
    if not isinstance(requirements, dict) or not isinstance(milestones, dict):
        errors.append("scenes: requirements and milestones must be tables")
        return errors
    known_milestones = set(milestones)
    for kind, required in requirements.items():
        required = strings(required, f"requirements.{kind}")
        unknown = sorted(set(required) - known_milestones)
        if unknown:
            errors.append(f"requirements.{kind}: unknown milestones: {', '.join(unknown)}")

    tracked_files = {scene.get("file") for scene in scenes}
    for scene in scenes:
        filename = scene.get("file", "<unknown>")
        kind = scene.get("kind")
        if not isinstance(kind, str):
            errors.append(f"scene {filename}: kind must be a string")
            continue
        required = set(strings(requirements.get(kind, []), f"scene {filename} requirements"))
        label = f"scene {filename}"
        auxiliary_claims(scene, label, {f"scene:{filename}:{name}" for name in required})
        states = scene.get("milestone_status", {})
        if not isinstance(states, dict):
            errors.append(f"{label}: milestone_status must be a table")
            states = {}
        unknown = sorted(set(states) - required)
        if not required:
            errors.append(f"scene {filename}: kind {kind!r} has no milestone requirements")
        if unknown:
            errors.append(f"scene {filename}: non-required milestones: {', '.join(unknown)}")
        if required - states.keys():
            errors.append(f"{label}: missing per-milestone status/applicability records")
        for milestone, state in states.items():
            milestone_label = f"{label} milestone {milestone}"
            if not isinstance(state, dict):
                errors.append(f"{milestone_label}: expected a status record")
                continue
            if not isinstance(state.get("status"), str) or state["status"] not in {
                "missing",
                "passing",
                "not-applicable",
            }:
                errors.append(f"{milestone_label}: invalid status")
                continue
            if not isinstance(state.get("reason"), str) or not state["reason"]:
                errors.append(f"{milestone_label}: missing applicability reason")
            passing = evidence_claims(state.get("evidence", []), milestone_label)
            assertion_id = f"scene:{filename}:{milestone}"
            if state.get("status") in {"passing", "not-applicable"} and assertion_id not in passing:
                errors.append(f"{milestone_label}: completed milestone requires its own evidence")
        if "complete" in scene or "qualification" in scene:
            errors.append(
                f"{label}: replace legacy complete/qualification with milestone/platform records"
            )

    for scenario in scenarios:
        scenario_id = scenario.get("id", "<unknown>")
        status = scenario.get("status")
        label = f"scenario {scenario_id}"
        required = strings(scenario.get("required_assertions", []), f"{label} required_assertions")
        contracts = scenario.get("assertion_contracts", [])
        if not isinstance(contracts, list) or not all(isinstance(item, dict) for item in contracts):
            errors.append(f"{label}: assertion_contracts must be a list of objects")
        elif [item.get("id") for item in contracts] != required:
            errors.append(f"{label}: assertion contracts must match the declared order")
        else:
            for contract in contracts:
                if "expected" not in contract or contract.get("operator") not in ("eq", "le", "ge"):
                    errors.append(f"{label}: invalid assertion contract")
        auxiliary_claims(scenario, label, set(required), scenario)
        passing = evidence_claims(scenario.get("evidence", []), label, scenario=scenario)
        unknown_scenes = sorted(set(strings(scenario.get("scenes", []), label)) - tracked_files)
        if status not in SCENARIO_STATUSES:
            errors.append(f"scenario {scenario_id}: invalid status {status!r}")
        if type(scenario.get("version")) is not int or scenario["version"] < 1:
            errors.append(f"{label}: version must be a positive integer")
        if not isinstance(scenario.get("layer"), str) or scenario["layer"] not in {
            "user-journey",
            "engine-conformance",
            "scene-contract",
            "cross-cutting",
        }:
            errors.append(f"{label}: invalid scenario layer")
        if "qualification" in scenario:
            errors.append(f"{label}: replace legacy qualification with independent platforms")
        if unknown_scenes:
            errors.append(f"scenario {scenario_id}: unknown scenes: {', '.join(unknown_scenes)}")
        if passing - set(required):
            errors.append(f"{label}: evidence claims undeclared assertions")
        if status == "passing" and (not required or not set(required) <= passing):
            errors.append(f"scenario {scenario_id}: passing requires every declared assertion")
        deterministic = scenario.get("deterministic", False)
        if type(deterministic) is not bool:
            errors.append(f"{label}: deterministic must be a boolean")
        if deterministic is True and status == "passing":
            try:
                repeat = validate_comparison(scenario.get("repeat_evidence"), root)
                if repeat["scenario_id"] != scenario_id:
                    errors.append(f"{label}: repeat evidence has the wrong scenario")
                compared = {(item["path"], item["sha256"]) for item in repeat["runs"]}
                claims = {
                    (item.get("path"), item.get("sha256"))
                    for item in scenario.get("evidence", [])
                    if isinstance(item, dict)
                }
                if not claims or not claims <= compared:
                    errors.append(f"{label}: repeat evidence does not cover the claimed runs")
            except (CaptureError, EvidenceError, OSError) as error:
                errors.append(f"{label}: {error}")

    if inventory is not None:
        source_files = {item["name"] for item in inventory.get("files", [])}
        missing = sorted(source_files - tracked_files)
        extra = sorted(tracked_files - source_files)
        if missing:
            errors.append(f"scenes: untracked inventory files: {', '.join(missing)}")
        if extra:
            errors.append(f"scenes: files absent from inventory: {', '.join(extra)}")
        source_kind = {item["name"]: item["kind"] for item in inventory.get("files", [])}
        for scene in scenes:
            filename = scene.get("file")
            if filename in source_kind and scene.get("kind") != source_kind[filename]:
                errors.append(
                    f"scene {filename}: tracked kind {scene.get('kind')!r} does not match "
                    f"inventory kind {source_kind[filename]!r}"
                )

    return errors


def load_lingo(lingo_dir: Path, source_files: list[dict[str, Any]]) -> dict[str, str]:
    result: dict[str, str] = {}
    missing: list[str] = []
    for item in source_files:
        name = item["name"]
        path = lingo_dir / f"{name}.lingo"
        if not path.exists():
            missing.append(path.name)
            continue
        result[name] = path.read_text(encoding="utf-8", errors="replace")
    if missing:
        raise TrackerError(
            f"missing {len(missing)} Lingo dumps in {lingo_dir}: {', '.join(missing)}"
        )
    return result


def capability_metrics(capabilities: list[dict[str, Any]], lingo: dict[str, str]) -> dict[str, Any]:
    scanned_lingo = {
        name: "\n".join(strip_lingo_strings(line) for line in source.splitlines())
        for name, source in lingo.items()
    }
    rows: list[dict[str, Any]] = []
    known_calls: set[str] = set()
    for capability in capabilities:
        patterns = [re.compile(pattern) for pattern in capability.get("patterns", [])]
        calls = [str(name) for name in capability.get("calls", [])]
        known_calls.update(name.casefold() for name in calls)
        call_patterns = [re.compile(rf"\b{re.escape(name)}\s*\(", re.IGNORECASE) for name in calls]
        occurrences = 0
        files = 0
        for source in scanned_lingo.values():
            count = sum(len(pattern.findall(source)) for pattern in patterns)
            count += sum(len(pattern.findall(source)) for pattern in call_patterns)
            occurrences += count
            files += count > 0
        rows.append(
            {
                "id": capability["id"],
                "description": capability.get("description", ""),
                "status": capability["status"],
                "occurrences": occurrences,
                "files": files,
                "evidence": capability.get("evidence", []),
                "required_cases": len(capability.get("required_cases", [])),
                "passing_cases": len(
                    {
                        case
                        for reference in capability.get("evidence", [])
                        for case in reference.get("assertions", [])
                    }
                ),
                "reference_status": capability.get("reference", {}).get("status", "missing"),
                "platforms": sorted(capability.get("platforms", {})),
            }
        )

    handler_names: set[str] = set()
    all_calls: Counter[str] = Counter()
    for source in scanned_lingo.values():
        handler_names.update(match.casefold() for match in HANDLER_RE.findall(source))
        for line in source.splitlines():
            if HANDLER_RE.match(line):
                continue
            calls = CALL_RE.findall(line)
            all_calls.update(match.casefold() for match in calls)
    external_calls = Counter(
        {
            name: count
            for name, count in all_calls.items()
            if name not in handler_names and name not in CALL_SYNTAX_WORDS
        }
    )
    unknown_calls = Counter(
        {name: count for name, count in external_calls.items() if name not in known_calls}
    )

    observed = [row for row in rows if row["occurrences"] > 0]
    implemented = [row for row in observed if row["status"] == "implemented"]
    verified = [row for row in observed if row["reference_status"] == "matched"]
    occurrences = sum(row["occurrences"] for row in observed)
    implemented_occurrences = sum(row["occurrences"] for row in implemented)
    verified_occurrences = sum(row["occurrences"] for row in verified)
    return {
        "catalog": rows,
        "observed": len(observed),
        "implemented": len(implemented),
        "verified": len(verified),
        "status_counts": dict(Counter(row["status"] for row in observed)),
        "occurrences": occurrences,
        "implemented_occurrences": implemented_occurrences,
        "verified_occurrences": verified_occurrences,
        "external_call_sites": sum(external_calls.values()),
        "cataloged_external_call_sites": sum(external_calls.values()) - sum(unknown_calls.values()),
        "unknown_calls": [
            {"name": name, "occurrences": count} for name, count in unknown_calls.most_common()
        ],
    }


def strip_lingo_strings(line: str) -> str:
    """Blank quoted content so strings cannot look like function calls."""
    result: list[str] = []
    quoted = False
    index = 0
    while index < len(line):
        char = line[index]
        if char == '"':
            quoted = not quoted
            result.append(" ")
        elif not quoted and line[index : index + 2] == "--":
            break
        else:
            result.append(" " if quoted else char)
        index += 1
    return "".join(result)


def scene_metrics(scenes_data: dict[str, Any], inventory: dict[str, Any]) -> dict[str, Any]:
    requirements = scenes_data["requirements"]
    source_by_name = {item["name"]: item for item in inventory["files"]}
    rows: list[dict[str, Any]] = []
    total_required = 0
    total_complete = 0
    weighted_required = 0
    weighted_complete = 0
    for scene in scenes_data["scenes"]:
        required = requirements[scene["kind"]]
        complete = [
            name
            for name, state in scene.get("milestone_status", {}).items()
            if state["status"] in {"passing", "not-applicable"}
        ]
        scripts = source_by_name[scene["file"]]["scripts"]["count"]
        weight = max(scripts, 1)
        total_required += len(required)
        total_complete += len(complete)
        weighted_required += len(required) * weight
        weighted_complete += len(complete) * weight
        rows.append(
            {
                "file": scene["file"],
                "kind": scene["kind"],
                "role": scene.get("role", ""),
                "scripts": scripts,
                "complete": len(complete),
                "required": len(required),
                "platforms": sorted(scene.get("platforms", {})),
                "milestone_status": scene.get("milestone_status", {}),
            }
        )
    return {
        "rows": rows,
        "tracked": len(rows),
        "complete_scenes": sum(row["complete"] == row["required"] for row in rows),
        "completed_milestones": total_complete,
        "required_milestones": total_required,
        "milestone_ratio": ratio(total_complete, total_required),
        "script_weighted_ratio": ratio(weighted_complete, weighted_required),
        "platform_counts": dict(Counter(platform for row in rows for platform in row["platforms"])),
    }


def scenario_metrics(scenarios_data: dict[str, Any]) -> dict[str, Any]:
    rows = scenarios_data["scenarios"]
    return {
        "rows": rows,
        "total": len(rows),
        "passing": sum(row["status"] == "passing" for row in rows),
        "status_counts": dict(Counter(row["status"] for row in rows)),
        "platform_counts": dict(
            Counter(platform for row in rows for platform in row.get("platforms", {}))
        ),
        "layer_counts": dict(Counter(row["layer"] for row in rows)),
    }


def ratio(numerator: int, denominator: int) -> float:
    return numerator / denominator if denominator else 0.0


def percent(value: float) -> str:
    return f"{value * 100:.1f}%"


def build_report(
    capabilities_data: dict[str, Any],
    scenes_data: dict[str, Any],
    scenarios_data: dict[str, Any],
    inventory: dict[str, Any],
    lingo: dict[str, str],
    *,
    evidence_root: Path | None = None,
) -> dict[str, Any]:
    errors = validate_trackers(
        capabilities_data, scenes_data, scenarios_data, inventory, evidence_root=evidence_root
    )
    if errors:
        raise TrackerError("\n".join(errors))
    capabilities = capability_metrics(capabilities_data["capabilities"], lingo)
    scenes = scene_metrics(scenes_data, inventory)
    scenarios = scenario_metrics(scenarios_data)
    totals = inventory["totals"]
    return {
        "schema_version": TRACKER_SCHEMA_VERSION,
        "static_recovery": {
            "files": totals["files"],
            "movies": totals["movies"],
            "casts": totals["casts"],
            "scripts": totals["scripts"],
            "script_source_bytes": totals["scriptSourceBytes"],
            "tracked_files": scenes["tracked"],
        },
        "capabilities": capabilities,
        "scenes": scenes,
        "scenarios": scenarios,
    }


def markdown_report(report: dict[str, Any]) -> str:
    static = report["static_recovery"]
    capabilities = report["capabilities"]
    scenes = report["scenes"]
    scenarios = report["scenarios"]
    implemented_ratio = ratio(capabilities["implemented"], capabilities["observed"])
    verified_ratio = ratio(capabilities["verified"], capabilities["observed"])
    implemented_use_ratio = ratio(
        capabilities["implemented_occurrences"], capabilities["occurrences"]
    )
    verified_use_ratio = ratio(capabilities["verified_occurrences"], capabilities["occurrences"])
    catalog_ratio = ratio(
        capabilities["cataloged_external_call_sites"], capabilities["external_call_sites"]
    )

    lines = [
        "# Findus Workshop compatibility progress",
        "",
        (
            "> These measurements are independent. Static recovery is not runtime compatibility, "
            "and emulator qualification is not physical-hardware qualification."
        ),
        "",
        "## Summary",
        "",
        "| Dimension | Result |",
        "| --- | ---: |",
        f"| Static Director files tracked | {static['tracked_files']}/{static['files']} |",
        f"| Decompiled script blocks inventoried | {static['scripts']:,} |",
        (
            "| Observed capability families implemented | "
            f"{capabilities['implemented']}/{capabilities['observed']} "
            f"({percent(implemented_ratio)}) |"
        ),
        (
            "| Observed capability families reference-matched | "
            f"{capabilities['verified']}/{capabilities['observed']} "
            f"({percent(verified_ratio)}) |"
        ),
        (
            "| Capability occurrences covered by implemented families | "
            f"{capabilities['implemented_occurrences']:,}/{capabilities['occurrences']:,} "
            f"({percent(implemented_use_ratio)}) |"
        ),
        (
            "| Capability occurrences in reference-matched families | "
            f"{capabilities['verified_occurrences']:,}/{capabilities['occurrences']:,} "
            f"({percent(verified_use_ratio)}) |"
        ),
        (
            "| External-style call sites cataloged | "
            f"{capabilities['cataloged_external_call_sites']:,}/"
            f"{capabilities['external_call_sites']:,} ({percent(catalog_ratio)}) |"
        ),
        (
            "| Scene milestones complete | "
            f"{scenes['completed_milestones']}/{scenes['required_milestones']} "
            f"({percent(scenes['milestone_ratio'])}) |"
        ),
        f"| Script-weighted scene milestones | {percent(scenes['script_weighted_ratio'])} |",
        (
            "| Scenes with every milestone complete | "
            f"{scenes['complete_scenes']}/{scenes['tracked']} |"
        ),
        f"| End-to-end scenarios passing | {scenarios['passing']}/{scenarios['total']} |",
        "",
        "## Capability status",
        "",
        "| Capability | Status | Passing / declared cases | Occurrences | Files |",
        "| --- | --- | ---: | ---: | ---: |",
    ]
    for row in sorted(
        (item for item in capabilities["catalog"] if item["occurrences"]),
        key=lambda item: (-item["occurrences"], item["id"]),
    ):
        lines.append(
            f"| `{row['id']}` | {row['status']} | "
            f"{row['passing_cases']}/{row['required_cases']} | "
            f"{row['occurrences']:,} | {row['files']} |"
        )

    lines.extend(
        [
            "",
            "## Scene evidence gates",
            "",
            "Platform evidence is independent; no result implies another platform passed.",
            "",
            "| Platform | Scenes | Scenarios |",
            "| --- | ---: | ---: |",
        ]
    )
    for platform in PLATFORMS:
        lines.append(
            f"| {platform} | {scenes['platform_counts'].get(platform, 0)} | "
            f"{scenarios['platform_counts'].get(platform, 0)} |"
        )

    lines.extend(
        [
            "",
            "## End-to-end scenarios",
            "",
            "| Scenario | Status | Independently tested platforms |",
            "| --- | --- | --- |",
        ]
    )
    for row in scenarios["rows"]:
        platforms = ", ".join(sorted(row.get("platforms", {}))) or "none"
        lines.append(f"| `{row['id']}` | {row['status']} | {platforms} |")

    unknown_calls = capabilities["unknown_calls"]
    lines.extend(
        [
            "",
            "## Uncataloged external-style calls",
            "",
            (
                "These are lexical candidates, not proven Director APIs. Classify real runtime "
                "requirements in `compatibility/capabilities.toml`; exclude false positives only "
                "with a documented rule."
            ),
            "",
            "| Identifier | Occurrences |",
            "| --- | ---: |",
        ]
    )
    for item in unknown_calls[:25]:
        lines.append(f"| `{item['name']}` | {item['occurrences']:,} |")
    if not unknown_calls:
        lines.append("| _none_ | 0 |")
    lines.append("")
    return "\n".join(lines)


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("report", "validate"), nargs="?", default="report")
    parser.add_argument(
        "--inventory", type=Path, default=Path(f"{work_dir()}/analysis/inventory.json")
    )
    parser.add_argument("--lingo-dir", type=Path, default=Path(f"{work_dir()}/analysis/lingo"))
    parser.add_argument(
        "--capabilities", type=Path, default=Path("compatibility/capabilities.toml")
    )
    parser.add_argument("--scenes", type=Path, default=Path("compatibility/scenes.toml"))
    parser.add_argument("--scenarios", type=Path, default=Path("compatibility/scenarios.toml"))
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--evidence-root", type=Path, default=Path.cwd())
    parser.add_argument(
        "--local-evidence",
        type=Path,
        help="local run overlay; defaults to DIRECTOR64_LOCAL_EVIDENCE or ignored output",
    )
    parser.add_argument(
        "--no-local-evidence",
        action="store_true",
        help="validate/report checked-in catalog definitions only",
    )
    parser.add_argument(
        "--inventory-mode",
        choices=("required", "optional"),
        default="required",
        help="whether the ignored local Director inventory must be available",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        capabilities_data = load_toml(args.capabilities)
        scenes_data = load_toml(args.scenes)
        scenarios_data = load_toml(args.scenarios)
        inventory = load_inventory(args.inventory, required=args.inventory_mode == "required")
        baseline_errors = validate_trackers(
            capabilities_data,
            scenes_data,
            scenarios_data,
            inventory,
            evidence_root=args.evidence_root,
        )
        if baseline_errors:
            raise TrackerError("\n".join(f"- {error}" for error in baseline_errors))
        if not args.no_local_evidence:
            scenarios_data = apply_overlay(
                scenarios_data,
                read_overlay(overlay_path(args.evidence_root, args.local_evidence)),
                args.evidence_root,
            )
        errors = validate_trackers(
            capabilities_data,
            scenes_data,
            scenarios_data,
            inventory,
            evidence_root=args.evidence_root,
        )
        if errors:
            raise TrackerError("\n".join(f"- {error}" for error in errors))
        if args.command == "validate":
            suffix = " with source inventory" if inventory is not None else " without local source"
            print(f"compatibility trackers valid{suffix}")
            return 0
        if inventory is None:
            raise TrackerError("a report requires the local Director inventory")
        lingo = load_lingo(args.lingo_dir, inventory["files"])
        report = build_report(
            capabilities_data,
            scenes_data,
            scenarios_data,
            inventory,
            lingo,
            evidence_root=args.evidence_root,
        )
        rendered = (
            json.dumps(report, indent=2, sort_keys=True) + "\n"
            if args.format == "json"
            else markdown_report(report)
        )
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(rendered, encoding="utf-8")
        else:
            print(rendered, end="")
        return 0
    except (TrackerError, LocalEvidenceError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

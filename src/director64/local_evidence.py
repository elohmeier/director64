"""Validated local run overlays, separate from checked-in scenario definitions.

The ignored default is the selected source workspace’s analysis/local-evidence.json.
Override it with
DIRECTOR64_LOCAL_EVIDENCE or the caller's --local-evidence path. An absent overlay
leaves the baseline unchanged; a present but invalid overlay fails closed.
"""

from __future__ import annotations

import fcntl
import json
import os
import tempfile
from copy import deepcopy
from pathlib import Path
from typing import Any

from director64.evidence import assertion_passes, validate_reference
from director64.project import work_dir


class LocalEvidenceError(ValueError):
    """A local overlay cannot substantiate its scenario results."""


def overlay_path(root: Path, explicit: Path | None = None) -> Path:
    path = explicit or Path(
        os.environ.get("DIRECTOR64_LOCAL_EVIDENCE", f"{work_dir()}/analysis/local-evidence.json")
    )
    return path if path.is_absolute() else root / path


def read_overlay(path: Path) -> dict[str, Any] | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except (OSError, ValueError) as error:
        raise LocalEvidenceError(f"invalid local evidence overlay {path}: {error}") from error
    if not isinstance(data, dict):
        raise LocalEvidenceError("local evidence overlay must be an object")
    return data


def _keys(record: Any, allowed: set[str], label: str) -> None:
    if not isinstance(record, dict) or set(record) - allowed:
        raise LocalEvidenceError(f"{label}: unknown fields or invalid record shape")


def apply_overlay(
    catalog: dict[str, Any], overlay: dict[str, Any] | None, root: Path
) -> dict[str, Any]:
    """Derive local scenario status/platforms without changing definitions or denominators."""
    result = deepcopy(catalog)
    if overlay is None:
        return result
    _keys(overlay, {"schema_version", "catalog_version", "scenarios"}, "local overlay")
    if type(overlay.get("schema_version")) is not int or overlay["schema_version"] != 1:
        raise LocalEvidenceError("unsupported local overlay schema_version")
    if overlay.get("catalog_version") != catalog.get("catalog_version"):
        raise LocalEvidenceError("local overlay catalog_version mismatch")
    records = overlay.get("scenarios")
    if not isinstance(records, list):
        raise LocalEvidenceError("local overlay scenarios must be a list")
    scenarios = {scenario["id"]: scenario for scenario in result["scenarios"]}
    seen: set[str] = set()
    for item in records:
        _keys(
            item, {"scenario_id", "scenario_version", "runs", "repeat_evidence"}, "local scenario"
        )
        scenario_id = item.get("scenario_id")
        if not isinstance(scenario_id, str) or scenario_id not in scenarios:
            raise LocalEvidenceError("local overlay refers to an unknown scenario")
        if scenario_id in seen:
            raise LocalEvidenceError(f"duplicate local scenario: {scenario_id}")
        seen.add(scenario_id)
        scenario = scenarios[scenario_id]
        if type(item.get("scenario_version")) is not int or (
            item["scenario_version"] != scenario.get("version")
        ):
            raise LocalEvidenceError(f"{scenario_id}: local scenario_version mismatch")
        runs = item.get("runs")
        if not isinstance(runs, list) or not runs:
            raise LocalEvidenceError(f"{scenario_id}: local promotion requires pinned runs")
        required = scenario.get("required_assertions", [])
        if not required:
            raise LocalEvidenceError(f"{scenario_id}: no declared assertion contract to validate")
        passing: set[str] = set()
        references: set[tuple[str, str]] = set()
        platforms: dict[str, list[dict[str, Any]]] = {}
        for reference in runs:
            _keys(reference, {"path", "sha256", "assertions"}, "local run")
            validated = validate_reference(reference, root)
            if not validated.valid:
                raise LocalEvidenceError(f"{scenario_id}: {'; '.join(validated.errors)}")
            record = validated.record
            if (
                record["scenario_id"] != scenario_id
                or record["scenario_version"] != scenario["version"]
                or record["catalog_version"] != catalog["catalog_version"]
            ):
                raise LocalEvidenceError(f"{scenario_id}: run identity/version mismatch")
            contracts = [
                {key: assertion.get(key) for key in ("id", "operator", "expected")}
                for assertion in record["assertions"]
            ]
            if not assertion_passes(contracts, scenario.get("assertion_contracts", []), "eq"):
                raise LocalEvidenceError(
                    f"{scenario_id}: run changes the declared assertion contract"
                )
            assertions = reference.get("assertions")
            if (
                not isinstance(assertions, list)
                or not assertions
                or not all(isinstance(value, str) for value in assertions)
                or len(assertions) != len(set(assertions))
                or not set(assertions) <= set(validated.assertions)
                or not set(assertions) <= set(required)
            ):
                raise LocalEvidenceError(f"{scenario_id}: invalid selected assertion evidence")
            key = (reference["path"], reference["sha256"])
            if key in references:
                raise LocalEvidenceError(f"{scenario_id}: duplicate local run")
            references.add(key)
            passing.update(assertions)
            platforms.setdefault(record["platform"]["id"], []).append(deepcopy(reference))
        if passing != set(required):
            raise LocalEvidenceError(f"{scenario_id}: incomplete assertion coverage")
        if scenario.get("deterministic") is True or "repeat_evidence" in item:
            # Import locally because the capture adapter also registers these overlays.
            from director64.captures import validate_comparison

            try:
                compared = validate_comparison(item.get("repeat_evidence"), root)
            except (ValueError, OSError) as error:
                raise LocalEvidenceError(
                    f"{scenario_id}: invalid repeat evidence: {error}"
                ) from error
            compared_runs = {(ref["path"], ref["sha256"]) for ref in compared["runs"]}
            if compared["scenario_id"] != scenario_id or not references <= compared_runs:
                raise LocalEvidenceError(
                    f"{scenario_id}: repeat evidence does not cover local runs"
                )
            scenario["repeat_evidence"] = deepcopy(item["repeat_evidence"])
        scenario["status"] = "passing"
        scenario["evidence"] = deepcopy(runs)
        scenario["platforms"] = platforms
        scenario.pop("blocked_reason", None)
    return result


def update_overlay(
    catalog: dict[str, Any], item: dict[str, Any], root: Path, path: Path
) -> dict[str, Any]:
    """Validate an update, preserve other scenarios, and atomically replace only the local file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.with_suffix(path.suffix + ".lock").open("a", encoding="utf-8") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        overlay = read_overlay(path)
        if overlay is None:
            overlay = {
                "schema_version": 1,
                "catalog_version": catalog["catalog_version"],
                "scenarios": [],
            }
        # Validate shape before examining existing records. Invalid unrelated entries
        # are retained and rejected, never silently discarded by an update.
        _keys(overlay, {"schema_version", "catalog_version", "scenarios"}, "local overlay")
        if not isinstance(overlay.get("scenarios"), list) or not all(
            isinstance(record, dict) for record in overlay["scenarios"]
        ):
            raise LocalEvidenceError("local overlay scenarios must be a list of objects")
        overlay["scenarios"] = [
            record
            for record in overlay["scenarios"]
            if record.get("scenario_id") != item.get("scenario_id")
        ]
        overlay["scenarios"].append(deepcopy(item))
        overlay["scenarios"].sort(key=lambda record: str(record.get("scenario_id", "")))
        apply_overlay(catalog, overlay, root)
        temporary_path = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w", dir=path.parent, prefix=f".{path.name}.", delete=False, encoding="utf-8"
            ) as handle:
                temporary_path = Path(handle.name)
                json.dump(overlay, handle, indent=2, sort_keys=True, allow_nan=False)
                handle.write("\n")
                handle.flush()
                os.fsync(handle.fileno())
            temporary_path.replace(path)
        finally:
            if temporary_path is not None and temporary_path.exists():
                temporary_path.unlink()
        return overlay

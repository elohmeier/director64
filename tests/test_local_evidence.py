from __future__ import annotations

import json
from copy import deepcopy

import pytest

from director64.compatibility import main
from director64.evidence import sha256
from director64.local_evidence import (
    LocalEvidenceError,
    apply_overlay,
    overlay_path,
    read_overlay,
    update_overlay,
)
from director64.project import work_dir


@pytest.fixture
def local_fixture(evidence_factory, pin_evidence):
    record, events = evidence_factory()
    catalog = {
        "schema_version": 1,
        "catalog_version": "scenarios-v1",
        "scenarios": [
            {
                "id": "demo",
                "version": 1,
                "status": "blocked",
                "platforms": {},
                "evidence": [],
                "required_assertions": list(record["expected_assertions"]),
                "assertion_contracts": [
                    {key: assertion[key] for key in ("id", "operator", "expected")}
                    for assertion in record["assertions"]
                ],
            }
        ],
    }
    overlay = {
        "schema_version": 1,
        "catalog_version": "scenarios-v1",
        "scenarios": [
            {
                "scenario_id": "demo",
                "scenario_version": 1,
                "runs": [pin_evidence(record)],
            }
        ],
    }
    return catalog, overlay, record, events


def test_absent_local_evidence_preserves_clean_baseline(local_fixture, tmp_path):
    catalog, _, _, _ = local_fixture
    result = apply_overlay(catalog, read_overlay(tmp_path / "absent.json"), tmp_path)
    assert result == catalog
    assert result["scenarios"][0]["status"] == "blocked"


def test_valid_local_results_derive_status_and_platform_without_altering_catalog(
    local_fixture, tmp_path
):
    catalog, overlay, _, _ = local_fixture
    before = deepcopy(catalog)
    result = apply_overlay(catalog, overlay, tmp_path)
    assert catalog == before
    assert result["scenarios"][0]["status"] == "passing"
    assert set(result["scenarios"][0]["platforms"]) == {"host"}
    assert len(result["scenarios"]) == len(catalog["scenarios"])
    assert (
        result["scenarios"][0]["assertion_contracts"]
        == catalog["scenarios"][0]["assertion_contracts"]
    )


@pytest.mark.parametrize(
    "mutation",
    [
        "future-schema",
        "catalog-version",
        "scenario-version",
        "unknown-scenario",
        "duplicate-scenario",
        "change-status",
        "change-contract",
        "change-platform",
        "change-denominator",
        "missing-record",
        "changed-record-hash",
        "missing-artifact",
        "changed-artifact",
        "no-runs",
        "duplicate-run",
        "unknown-assertion",
        "malformed-assertion",
        "incomplete-assertions",
        "missing-repeat",
    ],
)
def test_overlay_mutations_cannot_promote_incomplete_evidence(local_fixture, tmp_path, mutation):
    catalog, overlay, record, _ = deepcopy(local_fixture)
    original = deepcopy(catalog)
    scenario = overlay["scenarios"][0]
    reference = scenario["runs"][0]
    if mutation == "future-schema":
        overlay["schema_version"] = 2
    elif mutation == "catalog-version":
        overlay["catalog_version"] = "scenarios-v2"
    elif mutation == "scenario-version":
        scenario["scenario_version"] = 2
    elif mutation == "unknown-scenario":
        scenario["scenario_id"] = "undeclared"
    elif mutation == "duplicate-scenario":
        overlay["scenarios"].append(deepcopy(scenario))
    elif mutation == "change-status":
        scenario["status"] = "passing"
    elif mutation == "change-contract":
        scenario["required_assertions"] = ["pointer"]
    elif mutation == "change-platform":
        scenario["platforms"] = {"original-n64": [reference]}
    elif mutation == "change-denominator":
        overlay["requirements"] = []
    elif mutation == "missing-record":
        reference["path"] = "missing.json"
    elif mutation == "changed-record-hash":
        reference["sha256"] = "0" * 64
    elif mutation == "missing-artifact":
        (tmp_path / record["artifacts"][0]["path"]).unlink()
    elif mutation == "changed-artifact":
        (tmp_path / record["artifacts"][0]["path"]).write_text("tampered")
    elif mutation == "no-runs":
        scenario["runs"] = []
    elif mutation == "duplicate-run":
        scenario["runs"].append(deepcopy(reference))
    elif mutation == "unknown-assertion":
        reference["assertions"].append("unexecuted")
    elif mutation == "malformed-assertion":
        reference["assertions"] = [{}]
    elif mutation == "incomplete-assertions":
        reference["assertions"].pop()
    elif mutation == "missing-repeat":
        catalog["scenarios"][0]["deterministic"] = True
        original = deepcopy(catalog)
    with pytest.raises(LocalEvidenceError):
        apply_overlay(catalog, overlay, tmp_path)
    assert catalog == original


def test_self_consistent_run_cannot_weaken_catalog_expectations(
    local_fixture, tmp_path, pin_evidence
):
    catalog, overlay, record, events = local_fixture
    record["assertions"][0].update(actual=0, expected=0)
    events[1]["actual"] = 0
    artifact = next(item for item in record["artifacts"] if item["role"] == "guest_events")
    path = tmp_path / artifact["path"]
    path.write_text("\n".join(json.dumps(event) for event in events))
    artifact["sha256"] = sha256(path)
    overlay["scenarios"][0]["runs"] = [pin_evidence(record)]
    with pytest.raises(LocalEvidenceError, match="changes the declared assertion contract"):
        apply_overlay(catalog, overlay, tmp_path)


def test_overlay_update_preserves_other_scenarios(local_fixture, tmp_path, pin_evidence):
    catalog, overlay, record, events = local_fixture
    path = tmp_path / "local-evidence.json"
    update_overlay(catalog, overlay["scenarios"][0], tmp_path, path)
    other_catalog = deepcopy(catalog["scenarios"][0])
    other_catalog["id"] = "other"
    catalog["scenarios"].append(other_catalog)
    other_record = deepcopy(record)
    other_record["scenario_id"] = "other"
    for event in events:
        event["scenario_id"] = "other"
    event_path = tmp_path / "other-events.jsonl"
    event_path.write_text("\n".join(json.dumps(event) for event in events))
    artifact = next(item for item in other_record["artifacts"] if item["role"] == "guest_events")
    artifact.update(path=event_path.name, sha256=sha256(event_path))
    other = {
        "scenario_id": "other",
        "scenario_version": 1,
        "runs": [pin_evidence(other_record, name="other-run.json")],
    }
    update_overlay(catalog, other, tmp_path, path)
    actual = read_overlay(path)
    assert [item["scenario_id"] for item in actual["scenarios"]] == ["demo", "other"]
    assert actual["scenarios"][0] == overlay["scenarios"][0]
    before = path.read_bytes()
    bad = deepcopy(other)
    bad["runs"][0]["sha256"] = "0" * 64
    with pytest.raises(LocalEvidenceError):
        update_overlay(catalog, bad, tmp_path, path)
    assert path.read_bytes() == before


def test_environment_path_and_explicit_override(tmp_path, monkeypatch):
    monkeypatch.delenv("DIRECTOR64_LOCAL_EVIDENCE", raising=False)
    assert overlay_path(tmp_path) == tmp_path / f"{work_dir()}/analysis/local-evidence.json"
    monkeypatch.setenv("DIRECTOR64_LOCAL_EVIDENCE", f"{work_dir()}/test-overlay.json")
    assert overlay_path(tmp_path) == tmp_path / f"{work_dir()}/test-overlay.json"
    assert overlay_path(tmp_path, tmp_path / "explicit.json") == tmp_path / "explicit.json"


def test_present_corrupt_overlay_fails_but_explicit_baseline_remains_available(tmp_path, capsys):
    path = tmp_path / "corrupt-overlay.json"
    path.write_text("{invalid json")
    args = [
        "validate",
        "--inventory-mode",
        "optional",
        "--inventory",
        str(tmp_path / "absent.json"),
        "--local-evidence",
        str(path),
    ]
    for name in ("capabilities", "scenes", "scenarios"):
        args += ["--" + name, f"games/findus-workshop/compatibility/{name}.toml"]
    assert main(args) == 2
    assert "invalid local evidence overlay" in capsys.readouterr().err
    assert main([*args, "--no-local-evidence"]) == 0

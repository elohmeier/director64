from __future__ import annotations

from copy import deepcopy

import pytest

from director64.compatibility import (
    TrackerError,
    build_report,
    capability_metrics,
    markdown_report,
    validate_trackers,
)


def tracker_fixture(reference):
    def selected(*ids):
        return [{**reference, "assertions": list(ids)}]

    capabilities = {
        "schema_version": 1,
        "catalog_version": "capabilities-v1",
        "capabilities": [
            {
                "id": "input.pointer",
                "description": "pointer",
                "status": "implemented",
                "patterns": [r"(?i)\bthe\s+mouseH\b"],
                "calls": ["rollOver"],
                "required_cases": ["pointer"],
                "evidence": selected("pointer"),
            },
            {
                "id": "audio.playback",
                "description": "audio",
                "status": "implemented",
                "calls": ["puppetSound"],
                "required_cases": ["audio"],
                "evidence": selected("audio"),
            },
        ],
    }
    scenes = {
        "schema_version": 1,
        "catalog_version": "scenes-v1",
        "milestones": {"logic": "logic", "reference": "reference"},
        "requirements": {"movie": ["logic", "reference"]},
        "scenes": [
            {
                "file": "DEMO.DXR",
                "kind": "movie",
                "role": "activity",
                "milestone_status": {
                    "logic": {
                        "status": "passing",
                        "reason": "Synthetic logic assertion evaluated",
                        "evidence": selected("scene:DEMO.DXR:logic"),
                    },
                    "reference": {"status": "missing", "reason": "No original reference"},
                },
                "platforms": {"host": selected("scene:DEMO.DXR:logic")},
            }
        ],
    }
    scenarios = {
        "schema_version": 1,
        "catalog_version": "scenarios-v1",
        "scenarios": [
            {
                "id": "demo",
                "description": "demo",
                "version": 1,
                "layer": "engine-conformance",
                "scenes": ["DEMO.DXR"],
                "status": "passing",
                "required_assertions": ["pointer", "audio", "scene:DEMO.DXR:logic"],
                "assertion_contracts": [
                    {"id": name, "operator": "eq", "expected": index}
                    for index, name in enumerate(["pointer", "audio", "scene:DEMO.DXR:logic"], 1)
                ],
                "evidence": [reference],
                "platforms": {"host": [reference]},
            }
        ],
    }
    inventory = {
        "totals": {"files": 1, "movies": 1, "casts": 0, "scripts": 2, "scriptSourceBytes": 100},
        "files": [{"name": "DEMO.DXR", "kind": "movie", "scripts": {"count": 2}}],
    }
    return capabilities, scenes, scenarios, inventory


@pytest.fixture
def trackers(evidence_factory, pin_evidence):
    record, _ = evidence_factory()
    return tracker_fixture(pin_evidence(record))


def test_validate_requires_real_evidence(trackers, tmp_path):
    assert validate_trackers(*trackers, evidence_root=tmp_path) == []
    trackers[0]["capabilities"][0]["evidence"] = []
    errors = validate_trackers(*trackers, evidence_root=tmp_path)
    assert "implemented requires evidence" in "\n".join(errors)


def test_capability_metrics_do_not_infer_reference_match(trackers):
    capabilities, _, _, _ = trackers
    lingo = {"DEMO.DXR": "on mouseUp\nset x to the mouseH\nrollOver(1)\npuppetSound(1)\nend"}
    result = capability_metrics(capabilities["capabilities"], lingo)
    assert result["observed"] == 2
    assert result["implemented"] == 2
    assert result["verified"] == 0
    assert result["occurrences"] == 3
    assert result["implemented_occurrences"] == 3
    assert result["verified_occurrences"] == 0
    assert result["unknown_calls"] == []


def test_report_tracks_milestones_and_independent_platforms(trackers, tmp_path):
    lingo = {"DEMO.DXR": "on mouseUp\nset x to the mouseH\npuppetSound(1)\nend\n"}
    report = build_report(*trackers, lingo, evidence_root=tmp_path)
    rendered = markdown_report(report)
    assert report["scenes"]["completed_milestones"] == 1
    assert report["scenes"]["required_milestones"] == 2
    assert report["scenes"]["milestone_ratio"] == 0.5
    assert report["scenarios"]["passing"] == 1
    assert report["scenarios"]["platform_counts"] == {"host": 1}
    assert "Static recovery is not runtime compatibility" in rendered
    assert "Highest gate" not in rendered


def test_inventory_and_scene_tracker_must_match(trackers, tmp_path):
    capabilities, scenes, scenarios, inventory = deepcopy(trackers)
    scenes["scenes"][0]["file"] = "OTHER.DXR"
    scenarios["scenarios"][0]["scenes"] = ["OTHER.DXR"]
    joined = "\n".join(
        validate_trackers(capabilities, scenes, scenarios, inventory, evidence_root=tmp_path)
    )
    assert "untracked inventory files: DEMO.DXR" in joined
    assert "files absent from inventory: OTHER.DXR" in joined


def test_call_catalog_ignores_identifiers_inside_strings(trackers):
    capabilities, _, _, _ = trackers
    result = capability_metrics(
        capabilities["capabilities"], {"DEMO.DXR": 'put "unknownCall(1)"\npuppetSound(1)\n'}
    )
    assert result["unknown_calls"] == []
    assert result["occurrences"] == 1


@pytest.mark.parametrize(
    "mutation",
    [
        "unknown-version",
        "missing-evidence",
        "changed-hash",
        "test-identifier",
        "missing-assertion",
        "wrong-scenario-version",
        "wrong-catalog-version",
        "platform-inference",
        "unrelated-platform",
        "shared-milestone-evidence",
        "missing-milestone",
        "unproven-noop",
        "unproven-reference",
        "undeclared-case",
        "legacy-promotion",
        "empty-contract",
        "duplicate-cases",
        "malformed-entry",
        "weakened-contract",
        "malformed-layer",
        "malformed-milestone",
        "malformed-kind",
        "malformed-scene-reference",
        "malformed-id",
        "malformed-requirements",
        "unproven-repeat",
    ],
)
def test_mutations_cannot_increase_reported_coverage(trackers, tmp_path, mutation):
    capabilities, scenes, scenarios, inventory = deepcopy(trackers)
    capability = capabilities["capabilities"][0]
    scene, scenario = scenes["scenes"][0], scenarios["scenarios"][0]
    reference = scenario["evidence"][0]
    if mutation == "unknown-version":
        capabilities["schema_version"] = 2
    elif mutation == "missing-evidence":
        reference["path"] = "missing.json"
    elif mutation == "changed-hash":
        reference["sha256"] = "0" * 64
    elif mutation == "test-identifier":
        scenario["evidence"] = ["tests/example"]
    elif mutation == "missing-assertion":
        scenario["required_assertions"].append("never-evaluated")
    elif mutation == "wrong-scenario-version":
        scenario["version"] = 2
    elif mutation == "wrong-catalog-version":
        scenarios["catalog_version"] = "scenarios-v2"
    elif mutation == "platform-inference":
        scenario["platforms"]["original-n64"] = scenario["platforms"].pop("host")
    elif mutation == "unrelated-platform":
        capability["platforms"] = {"host": [{**reference, "assertions": ["audio"]}]}
    elif mutation == "shared-milestone-evidence":
        scene["milestone_status"]["reference"] = deepcopy(scene["milestone_status"]["logic"])
    elif mutation == "missing-milestone":
        del scene["milestone_status"]["reference"]
    elif mutation == "unproven-noop":
        scene["milestone_status"]["reference"]["status"] = "not-applicable"
    elif mutation == "unproven-reference":
        capability["reference"] = {
            "status": "matched",
            "required_checks": ["pointer"],
            "evidence": capability["evidence"],
        }
    elif mutation == "undeclared-case":
        capability["required_cases"] = ["different"]
    elif mutation == "legacy-promotion":
        scenario["legacy_evidence"] = [reference["path"]]
        scenario["evidence"] = []
    elif mutation == "empty-contract":
        capability["required_cases"] = []
        capability["evidence"] = []
    elif mutation == "duplicate-cases":
        capability["required_cases"] *= 2
    elif mutation == "malformed-entry":
        capabilities["capabilities"].append("not-an-object")
    elif mutation == "weakened-contract":
        scenario["assertion_contracts"][0]["expected"] = 0
    elif mutation == "malformed-layer":
        scenario["layer"] = {}
    elif mutation == "malformed-milestone":
        scene["milestone_status"]["logic"]["status"] = {}
    elif mutation == "malformed-kind":
        scene["kind"] = {}
    elif mutation == "malformed-scene-reference":
        scenario["scenes"] = [{}]
    elif mutation == "malformed-id":
        scene["file"] = {}
    elif mutation == "malformed-requirements":
        scenes["requirements"] = []
    elif mutation == "unproven-repeat":
        scenario["deterministic"] = True
    with pytest.raises(TrackerError):
        build_report(capabilities, scenes, scenarios, inventory, {}, evidence_root=tmp_path)


def test_m64_and_original_n64_are_independent(tmp_path, evidence_factory, pin_evidence):
    record, _ = evidence_factory(platform="m64")
    reference = pin_evidence(record)
    capability, scenes, scenarios, inventory = tracker_fixture(reference)
    scenes["scenes"][0]["platforms"] = {
        "m64": [
            {
                **reference,
                "assertions": ["scene:DEMO.DXR:logic"],
            }
        ]
    }
    scenarios["scenarios"][0]["platforms"] = {"m64": [reference]}
    report = build_report(capability, scenes, scenarios, inventory, {}, evidence_root=tmp_path)
    assert report["scenarios"]["platform_counts"] == {"m64": 1}
    assert report["scenes"]["platform_counts"] == {"m64": 1}

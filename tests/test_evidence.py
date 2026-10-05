from __future__ import annotations

import json
import xml.etree.ElementTree as ET

import pytest

from director64.evidence import (
    assertion_passes,
    junit_report,
    main,
    sha256,
    validate_record,
    validate_reference,
)


def rewrite_events(record, events, root):
    artifact = next(item for item in record["artifacts"] if item["role"] == "guest_events")
    path = root / artifact["path"]
    path.write_text("\n".join(json.dumps(event) for event in events) + "\n")
    artifact["sha256"] = sha256(path)


def test_valid_evidence_derives_results(tmp_path, evidence_factory, pin_evidence):
    record, _ = evidence_factory()
    result = validate_reference(pin_evidence(record), tmp_path)
    assert result.errors == []
    assert result.assertions == record["expected_assertions"]
    assert result.as_dict()["status"] == "passing"
    assert ET.fromstring(junit_report(result)).attrib["failures"] == "0"


@pytest.mark.parametrize(
    "mutation",
    [
        "missing-schema",
        "future-schema",
        "legacy",
        "missing-rom",
        "artifact-tamper",
        "missing-file",
        "unhashed-artifact",
        "duplicate-artifact",
        "path-escape",
        "absolute-path",
        "no-assertions",
        "missing-result",
        "reordered-results",
        "wrong-actual",
        "lying-status",
        "skipped-result",
        "bad-operator",
        "wrong-checkpoint",
        "reused-checkpoint",
        "event-gap",
        "event-duplicate",
        "decreasing-tick",
        "mixed-run",
        "mixed-scenario",
        "missing-start",
        "missing-end",
        "premature-success",
        "failure",
        "crash",
        "audio-underrun",
        "timing-overrun",
        "unknown-event",
        "first-frame-before-load",
        "false-memory-ok",
        "unevaluated-checkpoint",
        "invalid-event-version",
        "boolean-actual",
        "boolean-tick",
        "malformed-event",
        "malformed-platform",
        "missing-provenance",
        "invalid-timestep",
        "unsupported-platform",
        "malformed-artifacts",
        "malformed-assertions",
        "duplicate-assertions",
    ],
)
def test_mutations_fail_closed(tmp_path, evidence_factory, mutation):
    record, events = evidence_factory()
    if mutation == "missing-schema":
        del record["schema_version"]
    elif mutation == "future-schema":
        record["schema_version"] = 2
    elif mutation == "legacy":
        record["legacy"] = True
    elif mutation == "missing-rom":
        record["artifacts"] = [item for item in record["artifacts"] if item["role"] != "rom"]
    elif mutation == "artifact-tamper":
        (tmp_path / record["artifacts"][0]["path"]).write_text("changed")
    elif mutation == "missing-file":
        record["artifacts"][0]["path"] = "absent"
    elif mutation == "unhashed-artifact":
        del record["artifacts"][0]["sha256"]
    elif mutation == "duplicate-artifact":
        record["artifacts"].append(record["artifacts"][0])
    elif mutation == "path-escape":
        record["artifacts"][0]["path"] = "../elsewhere"
    elif mutation == "absolute-path":
        record["artifacts"][0]["path"] = str(tmp_path / record["artifacts"][0]["path"])
    elif mutation == "no-assertions":
        record["expected_assertions"] = []
        record["assertions"] = []
    elif mutation == "missing-result":
        record["assertions"].pop()
    elif mutation == "reordered-results":
        record["assertions"].reverse()
    elif mutation == "wrong-actual":
        record["assertions"][0]["actual"] = 99
    elif mutation == "lying-status":
        record["assertions"][0]["expected"] = 99
    elif mutation == "skipped-result":
        record["assertions"][0]["status"] = "skipped"
    elif mutation == "bad-operator":
        record["assertions"][0]["operator"] = "truthy"
    elif mutation == "wrong-checkpoint":
        record["assertions"][0]["event_sequence"] = 0
    elif mutation == "reused-checkpoint":
        record["assertions"][1]["event_sequence"] = 1
    elif mutation == "event-gap":
        events[1]["sequence"] = 10
    elif mutation == "event-duplicate":
        events[1]["sequence"] = 0
    elif mutation == "decreasing-tick":
        events[2]["tick"] = 0
    elif mutation == "mixed-run":
        events[1]["run_id"] = "other-run"
    elif mutation == "mixed-scenario":
        events[1]["scenario_id"] = "other-scenario"
    elif mutation == "missing-start":
        events.pop(0)
    elif mutation == "missing-end":
        events.pop()
    elif mutation == "premature-success":
        events[0]["type"] = "SCENARIO_OK"
    elif mutation in {"failure", "crash", "audio-underrun", "timing-overrun"}:
        events[-1]["type"] = {
            "failure": "SCENARIO_FAIL",
            "crash": "CRASH",
            "audio-underrun": "AUDIO_UNDERRUN",
            "timing-overrun": "TIMING_OVERRUN",
        }[mutation]
    elif mutation == "unknown-event":
        events[0]["type"] = "MADE_UP_SUCCESS"
    elif mutation == "first-frame-before-load":
        events[0]["type"] = "SCENE_FIRST_FRAME"
        events[0]["scene_id"] = "unloaded"
    elif mutation == "false-memory-ok":
        events[0].update(type="MEMORY_OK", free_bytes=1, minimum_free_bytes=100)
    elif mutation == "unevaluated-checkpoint":
        record["expected_assertions"].pop()
        record["assertions"].pop()
    elif mutation == "invalid-event-version":
        events[1]["schema_version"] = 2
    elif mutation == "boolean-actual":
        events[1]["actual"] = True
    elif mutation == "boolean-tick":
        events[1]["tick"] = True
    elif mutation == "malformed-event":
        events[1] = []
    elif mutation == "malformed-platform":
        record["platform"] = []
    elif mutation == "missing-provenance":
        del record["toolchain"]["dirty"]
    elif mutation == "invalid-timestep":
        record["replay"]["timestep_denominator"] = 0
    elif mutation == "unsupported-platform":
        record["platform"]["id"] = "generic-n64"
    elif mutation == "malformed-artifacts":
        record["artifacts"].append("unhashed")
    elif mutation == "malformed-assertions":
        record["assertions"].append("passing")
    elif mutation == "duplicate-assertions":
        record["expected_assertions"].append(record["expected_assertions"][0])
    rewrite_events(record, events, tmp_path)
    result = validate_record(record, tmp_path)
    assert not result.valid, mutation
    assert result.assertions == [], mutation
    assert result.as_dict()["passing_assertions"] == []
    assert ET.fromstring(junit_report(result)).attrib["failures"] == "1"


def test_record_itself_is_hash_pinned(tmp_path, evidence_factory, pin_evidence):
    record, _ = evidence_factory()
    reference = pin_evidence(record)
    record["platform"]["id"] = "original-n64"
    (tmp_path / reference["path"]).write_text(json.dumps(record))
    assert not validate_reference(reference, tmp_path).valid


def test_symlink_cannot_escape_evidence_root(tmp_path, evidence_factory):
    record, _ = evidence_factory()
    outside = tmp_path.parent / "outside-fixture"
    outside.write_text("outside")
    link = tmp_path / "escape"
    link.symlink_to(outside)
    record["artifacts"][0].update(path="escape", sha256=sha256(outside))
    assert not validate_record(record, tmp_path).valid


def test_numeric_contracts_are_evaluated_without_bool_coercion():
    assert assertion_passes(100, 90, "ge")
    assert assertion_passes(0, 0, "le")
    assert not assertion_passes(True, 1, "eq")
    assert not assertion_passes(True, 1, "ge")
    assert not assertion_passes(float("nan"), 1, "le")
    assert not assertion_passes(float("nan"), float("nan"), "eq")
    assert not assertion_passes([True], [1], "eq")


def test_cli_emits_machine_readable_failure(tmp_path, capsys):
    record = tmp_path / "legacy.json"
    record.write_text('{"schema_version": 0}')
    assert main([str(record), "--root", str(tmp_path)]) == 2
    assert json.loads(capsys.readouterr().out)["status"] == "failed"

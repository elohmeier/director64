"""The two pinned MX 2004 JavaScript handlers lower to their recovered semantics."""

import copy

import pytest

from director64.project import GameSpec


def fixture(module):
    return {
        "handlers": [
            {
                "movie": "TASKSCRIPTS.CXT",
                "cast": "External",
                "member": 9,
                "name": "int2hex",
                "parameters": [],
                "source_sha256": module.INT2HEX,
                "body": [{"op": "unrecovered", "opcode": "unk26", "line": 30}],
            },
            {
                "movie": "ZMSCRIPTS.CXT",
                "cast": "External",
                "member": 58,
                "name": "cleargarbage",
                "parameters": [],
                "source_sha256": module.CLEARGARBAGE,
                "body": [{"op": "unrecovered", "opcode": "unk26", "line": 7405}],
            },
        ]
    }


@pytest.fixture
def module(monkeypatch):
    for key in ("DIRECTOR64_GAME", "DIRECTOR64_SOURCE", "DIRECTOR64_WORK_DIR"):
        monkeypatch.setenv(key, "")
    return GameSpec.load("lernerfolg-deutsch-1-2").host("compatibility")


def test_javascript_handlers_lower(module):
    program = fixture(module)
    untouched = copy.deepcopy(program)
    result = module.apply(program, module.SOURCE)
    int2hex = result["handlers"][0]
    assert int2hex["parameters"] == ["num"]
    assert int2hex["body"] == [
        {
            "op": "return",
            "line": 30,
            "value": ["call", "js_int2hex", [["variable", "num"]]],
        }
    ]
    cleargarbage = result["handlers"][1]
    assert cleargarbage["body"][0]["value"] == ["call", "js_cleargarbage", []]
    assert program == untouched
    assert [fix["id"] for fix in result["compatibility_fixes"]] == [
        "javascript-int2hex",
        "javascript-cleargarbage",
    ]
    assert all(
        not fix["original_projector_verified"] for fix in result["compatibility_fixes"]
    )


@pytest.mark.parametrize("changed", ["disc", "script", "body", "parameters", "missing"])
def test_source_drift_rejected(module, changed):
    program = fixture(module)
    source = module.SOURCE
    if changed == "disc":
        source = "0" * 64
    elif changed == "script":
        program["handlers"][0]["source_sha256"] = "0" * 64
    elif changed == "body":
        program["handlers"][0]["body"].append({"op": "return", "line": 31})
    elif changed == "parameters":
        program["handlers"][0]["parameters"] = ["existing"]
    elif changed == "missing":
        del program["handlers"][1]
    with pytest.raises(ValueError):
        module.apply(program, source)

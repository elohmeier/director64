"""Keep the selected BR correction separate from generic Lingo list semantics."""

import copy

import pytest

from director64.lingo import LingoError
from director64.project import GameSpec


def fixture(module):
    call = ["call", "getat", [["variable", "avstlist"], ["variable", "a"]]]
    return {
        "handlers": [
            {
                "movie": "BR.DXR",
                "cast": "Internal",
                "member": 4,
                "name": "ritabana",
                "source_sha256": module.SCRIPT,
                "body": [copy.deepcopy(call), copy.deepcopy(call)],
            }
        ]
    }


@pytest.fixture
def module(monkeypatch):
    for key in ("DIRECTOR64_GAME", "DIRECTOR64_SOURCE", "DIRECTOR64_WORK_DIR"):
        monkeypatch.setenv(key, "")
    return GameSpec.load("findus-mucklas").host("compatibility")


def test_pinned_spacing_correction(module):
    program = fixture(module)
    untouched = copy.deepcopy(program)
    extra = copy.deepcopy(program["handlers"][0])
    extra["movie"] = "OTHER.DXR"
    program["handlers"].append(extra)
    result = module.apply(program, module.SOURCE)
    assert result["handlers"][0]["body"][0][1] == "getaprop"
    assert result["handlers"][0]["body"][1][1] == "getaprop"
    assert result["handlers"][1] == extra
    assert program["handlers"][0] == untouched["handlers"][0]
    assert result["compatibility_fixes"][0]["calls_changed"] == 2
    assert not result["compatibility_fixes"][0]["original_projector_verified"]


@pytest.mark.parametrize("changed", ["disc", "script", "member", "missing", "duplicate", "sites"])
def test_source_drift_rejected(module, changed):
    program = fixture(module)
    source = module.SOURCE
    if changed == "disc":
        source = "0" * 64
    elif changed == "script":
        program["handlers"][0]["source_sha256"] = "0" * 64
    elif changed == "member":
        program["handlers"][0]["member"] = 5
    elif changed == "missing":
        program["handlers"].clear()
    elif changed == "duplicate":
        program["handlers"] *= 2
    elif changed == "sites":
        program["handlers"][0]["body"].pop()
    with pytest.raises(LingoError, match="changed"):
        module.apply(program, source)

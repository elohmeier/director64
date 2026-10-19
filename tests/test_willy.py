"""Source-free contracts for the selected D6 port's new syntax and data paths."""

import pytest

from director64.lingo import expression, parse_movie
from director64.project import GameSpec


def test_recovered_alert_fails_validation():
    game = GameSpec.load("willy-werkel-cars")
    alert = "00.CXT:460:findxtra:4869: expected string/symbol"
    state = {"error": "", "quit": False, "script_errors": 1, "last_script_error": alert}
    with pytest.raises(ValueError, match="findxtra"):
        game.host("full_journey").validate_state(state)
    state["script_errors"] = 0
    game.host("full_journey").validate_state(state)


def test_willy_profile():
    game = GameSpec.load("willy-werkel-cars")
    assert game.data["port"]["director_version"] == 600
    assert game.data["port"]["extended_d6"]
    assert game.source["kind"] == "zip"


def test_classic_cast_properties():
    assert expression('the text of member "db" of castLib "data"') == [
        "get",
        "text",
        ["reference", "member", ["string", "db"], ["string", "data"]],
    ]
    assert expression("the number of newMember") == ["get", "number", ["variable", "newmember"]]
    assert expression('the number of castMembers of castLib "data"') == [
        "get",
        "numberofmembers",
        ["call", "castlib", [["string", "data"]]],
    ]
    assert expression("the last char of name") == expression("the last char in name")


def test_legacy_identifier_bytes():
    handlers = parse_movie(
        "-- cast: Internal; member: 1; type: MovieScript; name: test\n"
        "on start\n  \x8cnejj()\nend\n",
        "MAIN.DXR",
    )
    assert handlers[0]["body"][0]["value"][1] == "\x8cnejj"


def test_decompiled_pi_call_keeps_local_identifier_distinct():
    assert expression("PI") == ["call", "pi", []]
    assert expression("PI()") == ["call", "pi", []]
    assert expression("pi") == ["variable", "pi"]
    assert expression('"PI"') == ["string", "PI"]

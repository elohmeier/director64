"""Source-free contracts for the selected D6 port's new syntax and data paths."""

import pytest

from director64.lingo import expression, parse_movie
from director64.project import GameSpec


def test_certificate_notice_requires_matching_source():
    compatibility = GameSpec.load("willy-werkel-cars").host("compatibility")
    program = {"handlers": [
        {"movie": "06.DXR", "member": 2, "name": "closewindow",
         "source_sha256": compatibility.WINDOW,
         "body": [{"op": "if", "yes": [
             {"op": "tell", "target": ["variable", "mymiaw"], "line": 1, "body": []}]}]},
        {"movie": "06.DXR", "member": 2, "name": "openwindow",
         "source_sha256": compatibility.OPENER, "body": [{}, {"line": 2}]},
        {"movie": "08.DXR", "member": 38, "name": "print",
         "source_sha256": compatibility.PRINT, "body": [{"op": "call", "line": 183}]},
    ]}
    result = compatibility.apply(program, compatibility.SOURCE)
    assert result["handlers"][2]["body"] == [{
        "op": "call", "line": 183, "value": ["call", "certificate_print_notice", []],
    }]
    assert program["handlers"][2]["body"] == [{"op": "call", "line": 183}]
    program["handlers"][2]["source_sha256"] = "changed"
    with pytest.raises(ValueError, match="certificate print source changed"):
        compatibility.apply(program, compatibility.SOURCE)


def test_recovered_alert_fails_validation():
    game = GameSpec.load("willy-werkel-cars")
    alert = "00.CXT:460:findxtra:4869: expected string/symbol"
    state = {"error": "", "quit": False, "script_errors": 1, "last_script_error": alert}
    with pytest.raises(ValueError, match="findxtra"):
        game.host("full_journey").validate_state(state)
    assert game.host("full_boot").FAILURE.search("DIRECTOR64 SCRIPT_ERROR " + alert)
    state["script_errors"] = 0
    game.host("full_journey").validate_state(state)


def test_controller_font_metrics_binary(tmp_path, monkeypatch):
    """Check mkfont's inclusive kerning indices and signed quantization."""
    import struct

    import pytest

    for key in ("DIRECTOR64_GAME", "DIRECTOR64_SOURCE", "DIRECTOR64_WORK_DIR"):
        monkeypatch.setenv(key, "")
    decode = GameSpec.load("willy-werkel-cars").host("text_metrics").decode
    ranges, glyphs, kranges, kern = 100, 112, 872, 1252
    data = bytearray(1261)
    data[:4] = b"FNT\x0b"
    for at, value in ((8, 24), (36, 1), (72, ranges), (80, glyphs),
                      (84, kranges), (92, kern)):
        struct.pack_into(">I", data, at, value)
    struct.pack_into(">IIi", data, ranges, 32, 95, 0)
    for i in range(95):
        data[glyphs + i * 8] = 5
    struct.pack_into(">HH", data, kranges + (87 - 32) * 4, 1, 2)
    struct.pack_into(">hb", data, kern + 3, 73 - 32, -10)
    struct.pack_into(">hb", data, kern + 6, 65 - 32, 7)
    assert decode(data) == {
        "size": 24, "advances": [5] * 95, "kerning": [[87, 73, -10], [87, 65, 7]]
    }
    struct.pack_into(">IIi", data, ranges, 33, 94, 0)
    with pytest.raises(ValueError, match="printable ASCII"):
        decode(data)
    with pytest.raises(ValueError, match="header"):
        decode(b"FNT\x0b")


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

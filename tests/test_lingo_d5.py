"""Classic Director 5 source forms retained without silently dropping work."""


from director64.aot import generate, native_blockers, normalize_handler
from director64.lingo import expression, parse_movie


def test_classic_script_constructor_and_member_number():
    assert expression('new(script "Control")') == [
        "call",
        "new",
        [["call", "script", [["string", "Control"]]]],
    ]
    assert expression('the number of member "Play"') == [
        "property",
        "number",
        "member",
        ["string", "Play"],
    ]
    assert expression('window "Dialog"') == ["call", "window", [["string", "Dialog"]]]


def test_legacy_symbols_preserve_source_bytes():
    assert expression("#\\") == ["symbol", "\\"]
    assert expression("#:") == ["symbol", ":"]
    assert expression("#s\x8age") == ["symbol", "s\x8age"]


def test_tell_retains_body_and_lowers_every_context(tmp_path):
    handlers = parse_movie(
        "-- cast: Internal; member: 1; type: CastScript; name: Door\n"
        'on mouseUp\n  tell the stage\n    go(1, "Outro")\n  end tell\nend\n',
        "DIALOG.DXR",
    )
    tell = handlers[0]["body"][0]
    assert tell["op"] == "tell"
    assert tell["target"] == ["the", "stage"]
    assert tell["body"][0]["value"][1] == "go"
    assert native_blockers(handlers[0]) == []
    lowered = normalize_handler(handlers[0])["body"]
    assert [node["value"][1] for node in lowered] == ["tell_stage", "go", "tell_end"]
    # Non-stage targets lower to the sprite timeline services: the target
    # evaluates once and unqualified sends retarget through sendSprite.
    handlers[0]["body"][0]["target"] = ["variable", "heldsprite"]
    handlers[0]["body"][0]["body"] = [
        {"op": "call", "line": 3, "value": ["call", "go", [["the", "lastframe"]]]},
        {"op": "call", "line": 4, "value": ["call", "starthelp", [["number", 2]]]},
    ]
    assert native_blockers(handlers[0]) == []
    lowered = normalize_handler(handlers[0])["body"]
    assert lowered[0]["op"] == "set" and lowered[0]["value"] == ["variable", "heldsprite"]
    channel = lowered[0]["target"]
    assert lowered[1]["value"] == [
        "call",
        "sprite_go",
        [channel, ["call", "sprite_lastframe", [channel]]],
    ]
    assert lowered[2]["value"] == [
        "call",
        "sendsprite",
        [channel, ["symbol", "starthelp"], ["number", 2]],
    ]
    generate({"handlers": handlers}, tmp_path)

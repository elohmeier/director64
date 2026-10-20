"""The Lingo parser (compiler/src/lingo.rs) on source forms the ports use."""

import pytest

from director64.lingo import LingoError, Statements, expression, parse_movie


def test_literals_and_precedence():
    assert expression('"C:\\data\\"') == ["string", "C:\\data\\"]
    assert expression("1 + 2 * 3")[3][1] == "*"
    assert expression('member 3 of castLib "External"')[3] == ["string", "External"]
    assert expression('member (3 + i) of castLib "External"') == expression(
        'member(3 + i, "External")'
    )
    assert expression("the castNum of sprite 3")[0] == "property"
    assert expression("char 3 to 4 of text")[0] == "chunk"
    assert expression("cast + 1")[2] == ["variable", "cast"]
    with pytest.raises(LingoError):
        expression("a ? b")


def test_reject_partial_handler():
    with pytest.raises(LingoError, match="truncated"):
        parse_movie(
            "-- cast: Internal; member: 1; type: MovieScript; name: test\non test\n", "fixture"
        )


def test_picture_name_symbols_keep_their_extension():
    # D5 HAMMERSPIEL uses these symbols as member names, including the period.
    assert expression("[#picture1.pct, #picture2.pct]") == [
        "list",
        [["symbol", "picture1.pct"], ["symbol", "picture2.pct"]],
    ]
    assert expression("string(#Picture.pct)") == ["call", "string", [["symbol", "Picture.pct"]]]
    assert expression("picture.name") == ["get", "name", ["variable", "picture"]]


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


def test_tell_retains_its_body():
    handlers = parse_movie(
        "-- cast: Internal; member: 1; type: CastScript; name: Door\n"
        'on mouseUp\n  tell the stage\n    go(1, "Outro")\n  end tell\nend\n',
        "DIALOG.DXR",
    )
    tell = handlers[0]["body"][0]
    assert tell["op"] == "tell"
    assert tell["target"] == ["the", "stage"]
    assert tell["body"][0]["value"][1] == "go"


def test_date_styles_and_optional_starts_with():
    assert expression("the long date") == ["the", "longdate"]
    assert expression("the short time") == ["the", "shorttime"]
    assert expression('"December" starts "Dec"') == expression('"December" starts with "Dec"')


def test_append_prepend_and_bare_return():
    source = "-- cast: Internal; member: 1; type: MovieScript; name: test\n"
    source += 'on test\n put "a" after text\n put "b" before text\n return\nend\n'
    body = parse_movie(source, "D7.DXR")[0]["body"]
    assert body[0]["value"] == ["binary", "&", ["variable", "text"], ["string", "a"]]
    assert body[1]["value"] == ["binary", "&", ["string", "b"], ["variable", "text"]]
    assert body[2]["op"] == "return"
    assert "value" not in body[2]


def test_chained_methods_properties_and_indexed_assignment():
    assert expression('script("actor").new(4).position.locH') == [
        "get",
        "loch",
        [
            "get",
            "position",
            ["method", "new", ["call", "script", [["string", "actor"]]], [["number", 4]]],
        ],
    ]
    assert expression("sprite(3).member.name") == [
        "get",
        "name",
        ["get", "member", ["reference", "sprite", ["number", 3], None]],
    ]
    assert expression("values[#actor][2]") == [
        "index",
        ["index", ["variable", "values"], ["symbol", "actor"]],
        ["number", 2],
    ]
    node = Statements([(1, "values[#actor].position = point(10, 20)")]).block()[0]
    assert node["op"] == "set"
    assert node["target"][0] == "get"
    node = Statements([(1, "state.ready = score >= 3 and tries < 8")]).block()[0]
    assert node["target"] == ["get", "ready", ["variable", "state"]]
    assert node["value"][:2] == ["binary", "and"]
    node = Statements([(1, "test(score = 3)")]).block()[0]
    assert node["op"] == "call"


def test_chunk_properties_and_nested_the_owners():
    assert expression("text.line[2..4]") == [
        "chunk",
        "line",
        ["number", 2],
        ["number", 4],
        ["variable", "text"],
    ]
    assert expression("text.char.count") == ["call", "count_chars", [["variable", "text"]]]
    assert expression("the last char in text") == ["last_chunk", "char", ["variable", "text"]]
    assert expression("the name of the member of sprite(3)") == [
        "get",
        "name",
        ["property", "member", "sprite", ["number", 3]],
    ]
    assert expression("the number of xtras") == ["the", "numberofxtras"]


def test_script_declarations_case_and_foreach_retain_scope():
    source = """-- cast: External; member: 7; type: MovieScript; name: actor
property position, mode
global result
on new me, items
  repeat with item in items
    case item.kind of
      #first, #second:
        result = item.position
      otherwise:
        result = VOID
    end case
  end repeat
  return me
end
-- cast: Internal; member: 7; type: MovieScript; name: other
on start
  result = 5
end
"""
    actor, other = parse_movie(source, "FIXTURE.DXR")
    assert actor["properties"] == ["position", "mode"]
    assert actor["body"][0]["names"] == ["result"]
    loop = actor["body"][1]
    assert loop["op"] == "foreach" and loop["variable"] == "item"
    case = loop["body"][0]
    assert case["branches"][0]["values"] == [["symbol", "first"], ["symbol", "second"]]
    assert case["branches"][1]["values"] is None
    assert other["properties"] == [] and other["script_globals"] == []


def test_local_named_return_is_an_assignment_not_a_return_statement():
    # Corroborated with BR's setlocal bytecode, rather than correcting the source.
    assert Statements([(1, "return = 0")]).block()[0] == {
        "line": 1,
        "op": "set",
        "target": ["variable", "return"],
        "value": ["number", 0],
    }


@pytest.mark.parametrize(
    "source",
    [
        "case value of\n#one:\ncall()",
        "repeat with item in things\ncall()",
        "case value of\n#one invalid:\nend case",
    ],
)
def test_incomplete_d8_control_flow_is_rejected(source):
    with pytest.raises(LingoError):
        Statements(list(enumerate(source.splitlines(), 1))).block()


def test_hilite_lowers_to_explicit_chunk_call():
    node = Statements([(1, 'hilite member("Fields", "Info").char[1.._count]')]).block()[0]
    assert node["op"] == "call"
    assert node["value"][:2] == ["call", "hilite_chunk"]
    chunk = node["value"][2][0]
    assert chunk[:2] == ["chunk", "char"]
    assert chunk[2] == ["number", 1]
    assert chunk[3] == ["variable", "_count"]


def test_abbr_date_joins_the_other_abbreviations():
    assert expression("the abbr date") == ["the", "abbrdate"]
    value = expression('the abbr date && "um" && the long time')
    assert value[:2] == ["binary", "&&"]
    assert value[3] == ["the", "longtime"]


def test_undecompiled_bytecode_marker_stays_explicit():
    source = (
        "-- cast: External; member: 9; type: MovieScript; name: (unnamed)\n"
        "on int2hex\n"
        "  -- unk26\n"
        "end\n"
    )
    handlers = parse_movie(source, "fixture")
    assert handlers[0]["body"] == [{"line": 3, "op": "unrecovered", "opcode": "unk26"}]


def test_ordinary_comments_are_still_not_statements():
    with pytest.raises(LingoError):
        Statements([(1, "-- authored comment")]).block()
    with pytest.raises(LingoError):
        Statements([(1, "-- unk26 trailing")]).block()


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


def test_legacy_sound_close_uses_the_stop_service():
    # LZ2 skips its publisher sound with this classic verb. ScummVM
    # 41ac2b31847622d0662d22c03fe6979e3b43cfbc b_sound also equates the verbs.
    node = Statements([(39, 'sound close 2')]).block()[0]
    assert node['value'] == ['call', 'sound_stop', [['number', 2]]]

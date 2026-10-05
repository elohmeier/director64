"""D10 recovery contracts; parsing is deliberately distinct from native execution."""

import pytest

from director64.aot import native_blockers
from director64.lingo import LingoError, Statements, expression, parse_movie


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
    assert native_blockers(handlers[0]) == []
    from director64.aot import normalize_handler

    lowered = normalize_handler(handlers[0])["body"]
    assert lowered == [
        {
            "op": "call",
            "line": 3,
            "value": ["call", "undecompiled_bytecode", [["string", "unk26"]]],
        }
    ]


def test_ordinary_comments_are_still_not_statements():
    with pytest.raises(LingoError):
        Statements([(1, "-- authored comment")]).block()
    with pytest.raises(LingoError):
        Statements([(1, "-- unk26 trailing")]).block()

"""D8 recovery contracts; parsing is deliberately distinct from native execution."""

import pytest

from director64.aot import generate
from director64.lingo import LingoError, Statements, expression, parse_movie


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


def test_script_declarations_case_and_foreach_retain_scope(tmp_path):
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
    result = generate({"handlers": [actor, other]}, tmp_path / "native")
    assert result["movies"][0]["handlers"] == 2
    # The declared properties are the handler entry's properties, not locals.
    (unit,) = (p for p in (tmp_path / "native").glob("*.c") if p.name != "symbols.c")
    source = unit.read_text()
    entry = source.split("static const lv_handler_t entries[]={")[1].split("},{")[0]
    locals_text, properties_text = entry.split("(const char *const[]){")[1:3]
    assert "position" not in locals_text and "mode" not in locals_text
    assert properties_text.startswith('"position","mode"}')



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

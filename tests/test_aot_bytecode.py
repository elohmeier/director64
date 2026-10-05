"""The compiler stage as the pipeline sees it: bytecode units, listings and
the handler index it writes for a parsed program."""

from pathlib import Path

import pytest

from director64.aot import LingoError, generate, text_hash
from director64.lingo import parse_movie


def compile_fixture(tmp_path: Path, source: str) -> tuple[str, str]:
    handlers = parse_movie(source, "FIXTURE.DXR")
    manifest = generate({"handlers": handlers}, tmp_path)
    assert manifest["movies"][0]["file"] == "fixture_dxr.c"
    return (tmp_path / "fixture_dxr.c").read_text(), (tmp_path / "fixture_dxr.lst").read_text()


def test_a_statement_is_one_state_with_its_line(tmp_path):
    _, listing = compile_fixture(
        tmp_path,
        """-- cast: Internal; member: 1; type: MovieScript; name: t
on start
  set x to 1 + 2
end
""",
    )
    # The assignment's state: line, operands, the op and a jump to the end
    # state, which returns VOID. Every state starts with its source line.
    assert "LINE 3\n" in listing
    assert "NUM8 1\n" in listing and "NUM8 2\n" in listing
    assert "BINARY +\n" in listing and "SET_LOCAL 0\n" in listing
    assert listing.count("RETURN_VOID") == 1


def test_expression_call_splits_the_state_and_keeps_the_result(tmp_path):
    _, listing = compile_fixture(
        tmp_path,
        """-- cast: Internal; member: 1; type: MovieScript; name: t
on start
  set x to twice(3) + 1
end
on twice n
  return n * 2
end
""",
    )
    names = [line.split()[1] for line in listing.splitlines() if line.strip()[:1].isdigit()]
    # twice lives in the calling script, so the call binds to its entry.
    assert "INVOKE_LOCAL_EXPR" in names
    # The continuation state pushes the call's result before the addition.
    after = names[names.index("INVOKE_LOCAL_EXPR") + 1 :]
    assert after[:2] == ["LINE", "RESULT"]


def test_listing_maps_offsets_to_mnemonics(tmp_path):
    _, listing = compile_fixture(
        tmp_path,
        """-- cast: Internal; member: 1; type: MovieScript; name: t
on start
  put "hi"
end
""",
    )
    rows = [line.split() for line in listing.splitlines() if line.strip()[:1].isdigit()]
    offsets = [int(row[0].rstrip(":")) for row in rows]
    assert offsets == sorted(offsets) and offsets[0] == 0
    assert "TEXT 0\n" in listing and "TRACE\n" in listing


def test_generated_movie_carries_its_handler_index(tmp_path):
    source, _ = compile_fixture(
        tmp_path,
        """-- cast: Internal; member: 1; type: MovieScript; name: t
on start
  set x to helper()
end
on helper
  return 1
end
""",
    )
    # Two handlers, two buckets: each entry sits in the bucket of its folded
    # name's hash, in table order, and the movie names the index.
    assert "static const uint16_t lx_handler_order[]={" in source
    assert "static const uint16_t lx_handler_buckets[]={" in source
    # The bound call pools no name, so the movie has no name or symbol tables.
    assert source.rstrip().endswith("lx_handler_order, lx_handler_buckets, 2};")
    buckets = [text_hash(n) & 1 for n in ("start", "helper")]
    order = sorted(range(2), key=lambda i: (buckets[i], i))
    assert f"lx_handler_order[]={{{','.join(map(str, order))}}};" in source


def test_text_hash_folds_case_like_the_runtime():
    # tests/native/test_lingo.c and the Rust crate assert the same constants.
    assert text_hash("exitframe") == text_hash("ExitFrame") == 945048422
    assert text_hash("") == 2166136261


def test_compile_errors_surface_as_lingo_errors(tmp_path):
    handlers = parse_movie(
        """-- cast: Internal; member: 1; type: MovieScript; name: t
on start
  set x to 1
end
""",
        "FIXTURE.DXR",
    )
    handlers[0]["body"][0]["value"] = ["nonsense"]
    with pytest.raises(LingoError):
        generate({"handlers": handlers}, tmp_path)


def test_calls_bind_to_the_movie_handler_the_lookup_would_reach(tmp_path):
    source, listing = compile_fixture(
        tmp_path,
        """-- cast: Internal; member: 1; type: MovieScript; name: t
on start
  helper(1)
  set x to helper(2)
  set y to new(#foo)
  do("helper 3")
  other(4)
end
on helper n
  return n
end
-- cast: Internal; member: 2; type: MovieScript; name: u
on other n
  return n
end
-- cast: Internal; member: 3; type: BehaviorScript; name: b
on mouseUp me
  helper(5)
  other(6)
end
""",
    )
    names = [line.split()[1:] for line in listing.splitlines() if line.strip()[:1].isdigit()]
    # start's helper calls bind to entry 1 (its own script), other() to
    # entry 2 (another movie script); do stays by name; new is the builtin
    # constructor and stays a by-name expression call; the behavior binds
    # both of its calls, since nothing in this corpus spells an ancestor.
    # (entry, argc, own): start's own script binds with own = 1, the
    # behavior's calls to movie scripts with own = 0.
    rows = [n[:4] for n in names]
    assert ["INVOKE_LOCAL", "1", "1", "1"] in rows
    assert ["INVOKE_LOCAL_EXPR", "1", "1", "1"] in rows
    assert ["INVOKE_LOCAL", "2", "1", "0"] in rows
    assert ["INVOKE_LOCAL", "1", "1", "0"] in rows  # mouseUp's helper(5)
    assert [n for n in names if n and n[0] == "INVOKE"] == [["INVOKE", "1", "1", "4"]]
    assert any(n[:1] == ["CALL"] and n[2] == "1" for n in names)  # new(#foo)
    manifest = (tmp_path / "manifest.json").read_text()
    assert '"local_calls": 5' in manifest and '"ancestors": false' in manifest


def test_statement_calls_to_builtins_skip_the_handler_search(tmp_path):
    _, listing = compile_fixture(
        tmp_path,
        """-- cast: Internal; member: 1; type: MovieScript; name: t
on start
  updateStage()
  set n to random(3)
  do("x")
end
""",
    )
    names = [line.split()[1] for line in listing.splitlines() if line.strip()[:1].isdigit()]
    assert "CALL_BUILTIN" in names and "CALL_BUILTIN_EXPR" in names
    assert "INVOKE" in names  # do keeps invoke's special path
    assert '"builtin_calls": 2' in (tmp_path / "manifest.json").read_text()

"""Classic forms found in the recovered Christmas calendar."""

from director64.lingo import expression, parse_movie


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

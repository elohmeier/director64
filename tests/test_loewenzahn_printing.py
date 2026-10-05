import copy
import importlib.util
from pathlib import Path

import pytest


def module(name):
    path = Path(__file__).parents[1] / "games/loewenzahn-1/host" / (name + ".py")
    spec = importlib.util.spec_from_file_location("loewenzahn_" + name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def test_print_adaptation_is_scoped_to_verified_source_method():
    compat = module("compatibility")
    handler = {
        "movie": "CURSOR.CXT",
        "cast": "External",
        "member": 90,
        "name": "drucken",
        "source_sha256": compat.SCRIPT,
        "parameters": ["me", "logobreite"],
        "line": 944,
        "body": [{"op": "original-print"}],
    }
    unrelated = handler | {"movie": "OTHER.CXT"}
    program = {"handlers": [handler, unrelated]}
    original = copy.deepcopy(program)
    result = compat.apply(program, compat.SOURCE)
    assert program == original
    assert result["handlers"][1] == unrelated
    assert result["handlers"][0]["body"][0]["value"][1] == "director64_print"
    assert result["compatibility_fixes"][0]["source_sha256"] == compat.SCRIPT
    for changed in ("source", "script", "ambiguous"):
        altered = copy.deepcopy(program)
        source = compat.SOURCE
        if changed == "source":
            source = "0" * 64
        elif changed == "script":
            altered["handlers"][0]["source_sha256"] = "0" * 64
        else:
            altered["handlers"].append(handler)
        with pytest.raises(ValueError, match="identity changed"):
            compat.apply(altered, source)


@pytest.mark.parametrize("data", [b"", bytes(512), bytes(656), bytes(2048)])
def test_print_pict_rejects_truncated_or_unrecognized_format(data):
    with pytest.raises(ValueError, match="PICT"):
        module("printing").decode_pict(data)

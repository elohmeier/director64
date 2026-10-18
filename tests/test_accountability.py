import copy
from types import SimpleNamespace

import pytest

from director64.accountability import build
from director64.aot import executable_program


def fixture():
    source = {"files": [{"name": "M", "members": [{"id": 9, "library_id": 1, "member_id": 2}],
                         "libraries": [{"library_id": 1, "name": "Internal"}],
                         "scripts": [{"id": 7, "member_id": 9, "content_sha256": "synthetic",
                                      "handlers": [{"id": 5, "name": "StartMovie"}]}]}]}
    program = {"handlers": [{"movie": "M", "cast": "internal", "member": 2, "name": "startmovie"}]}
    generated = {"movies": [{"movie": "M", "handlers": 1, "file": "m.c"}]}
    return source, program, generated


def test_shared_accountability_rejects_lost_or_duplicated_handlers():
    source, program, generated = fixture()
    assert build(source, program, generated)["counts"]["scripts"] == 1
    missing = copy.deepcopy(program)
    missing["handlers"][0]["member"] = 3
    with pytest.raises(ValueError, match="source mapping mismatch"):
        build(source, missing, generated)
    generated["movies"][0]["handlers"] = 2
    with pytest.raises(ValueError, match="handler count mismatch"):
        build(source, program, generated)


def test_native_port_uses_shared_rust_compatibility_without_host_adapter(tmp_path, monkeypatch):
    import director64.aot as aot
    (tmp_path / "aot").mkdir()
    game = SimpleNamespace(root=tmp_path, work=tmp_path, directory=tmp_path / "game",
                           slug="loewenzahn-7", source={"sha256": "source"},
                           data={"port": {"native_compatibility": True}})
    monkeypatch.setattr(aot, "generator_command", lambda root: ["compiler"])
    calls = []
    def run(command, **kwargs):
        calls.append(command)
        return SimpleNamespace(stdout='{"handlers":[],"compatibility_fixes":["shared"]}')
    monkeypatch.setattr(aot.subprocess, "run", run)
    result = executable_program(game)
    assert '"shared"' in result.read_text()
    assert calls[0][:3] == ["compiler", "director", "port-program"]
    assert calls[0][-4:] == ["loewenzahn-7", "source", "-", "compat"]

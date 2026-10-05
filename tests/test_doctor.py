import pytest

from director64 import doctor
from director64.toolchain import RECIPE_LABEL, ToolchainError


@pytest.fixture
def prerequisites(tmp_path, monkeypatch):
    (tmp_path / "mise.toml").write_text('[tools]\npython = "3.14.6"\n')
    monkeypatch.setattr(doctor.platform, "python_version", lambda: "3.14.6")
    monkeypatch.setattr(doctor.shutil, "which", lambda command: f"/tools/{command}")
    monkeypatch.setenv("CC", "cc")
    monkeypatch.setenv("CXX", "c++")
    monkeypatch.setattr(doctor, "source_revision", lambda root: "sdk-revision")
    monkeypatch.setattr(
        doctor, "inspect_image",
        lambda *args: {"Id": "image-id", "Config": {"Labels": {RECIPE_LABEL: "recipe-hash"}}},
    )
    return tmp_path


def test_doctor_reports_image_and_recipe_without_a_game(prerequisites, capsys):
    assert doctor.main(prerequisites) == 0
    output = capsys.readouterr().out
    assert "sdk-revision" in output
    assert "image-id" in output
    assert "recipe-hash" in output


@pytest.mark.parametrize(("missing", "status"), [("cc", 1), ("gopher64", 0)])
def test_doctor_distinguishes_required_and_optional_tools(
    prerequisites, monkeypatch, missing, status
):
    monkeypatch.setattr(
        doctor.shutil, "which", lambda command: None if command == missing else f"/tools/{command}"
    )
    assert doctor.main(prerequisites) == status


def test_doctor_reports_incompatible_image_without_rebuilding(prerequisites, monkeypatch, capsys):
    def stale(*args):
        raise ToolchainError("toolchain image differs from build recipe")

    monkeypatch.setattr(doctor, "inspect_image", stale)
    assert doctor.main(prerequisites) == 1
    assert "mise run toolchain" in capsys.readouterr().out

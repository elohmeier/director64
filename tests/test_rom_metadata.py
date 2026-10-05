import subprocess
from dataclasses import replace

import pytest

from director64.project import GameSpec
from director64.rom import RomError, metadata_fields
from director64.rom_metadata import prepare


@pytest.fixture
def metadata_game(tmp_path):
    original = GameSpec.load("findus-workshop")
    game = replace(original, root=tmp_path)
    for name in ("games/findus-workshop/metadata/metadata.ini", "pyproject.toml"):
        target = tmp_path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((original.root / name).read_bytes())
    (tmp_path / ".gitignore").write_text("/build/\n")
    subprocess.run(["git", "init", "-q", "-b", "main", str(tmp_path)], check=True)
    subprocess.run(["git", "-C", str(tmp_path), "add", "."], check=True)
    subprocess.run(
        [
            "git",
            "-C",
            str(tmp_path),
            "-c",
            "user.name=Test",
            "-c",
            "user.email=test@example.com",
            "commit",
            "-qm",
            "Synthetic metadata fixture",
        ],
        check=True,
    )
    return game


def test_metadata_is_stable_and_records_build_identity(metadata_game):
    game = metadata_game
    path = prepare(game, "release")
    raw = path.read_bytes()
    stamp = path.stat().st_mtime_ns
    revision = subprocess.check_output(["git", "-C", str(game.root), "rev-parse", "HEAD"])
    assert revision.strip() in raw
    assert game.source["sha256"].encode() in raw
    fields = metadata_fields(raw, title="Findus Workshop", controller_count=4)
    assert fields["short-desc"].startswith("v0.1.0+" + revision.decode()[:12] + " - ")
    assert "8 MiB" in fields["short-desc"]
    assert fields["release-date"] == "2026-09-06"
    assert prepare(game, "release").read_bytes() == raw
    assert path.stat().st_mtime_ns == stamp


def test_template_edits_refresh_metadata_without_asset_recovery(metadata_game):
    path = prepare(metadata_game, "release")
    old = path.read_bytes()
    template = metadata_game.directory / "metadata/metadata.ini"
    template.write_text(template.read_text().replace("2026-09-06", "2026-09-07"))
    assert prepare(metadata_game, "release") == path
    assert path.read_bytes() != old
    fields = metadata_fields(path.read_bytes(), title="Findus Workshop", controller_count=4)
    assert fields["release-date"] == "2026-09-07"
    assert "-dirty" in fields["short-desc"]


def test_probe_and_untracked_changes_are_visible(metadata_game):
    (metadata_game.root / "untracked.c").write_text("// unfinished change\n")
    path = prepare(metadata_game, "probe")
    fields = metadata_fields(path.read_bytes(), title="Findus Workshop", controller_count=4)
    assert "-dirty-probe - " in fields["short-desc"]
    assert path.parent.name == "probe"


def test_build_prefix_cannot_exceed_menu_description_limit(metadata_game):
    template = metadata_game.directory / "metadata/metadata.ini"
    template.write_text(
        template.read_text().replace("German workshop adventure. Requires 8 MiB RAM.", "x" * 120)
    )
    with pytest.raises(RomError, match="120-byte"):
        prepare(metadata_game, "release")

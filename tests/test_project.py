from dataclasses import replace
from pathlib import Path

import pytest

from director64.project import GameSpec


def test_games_and_source_workspaces_are_disjoint():
    workshop = GameSpec.load("findus-workshop")
    garden = GameSpec.load("findus-garden")
    assert workshop.work != garden.work
    assert workshop.media.parent != garden.media.parent
    assert workshop.dist != garden.dist
    edition = replace(workshop, source={**workshop.source, "id": "sha256-0123456789ab"})
    assert edition.work != workshop.work
    with pytest.raises(ValueError, match="catalogued"):
        garden.require_port()


@pytest.mark.parametrize("slug", ["../findus-workshop", "Findus", "a/b", "--bad"])
def test_manifest_traversal_rejected(slug):
    with pytest.raises(ValueError, match="slug"):
        GameSpec.load(slug)


def test_unknown_edition_fails_closed():
    with pytest.raises(ValueError, match="unknown source"):
        GameSpec.load("findus-workshop", "sha256-0123456789ab")


def test_all_catalogued_sources_have_distinct_full_hashes():
    root = GameSpec.load("findus-workshop").root
    games = [GameSpec.load(p.parent.name) for p in root.glob("games/*/game.toml")]
    assert len(games) == 20
    assert len({g.source["sha256"] for g in games}) == 20
    assert all(g.media.is_relative_to(root / "media" / g.slug) for g in games)
    assert all(Path(g.source["file"]).name == g.source["file"] for g in games)


def alternate_root(tmp_path: Path, **alternate) -> Path:
    game = tmp_path / "games/disc"
    game.mkdir(parents=True)
    game.joinpath("game.toml").write_text(
        f"""schema_version = 1
slug = "disc"
title = "Disc"
status = "catalogued"
default_source = "sha256-{"a" * 12}"

[[sources]]
id = "sha256-{"a" * 12}"
file = "DISC.iso"
sha256 = "{"a" * 64}"
bytes = 2048
kind = "iso9660"

[[sources.alternates]]
"""
        + "".join(f"{key} = {value!r}\n".replace("'", '"') for key, value in alternate.items())
    )
    return tmp_path


def test_an_alternate_rip_is_found_and_keeps_the_edition(tmp_path: Path):
    root = alternate_root(tmp_path, file="CD01.img", sha256="b" * 64, bytes=2352)
    game = GameSpec.load("disc", root=root)
    assert game.image["file"] == "DISC.iso"
    directory = root / "media/disc" / game.source["id"]
    directory.mkdir(parents=True)
    (directory / "CD01.img").write_bytes(b"")
    assert game.image["sha256"] == "b" * 64
    assert game.media == directory / "CD01.img"
    assert game.source["sha256"] == "a" * 64
    (directory / "DISC.iso").write_bytes(b"")
    assert game.media == directory / "DISC.iso"


@pytest.mark.parametrize(
    ("alternate", "message"),
    [
        ({"file": "../CD01.img", "sha256": "b" * 64, "bytes": 1}, "basename"),
        ({"file": "CD01.img", "sha256": "a" * 64, "bytes": 1}, "duplicate"),
        ({"file": "disc.ISO", "sha256": "b" * 64, "bytes": 1}, "distinct"),
        ({"file": "CD01.img", "sha256": "b" * 63, "bytes": 1}, "SHA-256"),
    ],
)
def test_alternate_rips_are_validated(tmp_path: Path, alternate, message):
    with pytest.raises(ValueError, match=message):
        GameSpec.load("disc", root=alternate_root(tmp_path, **alternate))

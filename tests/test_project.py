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
    assert len(games) == 8
    assert len({g.source["sha256"] for g in games}) == 8
    assert all(g.media.is_relative_to(root / "media" / g.slug) for g in games)
    assert all(Path(g.source["file"]).name == g.source["file"] for g in games)

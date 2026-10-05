from dataclasses import replace

import pytest

from director64.project import GameSpec
from director64.rom_inputs import record, verify


def fixture_game(tmp_path):
    game = replace(GameSpec.load("findus-mucklas"), root=tmp_path)
    for name in [
        "runtime/lingo/lingo_runtime.c",
        "runtime/lingo/lingo_runtime.h",
        "runtime/director/d6.inc",
        "platforms/n64/Makefile",
        "platforms/n64/director_main.c",
    ]:
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("synthetic " + name)
    (game.directory / "runtime").mkdir(parents=True)
    (game.directory / "runtime/adapter.c").write_text("synthetic adapter")
    game.work.mkdir(parents=True)
    (game.work / "build-inputs.json").write_text("{}")
    rom = game.work / "n64/findus-mucklas-probe.z64"
    rom.parent.mkdir(parents=True)
    rom.write_bytes(b"rom")
    return game, rom


def test_a_rom_verifies_against_the_sources_it_was_built_from(tmp_path):
    game, rom = fixture_game(tmp_path)
    record(game, "probe", rom)
    verify(game, "probe", rom)


@pytest.mark.parametrize(
    "changed",
    [
        "runtime/lingo/lingo_runtime.h",
        "runtime/director/d6.inc",
        "platforms/n64/director_main.c",
        "games/findus-mucklas/runtime/adapter.c",
    ],
)
def test_a_source_change_after_the_build_makes_the_rom_stale(tmp_path, changed):
    game, rom = fixture_game(tmp_path)
    record(game, "probe", rom)
    (tmp_path / changed).write_text("edited")
    with pytest.raises(ValueError, match="stale.*" + changed):
        verify(game, "probe", rom)


def test_regenerated_assets_make_the_rom_stale(tmp_path):
    game, rom = fixture_game(tmp_path)
    record(game, "probe", rom)
    (game.work / "build-inputs.json").write_text('{"generated": {}}')
    with pytest.raises(ValueError, match="stale"):
        verify(game, "probe", rom)


def test_a_new_source_file_makes_the_rom_stale(tmp_path):
    game, rom = fixture_game(tmp_path)
    record(game, "probe", rom)
    (tmp_path / "runtime/lingo/new_helper.c").write_text("added")
    with pytest.raises(ValueError, match="stale.*new_helper.c"):
        verify(game, "probe", rom)


def test_a_rom_the_build_did_not_produce_is_rejected(tmp_path):
    game, rom = fixture_game(tmp_path)
    record(game, "probe", rom)
    rom.write_bytes(b"another rom")
    with pytest.raises(ValueError, match="not the ROM"):
        verify(game, "probe", rom)


def test_an_unrecorded_rom_names_the_command_that_builds_it(tmp_path):
    game, rom = fixture_game(tmp_path)
    with pytest.raises(ValueError, match="director64 probe --game findus-mucklas"):
        verify(game, "probe", rom)

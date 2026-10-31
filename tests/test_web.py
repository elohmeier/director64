"""The web and Android runtime builds: game profile header, flags and site packs."""

from __future__ import annotations

import fcntl
import json
import struct
import zlib
from dataclasses import replace

import pytest

from director64 import web
from director64.project import GameSpec, repository


def test_profile_header_carries_the_player_table():
    header = web.profile_header(GameSpec.load("findus-circus"))
    assert '#define DIRECTOR64_ENTRY_MOVIE "PF2"' in header
    assert '#define GAME_SAVES "score.dat", "pf2.ini"' in header
    assert '#define GAME_MOVIE_PATH "C:\\134PF2\\134"' in header
    assert "#define GAME_KEY_FIELD_FRAMES 105, 114, 175, 184" in header
    assert "GAME_SAVES" not in web.profile_header(GameSpec.load("loewenzahn-1"))


def test_full_engine_keeps_source_family_and_exact_stream_directories():
    flags = web.family_defines(GameSpec.load("loewenzahn-3"))
    assert "-DDIRECTOR64_DIRECTOR_VERSION=6" in flags
    assert "-DDIRECTOR64_FULL_ENGINE=1" in flags
    assert "-DDIRECTOR64_STREAM_PATHS=1" in flags
    assert "-DDIRECTOR64_D85=1" not in flags
    header = web.profile_header(GameSpec.load("loewenzahn-4"))
    path = web.c_string("C:\\DATA\\SLIDE\\BRUECK\\")
    assert f'{{"BRUECK", {path}}}' in header
    assert "#define GAME_STREAM_ROOT" in header
    assert "-DDIRECTOR64_COLOR_DEPTH=8" in web.family_defines(GameSpec.load("findus-garden"))
    assert "-DDIRECTOR64_SOURCE_CURSOR_MASK=12u" in web.game_defines(GameSpec.load("loewenzahn-1"))


def test_unknown_player_keys_are_refused():
    game = GameSpec.load("findus-workshop")
    game = replace(game, data={**game.data, "player": {"save_files": []}})
    with pytest.raises(ValueError, match="unknown \\[player\\] keys"):
        web.player(game)


@pytest.mark.parametrize("size", [False, 65535, 16777217, 65536.0, "65536"])
def test_save_capacity_rejects_invalid_values(size):
    game = GameSpec.load("findus-workshop")
    game = replace(game, data={**game.data, "player": {"save_slot_bytes": size}})
    with pytest.raises(ValueError, match="save_slot_bytes must be an integer"):
        web.profile_header(game)


def test_painting_profiles_have_large_binary_cast_storage_only_where_required():
    for slug in ("loewenzahn-6", "loewenzahn-7", "loewenzahn-advent"):
        game = GameSpec.load(slug)
        header = web.profile_header(game)
        assert "#define DIRECTOR64_SAVE_SLOT_BYTES 16777216u" in header
        assert "-DDIRECTOR64_SAVE_SLOT_BYTES=16777216u" in web.game_defines(game)
        assert "-sINITIAL_MEMORY=112MB" in web.compile_command(game, game.work / "web")
        for name in ("images.cst|images", "temp.cst|temp", "library.cst|library"):
            assert name in web.player(game)["saves"]
    assert "tempimages.cst|tempimages" in web.player(GameSpec.load("loewenzahn-7"))["saves"]
    for slug in ("findus-birthday", "loewenzahn-8"):
        header = web.profile_header(GameSpec.load(slug))
        assert "#define DIRECTOR64_SAVE_SLOT_BYTES 65536u" in header


def test_runtime_is_built_for_the_family_switches_and_package():
    willy = GameSpec.load("willy-werkel-cars")
    assert web.family_defines(willy) == ["-DDIRECTOR64_DIRECTOR_VERSION=6",
                                         "-DDIRECTOR64_EXTENDED_D6=1",
                                         "-DDIRECTOR64_FULL_ENGINE=1",
                                         "-DDIRECTOR64_MEDIA_ROOT=Willy1"]
    assert web.game_defines(willy) == ["-DDIRECTOR64_WILLY=1", "-DDIRECTOR64_GAME_PROBE=1"]
    command = web.compile_command(willy, willy.work / "web")
    assert command[0] == "emcc"
    for source in ("runtime/storage/saves.c", "runtime/package/package.c",
                   "games/willy-werkel-cars/runtime/willy_input.c"):
        assert source in command
    exports = next(a for a in command if a.startswith("-sEXPORTED_FUNCTIONS="))
    assert "_d64_load_package" in exports and "_d64_rpc" in exports
    assert "_d64_step_pad," in exports and "_d64_native_cursor," in exports
    assert "_d64_pointer_player" not in exports and "_d64_render_pads" not in exports
    assert not any("interaction/pointer.c" in item for item in command)


def test_site_packs_keep_images_compressed(tmp_path):
    raw = b"pixels" * 50
    deflate = zlib.compressobj(9, zlib.DEFLATED, -15)
    stored = b"D64Z" + struct.pack(">I", len(raw)) + deflate.compress(raw) + deflate.flush()
    source, assets = tmp_path / "game", tmp_path / "assets"
    source.mkdir()
    assets.mkdir()
    (source / "images.bin").write_bytes(b"D64W" + stored)
    (source / "images.json").write_text(json.dumps({"a.fdi": [4, len(stored)], "w.fdi": [0, 4]}))
    web.write_pack(source, "images", assets)
    assert json.loads((assets / "images.json").read_text()) == {
        "w.fdi": [0, 4], "a.fdi": [4, len(stored)]}
    assert (assets / "images.bin").read_bytes() == b"D64W" + stored
    with pytest.raises(ValueError, match="length"):
        web.inflated(b"D64Z" + struct.pack(">I", 1) + stored[8:])


def test_notices_carry_every_license_text_they_cite():
    root = repository()
    text = web.notices(root, "header\n")
    assert text.startswith("header\n") and "MIT License" in text
    for path in (root / "licenses").glob("*.txt"):
        assert path.read_text(encoding="utf-8").splitlines()[0] in text


def test_runtime_receipt_detects_header_changes_during_compile(tmp_path, monkeypatch):
    game = replace(GameSpec.load("findus-birthday"), root=tmp_path)
    header = tmp_path / "runtime/director/fixture.h"
    header.parent.mkdir(parents=True)
    header.write_text("before")
    monkeypatch.setattr(web, "profile_header", lambda game: "synthetic profile")
    monkeypatch.setattr(web, "compile_command", lambda game, output: ["compiler"])
    monkeypatch.setattr(web, "emscripten", lambda *args: header.write_text("after"))
    output = tmp_path / "build/runtime"
    with pytest.raises(ValueError, match="inputs changed"):
        web.build_runtime(game, output, "unused")
    assert not (output / "runtime-inputs.sha256").exists()
    monkeypatch.setattr(web, "emscripten", lambda *args: None)
    web.build_runtime(game, output, "unused")
    receipt = (output / "runtime-inputs.sha256").read_text().strip()
    assert receipt == web.runtime_inputs(game)
    header.write_text("next change")
    assert receipt != web.runtime_inputs(game)


def test_web_build_holds_conversion_lock_until_shared_assets_are_consumed(tmp_path, monkeypatch):
    from director64 import android

    game = replace(GameSpec.load("findus-birthday"), root=tmp_path)
    observed = []

    def held(stage):
        with ((game.work / ".conversion.lock").open("a+") as contender,
              pytest.raises(BlockingIOError)):
            fcntl.flock(contender, fcntl.LOCK_EX | fcntl.LOCK_NB)
        observed.append(stage)

    def convert(*args):
        held("conversion")
        return game.work / "converted"

    def runtime(*args):
        held("compilation")
        return []

    def site(*args):
        held("asset-copy")
        return game.dist / "web"

    monkeypatch.setattr(android, "convert", convert)
    monkeypatch.setattr(web, "container_engine", lambda: "unused")
    monkeypatch.setattr(web, "build_runtime", runtime)
    monkeypatch.setattr(web, "build_site", site)
    assert web.build(game) == game.dist / "web"
    assert observed == ["conversion", "compilation", "asset-copy"]
    with (game.work / ".conversion.lock").open("a+") as contender:
        fcntl.flock(contender, fcntl.LOCK_EX | fcntl.LOCK_NB)

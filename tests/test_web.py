"""The web and Android runtime builds: game profile header, flags and site packs."""

from __future__ import annotations

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


def test_unknown_player_keys_are_refused():
    game = GameSpec.load("findus-workshop")
    game = replace(game, data={**game.data, "player": {"save_files": []}})
    with pytest.raises(ValueError, match="unknown \\[player\\] keys"):
        web.player(game)


def test_runtime_is_built_for_the_family_switches_and_package():
    willy = GameSpec.load("willy-werkel-cars")
    assert web.family_defines(willy) == ["-DDIRECTOR64_DIRECTOR_VERSION=6",
                                         "-DDIRECTOR64_EXTENDED_D6=1"]
    assert web.game_defines(willy) == ["-DDIRECTOR64_WILLY=1", "-DDIRECTOR64_GAME_PROBE=1"]
    command = web.compile_command(willy, willy.work / "web")
    assert command[0] == "emcc"
    for source in ("runtime/storage/saves.c", "runtime/package/package.c",
                   "games/willy-werkel-cars/runtime/willy_input.c"):
        assert source in command
    exports = next(a for a in command if a.startswith("-sEXPORTED_FUNCTIONS="))
    assert "_d64_load_package" in exports and "_d64_rpc" in exports


def test_site_packs_store_deflated_images_inflated(tmp_path):
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
        "w.fdi": [0, 4], "a.fdi": [4, len(raw)]}
    assert (assets / "images.bin").read_bytes() == b"D64W" + raw
    with pytest.raises(ValueError, match="length"):
        web.inflated(b"D64Z" + struct.pack(">I", 1) + stored[8:])


def test_notices_carry_every_license_text_they_cite():
    root = repository()
    text = web.notices(root, "header\n")
    assert text.startswith("header\n") and "MIT License" in text
    for path in (root / "licenses").glob("*.txt"):
        assert path.read_text(encoding="utf-8").splitlines()[0] in text

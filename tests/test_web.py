"""Browser player build contracts, on synthetic trees: no source media."""

from __future__ import annotations

import json
import re
import shutil
from dataclasses import replace

import pytest

from director64 import web
from director64.project import GameSpec, repository


def test_workshop_profile_names_the_archive_files():
    game = GameSpec.load("findus-workshop")
    profile = web.web_profile(game)
    assert profile["save_files"][0] == "vakt1.txt" and len(profile["save_files"]) == 9
    header = web.profile_header(game, profile)
    assert '#define DIRECTOR64_ENTRY_MOVIE "START"' in header
    assert '"vakt1.txt", "vakt2.txt"' in header and header.count('"') == 20


def test_a_game_without_saves_declares_it():
    profile = web.web_profile(GameSpec.load("loewenzahn-1"))
    assert profile["saves"] is False and profile["save_files"] == []


def test_the_runtime_is_built_for_the_ports_family_and_switches():
    willy = GameSpec.load("willy-werkel-cars")
    assert web.family_defines(willy) == [
        "-DDIRECTOR64_DIRECTOR_VERSION=6",
        "-DDIRECTOR64_EXTENDED_D6=1",
    ]
    assert web.game_defines(willy) == ["-DDIRECTOR64_WILLY=1", "-DDIRECTOR64_GAME_PROBE=1"]
    assert web.web_profile(willy)["save_files"] == ["data.cxt"]
    workshop = GameSpec.load("findus-workshop")
    assert web.family_defines(workshop)[1] == "-DDIRECTOR64_EXTENDED_D6=0"
    assert web.game_defines(workshop) == []


def test_invalid_profiles_are_refused(tmp_path):
    root = tmp_path / "root"
    (root / "games/findus-workshop").mkdir(parents=True)
    shutil.copyfile(
        repository() / "games/findus-workshop/game.toml", root / "games/findus-workshop/game.toml"
    )
    game = replace(GameSpec.load("findus-workshop"), root=root)
    with pytest.raises(ValueError, match="no browser profile"):
        web.web_profile(game)
    for body in (
        "schema_version = 2\nsave_files = ['a.txt']\n",
        "schema_version = 1\nsave_files = []\n",
        "schema_version = 1\nsaves = false\nsave_files = ['a.txt']\n",
        "schema_version = 1\nsave_files = ['a\"b']\n",
    ):
        (root / "games/findus-workshop/web.toml").write_text(body)
        with pytest.raises(ValueError, match="invalid browser profile"):
            web.web_profile(game)


def test_packs_index_every_asset_and_hash_names_with_contents(tmp_path):
    (tmp_path / "a.fdi").write_bytes(b"alpha")
    (tmp_path / "b.fdi").write_bytes(b"be")
    entry = web.pack({"b.fdi": tmp_path / "b.fdi", "a.fdi": tmp_path / "a.fdi"}, tmp_path / "x.bin")
    assert entry["index"] == {"a.fdi": [0, 5], "b.fdi": [5, 2]}
    assert (tmp_path / "x.bin").read_bytes() == b"alphabe"
    assert entry["bytes"] == 7 and entry["count"] == 2
    renamed = web.pack(
        {"c.fdi": tmp_path / "a.fdi", "b.fdi": tmp_path / "b.fdi"}, tmp_path / "y.bin"
    )
    assert renamed["sha256"] != entry["sha256"]


def test_sounds_are_served_under_their_cast_names(tmp_path):
    root = tmp_path / "root"
    game = replace(GameSpec.load("findus-workshop"), root=root)
    (game.work / "director/images").mkdir(parents=True)
    (game.work / "director/wav").mkdir(parents=True)
    (game.work / "director/images/1.fdi").write_bytes(b"i")
    (game.work / "director/wav/abc.wav").write_bytes(b"w")
    images, audio = web.asset_files(game)
    assert list(images) == ["1.fdi"] and list(audio) == ["abc.wav64"]


def test_package_runtime_links_the_engine_and_adapter_but_no_game_data():
    game = GameSpec.load("findus-workshop")
    command = web.compile_command(game, game.work / "web")
    assert "-DDIRECTOR64_PACKAGE" in command and "runtime/package/package.c" in command
    assert not any("/aot/c" in a or "_scene.c" in a or "registry.c" in a for a in command)
    exports = next(a for a in command if a.startswith("-sEXPORTED_FUNCTIONS="))
    assert "_d64_load_package" in exports


def test_abi_digest_covers_the_vocabulary_and_opcodes():
    import hashlib

    root = repository()
    expected = hashlib.sha256(
        (root / "runtime/lingo/names.txt").read_bytes()
        + (root / "runtime/lingo/lingo_bytecode.h").read_bytes()
    ).digest()[:16]
    assert web.abi_digest(root) == expected
    header = web.profile_header(GameSpec.load("findus-workshop"), {"save_files": ["a.txt"]})
    assert "#define DIRECTOR64_ABI {0x" + expected[:1].hex() in header


def test_compile_links_the_shared_engine_generated_tables_and_adapter():
    game = GameSpec.load("findus-workshop")
    command = web.compile_command(game, game.work / "web", linked=True)
    assert command[0] == "emcc" and "-Werror" in command
    for source in (
        "platforms/web/web_runtime.c",
        "runtime/director/director.c",
        "runtime/lingo/lingo_runtime.c",
        "games/findus-workshop/runtime/archive.c",
    ):
        assert source in command
    assert "games/findus-workshop/runtime/director_replay.c" not in command
    assert "-DDIRECTOR64_DIRECTOR_VERSION=6" in command
    # A single-threaded module: Pages cannot send cross-origin isolation headers.
    assert not any("PTHREAD" in a or "SHARED_MEMORY" in a for a in command)
    exports = next(a for a in command if a.startswith("-sEXPORTED_FUNCTIONS="))
    assert "_d64_rpc" in exports and "_d64_step" in exports
    # The page's text box: which field a press opens, and the commit.
    assert "_d64_text_field" in exports and "_d64_edit_text" in exports


def test_probe_wrapper_runs_the_harness_over_the_site(tmp_path):
    game = replace(GameSpec.load("findus-workshop"), root=repository())
    probe = web.write_probe(game, tmp_path)
    text = probe.read_text()
    assert text.startswith("#!/bin/sh\n") and "platforms/web/node/probe.mjs" in text
    assert str(tmp_path / "site") in text and probe.stat().st_mode & 0o111


def test_toolchain_is_pinned_by_digest():
    assert "@sha256:" in web.EMSDK_IMAGE and len(web.EMSDK_IMAGE.split("@sha256:")[1]) == 64


def test_site_files_exist():
    for name in web.SITE_FILES:
        assert (repository() / "platforms/web/site" / name).is_file()
    assert (repository() / web.FONT).is_file()
    json.dumps(web.EXPORTS)


def test_profiles_carry_identity_and_policy_but_no_disc_content():
    game = GameSpec.load("findus-workshop")
    entry = web.profile_entry(game, web.web_profile(game))
    assert entry["source"]["sha256"] == game.source["sha256"]
    assert entry["source"]["bytes"] == game.source["bytes"]
    assert entry["port"]["media_root"] == "POFMEDIA" and entry["save_files"][0] == "vakt1.txt"
    text = json.dumps(entry)
    assert "build/" not in text and "media/" not in text


def test_the_development_mount_serves_local_builds_outside_the_site(tmp_path):
    local = tmp_path / "local"
    (local / "assets").mkdir(parents=True)
    handler = type("H", (web.Handler,), {"local": {"game": local}})
    probe = handler.__new__(handler)
    probe.directory = str(tmp_path / "site")
    assert probe.translate_path("/local/game/assets/images.bin") == str(local / "assets/images.bin")
    assert probe.translate_path("/local/game/../../etc/passwd") == str(local / "etc/passwd")
    assert probe.translate_path("/app.js").startswith(str(tmp_path / "site"))


def test_notices_carry_the_license_and_every_text_they_cite():
    root = repository()
    text = web.notices(root, "header\n")
    assert text.startswith("header\n") and "MIT License" in text
    cited = set(
        re.findall(
            r"\(licenses/([\w.-]+\.txt)\)",
            (root / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8"),
        )
    )
    assert cited
    shipped = {p.name for p in (root / "licenses").glob("*.txt")}
    assert cited <= shipped
    for name in shipped:
        assert (root / "licenses" / name).read_text(encoding="utf-8").splitlines()[0] in text

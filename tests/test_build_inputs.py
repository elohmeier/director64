from dataclasses import replace

import pytest

from director64.build_inputs import begin, identity, record, verify
from director64.project import GameSpec


def fixture_game(tmp_path):
    original = GameSpec.load("findus-workshop")
    game = replace(original, root=tmp_path)
    # A minimal synthetic checkout uses the same contract with independent bytes.
    from director64.iso import file_sha256

    game.media.parent.mkdir(parents=True)
    game.media.write_bytes(b"synthetic disc")
    game = replace(game, source={**game.source, "sha256": file_sha256(game.media), "bytes": 14})
    for name in ["game.toml", "host/source-policy.json"]:
        path = game.directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("synthetic")
    for name in [
        "package-lock.json",
        "config/provenance.toml",
        "runtime/director/director.h",
        "runtime/director/cursor.h",
        "runtime/lingo/lingo_runtime.h",
        "runtime/lingo/names.txt",
        "runtime/lingo/lingo_names.h",
        "runtime/lingo/lingo_bytecode.h",
        *[
            "src/director64/" + name
            for name in (
                "lingo.py",
                "aot.py",
                "director.py",
                "iso.py",
                "full_assets.py",
                "image_pack.py",
                "build_inputs.py",
                "accountability.py",
                "cli.py",
                "video.py",
            )
        ],
    ]:
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("synthetic")
    for name in [
        "aot/program.json",
        "aot/c/manifest.json",
        "aot/c/movie.c",
        "director/c/movie_scene.c",
        "director/model.json",
        "director/packed.json",
    ]:
        path = game.work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("synthetic")
    return game


@pytest.mark.parametrize(
    "changed",
    [
        "src/director64/aot.py",
        "runtime/director/cursor.h",
        "runtime/lingo/names.txt",
    ],
)
def test_cache_rejects_changed_compiler_and_outputs(tmp_path, changed):
    game = fixture_game(tmp_path)
    record(game)
    verify(game)
    compiler = tmp_path / changed
    compiler.write_text("changed")
    with pytest.raises(ValueError, match="inputs changed"):
        verify(game)
    compiler.write_text("synthetic")
    (game.work / "aot/c/movie.c").write_text("changed")
    with pytest.raises(ValueError, match="generated inputs changed"):
        verify(game)


def test_cache_rejects_same_size_source_replacement(tmp_path):
    game = fixture_game(tmp_path)
    record(game)
    game.media.write_bytes(b"different disc")
    with pytest.raises(ValueError, match="source container hash"):
        verify(game)


def test_cache_rejects_changed_game_compatibility(tmp_path):
    game = fixture_game(tmp_path)
    game = replace(game, data={**game.data, "port": {"native_compatibility": True}})
    correction = game.directory / "host/compatibility.py"
    correction.write_text("original correction")
    record(game)
    verify(game)
    correction.write_text("changed correction")
    with pytest.raises(ValueError, match="inputs changed"):
        verify(game)


def test_shared_rust_compatibility_receipt_tracks_absent_host_override(tmp_path):
    game = fixture_game(tmp_path)
    game = replace(game, data={**game.data, "port": {"native_compatibility": True}})
    record(game)
    verify(game)
    (game.directory / "host/compatibility.py").write_text("new host override")
    with pytest.raises(ValueError, match="inputs changed"):
        verify(game)


def test_cache_rejects_changed_companion_url(tmp_path):
    game = fixture_game(tmp_path)
    game = replace(game, data={**game.data, "port": {"asset_inputs": ["host/printing.toml"]}})
    config = game.directory / "host/printing.toml"
    config.write_text('base_url = "http://example.test/old/"')
    record(game)
    verify(game)
    config.write_text('base_url = "http://example.test/new/"')
    with pytest.raises(ValueError, match="inputs changed"):
        verify(game)


def test_cache_tracks_optional_local_override(tmp_path):
    game = fixture_game(tmp_path)
    port = {
        "asset_inputs": ["host/printing.toml"],
        "optional_asset_inputs": ["host/printing.local.toml"],
    }
    game = replace(game, data={**game.data, "port": port})
    (game.directory / "host/printing.toml").write_text('base_url = "https://print.invalid/"')
    local = game.directory / "host/printing.local.toml"
    record(game)
    verify(game)
    local.write_text('base_url = "http://example.test/mine/"')
    with pytest.raises(ValueError, match="inputs changed"):
        verify(game)
    record(game)
    local.unlink()
    with pytest.raises(ValueError, match="inputs changed"):
        verify(game)


def test_failed_asset_build_invalidates_previous_receipt(tmp_path):
    game = fixture_game(tmp_path)
    record(game)
    begin(game)
    with pytest.raises(ValueError, match="missing build inputs receipt"):
        verify(game)


def test_asset_build_rejects_inputs_changed_during_build(tmp_path):
    game = fixture_game(tmp_path)
    inputs = begin(game)
    (tmp_path / "src/director64/aot.py").write_text("changed during asset packing")
    with pytest.raises(ValueError, match="inputs changed during asset build"):
        record(game, inputs)
    assert not (game.work / "build-inputs.json").exists()


def test_stable_asset_build_records_verified_inputs(tmp_path):
    game = fixture_game(tmp_path)
    inputs = begin(game)
    record(game, inputs)
    verify(game)


def test_receipt_covers_actual_package_abi_inputs():
    from director64 import web

    game = GameSpec.load("findus-workshop")
    inputs = identity(game)["inputs"]
    assert len(web.abi_digest(game.root)) == 16
    assert inputs["runtime/lingo/names.txt"]
    assert inputs["runtime/lingo/lingo_bytecode.h"]


def test_receipt_tracks_original_ttf_and_font_resource_transport(tmp_path):
    game = fixture_game(tmp_path)
    transport = tmp_path / "src/director64/system_fonts.py"
    transport.write_text("original pinned resource transport")
    font = game.work / "director/fonts/original.ttf"
    font.parent.mkdir(parents=True)
    font.write_bytes(b"synthetic original SFNT bytes")
    record(game)
    verify(game)
    font.write_bytes(b"changed original SFNT bytes")
    with pytest.raises(ValueError, match="generated inputs changed"):
        verify(game)
    record(game)
    transport.write_text("changed font resource transport")
    with pytest.raises(ValueError, match="inputs changed"):
        verify(game)

"""Android build freshness and install guards, without game media or the SDK."""

from __future__ import annotations

import hashlib
import json
from types import SimpleNamespace

import pytest

from director64 import android, web
from director64.project import GameSpec


def repair_game(tmp_path):
    original, repaired = b"original disc", b"repaired disc"
    source = {
        "id": "sha256-" + hashlib.sha256(original).hexdigest()[:12],
        "file": "original.iso", "sha256": hashlib.sha256(original).hexdigest(),
        "bytes": len(original), "kind": "iso9660",
        "repairs": [{"file": "repaired.iso", "sha256": hashlib.sha256(repaired).hexdigest(),
                     "bytes": len(repaired), "recipe": "host/media-repair.json"}],
    }
    game = GameSpec(tmp_path, "fixture", {"port": {"director_version": 850}}, source)
    game.media.parent.mkdir(parents=True)
    game.media.write_bytes(original)
    return game, game.media.parent / "repaired.iso", repaired


def test_stale_or_modified_tools_rebuild_before_android_conversion(tmp_path, monkeypatch):
    folder = tmp_path / "build/web/tools"
    folder.mkdir(parents=True)
    (folder / "pipeline.mjs").write_bytes(b"old pipeline")
    stamp = {
        "source_sha256": "old",
        "revision": "old-revision",
        "files": {"pipeline": "pipeline.mjs"},
        "sha256": {"pipeline": web.sha256(folder / "pipeline.mjs")},
    }
    (folder / "manifest.json").write_text(json.dumps(stamp))
    monkeypatch.setattr(web, "import_source_digest", lambda root: "current")
    monkeypatch.setattr("director64.toolchain.container_engine", lambda: "unused")
    builds = []
    monkeypatch.setattr(
        web, "build_tools", lambda root, engine: builds.append(root) or {"revision": "rebuilt"}
    )
    assert android.converter_revision(tmp_path) == "rebuilt"
    stamp["source_sha256"] = "current"
    (folder / "manifest.json").write_text(json.dumps(stamp))
    assert android.converter_revision(tmp_path) == "old-revision"
    (folder / "pipeline.mjs").write_bytes(b"new pipeline")
    assert android.converter_revision(tmp_path) == "rebuilt"
    assert len(builds) == 2


def test_apk_install_requires_current_source_and_untampered_build_receipt(tmp_path, monkeypatch):
    game = SimpleNamespace(dist=tmp_path, slug="synthetic", source={"sha256": "disc"})
    path = android.apk_path(game)
    path.write_bytes(b"APK")
    monkeypatch.setattr(android, "apk_input_digest", lambda game, abis: "current")
    with pytest.raises(ValueError, match="stale build receipt"):
        android.verify_apk(game)
    receipt = {
        "source_sha256": "disc",
        "abis": ["armeabi-v7a"],
        "inputs": "current",
        "apk_sha256": web.sha256(path),
    }
    stamp = android.apk_receipt_path(game)
    stamp.write_text(json.dumps(receipt))
    assert android.verify_apk(game) == path
    for key, wrong in [("inputs", "old"), ("source_sha256", "other-disc")]:
        stamp.write_text(json.dumps({**receipt, key: wrong}))
        with pytest.raises(ValueError, match="stale build receipt"):
            android.verify_apk(game)
    stamp.write_text(json.dumps(receipt))
    path.write_bytes(b"BAD")
    with pytest.raises(ValueError, match="stale build receipt"):
        android.verify_apk(game)


def test_failed_rebuild_invalidates_previous_install_receipt(tmp_path, monkeypatch):
    game = SimpleNamespace(
        root=tmp_path,
        dist=tmp_path,
        slug="synthetic",
        source={"sha256": "disc", "id": "source"},
        require_port=lambda: None,
    )
    android.apk_receipt_path(game).write_text("previous success")
    monkeypatch.setattr(android, "apk_input_digest", lambda game, abis: "current")

    def failed(*args):
        raise ValueError("source media failed")

    monkeypatch.setattr(android, "convert", failed)
    with pytest.raises(ValueError, match="source media failed"):
        android.build(game, 1, ["armeabi-v7a"], False)
    assert not android.apk_receipt_path(game).exists()


def test_asset_reuse_checks_profile_exporter_source_and_pack_hashes(tmp_path, monkeypatch):
    source = tmp_path / "disc.iso"
    source.write_bytes(b"not parsed on a cache hit")
    export = tmp_path / "platforms/android/export/export.mjs"
    export.parent.mkdir(parents=True)
    export.write_text("exporter")
    game = SimpleNamespace(
        root=tmp_path,
        media=source,
        source={"sha256": "disc"},
        slug="synthetic",
        data={"port": {"director_version": 850}},
    )
    profile = {"policy": {"stage_prescale": False}}
    monkeypatch.setattr(android, "converter_revision", lambda root: "converter")
    monkeypatch.setattr(web, "abi_digest", lambda root: b"abi")
    monkeypatch.setattr(web, "web_profile", lambda game: {})
    monkeypatch.setattr(web, "profile_entry", lambda game, web: profile)
    validated = []
    monkeypatch.setattr(
        web, "validate_package", lambda game, output, package: validated.append(package)
    )
    out = tmp_path / "game"
    out.mkdir()
    for name in android.GAME_FILES:
        (out / name).write_bytes(b"asset")
    receipt = {
        "converter": "converter",
        "abi": b"abi".hex(),
        "source_sha256": "disc",
        "profile_sha256": hashlib.sha256(json.dumps(profile, sort_keys=True).encode()).hexdigest(),
        "export_sha256": hashlib.sha256(web.sha256(export).encode()).hexdigest(),
        "bytes": {name: 5 for name in android.GAME_FILES},
        "sha256": {name: web.sha256(out / name) for name in android.GAME_FILES},
    }
    (out / "manifest.json").write_text(json.dumps(receipt))

    class ConversionStarted(Exception):
        pass

    def run(*args, **kwargs):
        raise ConversionStarted

    monkeypatch.setattr(android.subprocess, "run", run)
    assert android.convert(game, out) == out
    assert validated == [out / "game.d64p"]

    def rejected(*args):
        raise ValueError("runtime rejected converted package")

    # Matching manifest hashes cannot bypass the actual loader contract.
    monkeypatch.setattr(web, "validate_package", rejected)
    with pytest.raises(ValueError, match="runtime rejected"):
        android.convert(game, out)
    monkeypatch.setattr(
        web, "validate_package", lambda game, output, package: validated.append(package)
    )
    for changed in ["profile", "export", "source", "asset"]:
        profile["policy"]["stage_prescale"] = changed == "profile"
        export.write_text("changed exporter" if changed == "export" else "exporter")
        game.source["sha256"] = "different disc" if changed == "source" else "disc"
        (out / "game.d64p").write_bytes(b"WRONG" if changed == "asset" else b"asset")
        with pytest.raises(ConversionStarted):
            android.convert(game, out)
    # Fresh conversion must pass the same gate before APK assembly too.
    monkeypatch.setattr(android.subprocess, "run", lambda *args, **kwargs: None)
    monkeypatch.setattr(web, "validate_package", rejected)
    with pytest.raises(ValueError, match="runtime rejected"):
        android.convert(game, out, force=True)


def test_conversion_tracks_selected_image_in_both_directions(tmp_path, monkeypatch):
    game, repaired, data = repair_game(tmp_path)
    edition = game.source["sha256"]
    out = tmp_path / "export/game"
    monkeypatch.setattr(android, "converter_revision", lambda root: "converter")
    monkeypatch.setattr(web, "abi_digest", lambda root: b"abi")
    monkeypatch.setattr(web, "web_profile", lambda game: {})
    monkeypatch.setattr(web, "profile_entry", lambda game, web: {"source": game.source})
    monkeypatch.setattr(web, "validate_package", lambda *args: None)
    exports = []

    def export(command, **kwargs):
        metadata = json.loads(command[5].read_text())
        manifest = json.loads(command[6].read_text())
        image = {key: game.image[key] for key in ("file", "sha256", "bytes")}
        assert metadata["source_image"] == manifest["source_image"] == image
        assert metadata["source_sha256"] == manifest["source_sha256"] == edition
        assert command[3].name == image["file"]
        exports.append(image["file"])
        out.mkdir(exist_ok=True)
        for name in android.GAME_FILES:
            (out / name).write_bytes(image["file"].encode())
        manifest["bytes"] = {name: (out / name).stat().st_size for name in android.GAME_FILES}
        manifest["sha256"] = {name: web.sha256(out / name) for name in android.GAME_FILES}
        (out / "manifest.json").write_text(json.dumps(manifest))

    monkeypatch.setattr(android.subprocess, "run", export)
    assert android.convert(game, out) == out
    assert android.convert(game, out) == out
    repaired.write_bytes(data)
    assert android.convert(game, out) == out
    assert android.convert(game, out) == out
    repaired.unlink()
    assert android.convert(game, out) == out
    assert exports == ["original.iso", "repaired.iso", "original.iso"]
    # A pre-fix receipt cannot establish which image supplied these assets.
    manifest = json.loads((out / "manifest.json").read_text())
    del manifest["source_image"]
    (out / "manifest.json").write_text(json.dumps(manifest))
    android.convert(game, out)
    assert len(exports) == 4
    repaired.write_bytes(data)
    android.convert(game, out)
    repaired.write_bytes(b"modified disc")
    with pytest.raises(ValueError, match="source image differs"):
        android.convert(game, out)
    assert len(exports) == 5


def test_conversion_rejects_source_selection_changed_during_export(tmp_path, monkeypatch):
    game, repaired, data = repair_game(tmp_path)
    monkeypatch.setattr(android, "converter_revision", lambda root: "converter")
    monkeypatch.setattr(web, "abi_digest", lambda root: b"abi")
    monkeypatch.setattr(web, "web_profile", lambda game: {})
    monkeypatch.setattr(web, "profile_entry", lambda game, web: {})
    monkeypatch.setattr(
        android.subprocess, "run", lambda *args, **kwargs: repaired.write_bytes(data)
    )
    with pytest.raises(ValueError, match="selected source image changed"):
        android.convert(game, tmp_path / "export/game")


def test_apk_receipt_tracks_selected_image_without_changing_edition(tmp_path, monkeypatch):
    game, repaired, data = repair_game(tmp_path)
    edition = game.source["sha256"]
    for name in (
        "src/director64/android.py", "src/director64/build_inputs.py",
        "src/director64/web.py", web.FONT, "tools/director/validate-package.c",
    ):
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("fixture")
    monkeypatch.setattr(web, "web_profile", lambda game: {})
    monkeypatch.setattr(web, "profile_entry", lambda game, web: {"source": game.source})
    monkeypatch.setattr(web, "import_source_digest", lambda root: "importer")
    game.dist.mkdir(parents=True)
    apk = android.apk_path(game)
    apk.write_bytes(b"APK")
    abis = ["armeabi-v7a"]

    def receipt():
        value = {
            "source_sha256": edition, "abis": abis,
            "inputs": android.apk_input_digest(game, abis), "apk_sha256": web.sha256(apk),
            "source_image": {key: game.image[key] for key in ("file", "sha256", "bytes")},
        }
        android.apk_receipt_path(game).write_text(json.dumps(value))
        return value

    original = receipt()
    assert android.verify_apk(game) == apk
    repaired.write_bytes(data)
    assert android.apk_input_digest(game, abis) != original["inputs"]
    with pytest.raises(ValueError, match="stale build receipt"):
        android.verify_apk(game)
    updated = receipt()
    assert android.verify_apk(game) == apk
    for field in ("file", "sha256", "bytes"):
        altered = {**updated, "source_image": {**updated["source_image"], field: "wrong"}}
        android.apk_receipt_path(game).write_text(json.dumps(altered))
        with pytest.raises(ValueError, match="stale build receipt"):
            android.verify_apk(game)
    receipt()
    repaired.unlink()
    assert android.apk_input_digest(game, abis) == original["inputs"]
    with pytest.raises(ValueError, match="stale build receipt"):
        android.verify_apk(game)
    assert game.source["sha256"] == edition


def test_importer_fingerprint_covers_compiler_pipeline_schema_and_vendored_decoder(
    tmp_path, monkeypatch
):
    original = web.sha256
    monkeypatch.setattr(
        web,
        "sha256",
        lambda path: original(path) if path.is_file() else hashlib.sha256(b"").hexdigest(),
    )
    files = [
        "compiler/src/synthetic.rs",
        "platforms/web/import/pipeline.mjs",
        "runtime/package/package.h",
        "third_party/libwebp/src/decoder.h",
    ]
    for name in files:
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("original")
    first = web.import_source_digest(tmp_path)
    for name in files:
        path = tmp_path / name
        path.write_text("changed")
        assert web.import_source_digest(tmp_path) != first
        path.write_text("original")
    generated = tmp_path / "build/unrelated.bin"
    generated.parent.mkdir()
    generated.write_bytes(b"generated output")
    assert web.import_source_digest(tmp_path) == first

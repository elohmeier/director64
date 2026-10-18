"""Android build freshness and install guards, without game media or the SDK."""

from __future__ import annotations

import hashlib
import json
from types import SimpleNamespace

import pytest

from director64 import android, web


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

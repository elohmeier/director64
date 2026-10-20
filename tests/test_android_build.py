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
    game = GameSpec(
        tmp_path, "fixture", {"title": "Fixture", "port": {"director_version": 850}}, source
    )
    game.media.parent.mkdir(parents=True)
    game.media.write_bytes(original)
    return game, game.media.parent / "repaired.iso", repaired


def test_apk_install_requires_current_source_and_untampered_build_receipt(tmp_path, monkeypatch):
    game = SimpleNamespace(
        dist=tmp_path, slug="synthetic", source={"sha256": "disc"}, image_identity={}
    )
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
        work=tmp_path / "build",
        slug="synthetic",
        source={"sha256": "disc", "id": "source"},
        require_port=lambda: None,
    )
    android.apk_receipt_path(game).write_text("previous success")
    monkeypatch.setattr(android, "apk_input_digest", lambda game, abis: "current")

    def failed(*args, **kwargs):
        raise ValueError("source media failed")

    monkeypatch.setattr(android, "convert", failed)
    with pytest.raises(ValueError, match="source media failed"):
        android.build(game, 1, ["armeabi-v7a"], False)
    assert not android.apk_receipt_path(game).exists()


def test_apk_receipt_tracks_selected_image_without_changing_edition(tmp_path, monkeypatch):
    game, repaired, data = repair_game(tmp_path)
    edition = game.source["sha256"]
    for name in (
        "src/director64/android.py", "src/director64/android_packs.py",
        "src/director64/project.py",
        "src/director64/web.py", web.FONT, "tools/director/validate-package.c",
    ):
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("fixture")
    monkeypatch.setattr(web, "profile_entry", lambda game: {"source": game.source})
    monkeypatch.setattr(android, "source_digest", lambda root: "converter")
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
    packer = tmp_path / "src/director64/android_packs.py"
    packer.write_text("changed packing")
    with pytest.raises(ValueError, match="stale build receipt"):
        android.verify_apk(game)
    packer.write_text("fixture")
    game.data["android"] = {"title": "Short TV title"}
    assert android.apk_input_digest(game, abis) != original["inputs"]
    with pytest.raises(ValueError, match="stale build receipt"):
        android.verify_apk(game)
    del game.data["android"]
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

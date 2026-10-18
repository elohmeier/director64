from __future__ import annotations

import hashlib
import json
from types import SimpleNamespace

import pytest

from director64 import media_repair
from director64.iso import Entry, Layout


def sha(data):
    return hashlib.sha256(data).hexdigest()


def test_image_patch_preserves_all_unlisted_bytes_and_original(tmp_path):
    before = bytes(range(256)) * 32
    source = tmp_path / "original.iso"
    target = tmp_path / "repaired.iso"
    source.write_bytes(before)
    after = before[:2048] + b"recovered" + before[2057:]
    media_repair.write_image(source, target, [(2048, b"recovered")],
                             source_sha256=sha(before), output_sha256=sha(after))
    assert target.read_bytes() == after
    assert source.read_bytes() == before


@pytest.mark.parametrize("patches", [
    [(100, b"123"), (101, b"45")], [(-1, b"x")], [(8192, b"x")], [(100, b"")],
])
def test_invalid_ranges_never_replace_a_prior_result(tmp_path, patches):
    before = bytes(8192)
    source = tmp_path / "original.iso"
    target = tmp_path / "repaired.iso"
    source.write_bytes(before)
    target.write_bytes(b"prior verified result")
    with pytest.raises(ValueError, match="overlapping|bounds"):
        media_repair.write_image(source, target, patches,
                                 source_sha256=sha(before), output_sha256="a" * 64)
    assert source.read_bytes() == before
    assert target.read_bytes() == b"prior verified result"
    assert not list(tmp_path.glob(".media-repair-*"))


@pytest.mark.parametrize("changed", ["source", "output"])
def test_unexpected_hash_preserves_existing_files(tmp_path, changed):
    before = bytes(8192)
    source = tmp_path / "original.iso"
    target = tmp_path / "repaired.iso"
    source.write_bytes(before)
    target.write_bytes(b"prior")
    with pytest.raises(ValueError, match="SHA-256"):
        media_repair.write_image(source, target, [(2048, b"x")],
                                 source_sha256="a" * 64 if changed == "source" else sha(before),
                                 output_sha256="b" * 64)
    assert source.read_bytes() == before
    assert target.read_bytes() == b"prior"
    assert not list(tmp_path.glob(".media-repair-*"))


def test_cannot_overwrite_original(tmp_path):
    path = tmp_path / "disc.iso"
    path.write_bytes(bytes(4096))
    with pytest.raises(ValueError, match="separate"):
        media_repair.write_image(path, path, [(0, b"x")],
                                 source_sha256=sha(path.read_bytes()), output_sha256="a" * 64)


def fixture_game(tmp_path):
    before = bytes(2048) + b"damaged!" + bytes(2040)
    after = bytes(2048) + b"intact!!" + bytes(2040)
    spec = {"file": "repaired.iso", "sha256": sha(after), "bytes": len(after),
            "recipe": "host/media-repair.json"}
    game = SimpleNamespace(root=tmp_path, slug="fixture", directory=tmp_path / "games/fixture",
                           source={"file": "original.iso", "sha256": sha(before),
                                   "bytes": len(before), "id": "sha256-" + sha(before)[:12],
                                   "repairs": [spec]})
    directory = tmp_path / "media/fixture" / game.source["id"]
    directory.mkdir(parents=True)
    (directory / "original.iso").write_bytes(before)
    (game.directory / "host").mkdir(parents=True)
    recipe = {"schema_version": 1, "source_sha256": sha(before),
              "output": {key: spec[key] for key in ("file", "sha256", "bytes")},
              "patches": [{"path": "CLIP.MPG", "bytes": 8,
                           "source_sha256": sha(b"damaged!"), "method": "recovered-copy",
                           "replacement_sha256": sha(b"intact!!"),
                           "output_sha256": sha(b"intact!!"), "expected_info": {"frames": 4}}]}
    (game.directory / spec["recipe"]).write_text(json.dumps(recipe))
    replacement = tmp_path / "recovered.mpg"
    replacement.write_bytes(b"intact!!")
    return game, directory, recipe, replacement


def test_preparation_checks_replacement_and_validates_before_commit(tmp_path, monkeypatch):
    game, directory, recipe, replacement = fixture_game(tmp_path)
    monkeypatch.setattr(media_repair, "layout_inventory", lambda _: (
        Layout(), [Entry("CLIP.MPG", 2048, 8)],
    ))
    calls = []

    def validate(root, path, expected):
        calls.append((path.read_bytes(), expected))
        assert not (directory / "repaired.iso").exists()
        return {"frames": 4}

    monkeypatch.setattr(media_repair, "validate_mpeg", validate)
    with pytest.raises(ValueError, match="supply --replacement"):
        media_repair.prepare(game, [])
    path = media_repair.prepare(game, [replacement])
    assert calls == [(b"intact!!", {"frames": 4})]
    assert sha(path.read_bytes()) == recipe["output"]["sha256"]
    receipt = json.loads((tmp_path / "build/media-repair/fixture/receipt.json").read_text())
    assert receipt["patches"][0]["details"]["reconstructed"] is False
    # A known image can be reused without retaining a second recovered file.
    assert media_repair.prepare(game, []) == path
    path.write_bytes(b"tampered")
    with pytest.raises(ValueError, match="modified"):
        media_repair.prepare(game, [replacement])


def test_decode_failure_cannot_create_repaired_image(tmp_path, monkeypatch):
    game, directory, _, replacement = fixture_game(tmp_path)
    monkeypatch.setattr(media_repair, "layout_inventory", lambda _: (
        Layout(), [Entry("CLIP.MPG", 2048, 8)],
    ))

    def reject(*args):
        raise ValueError("unexpected decode damage")

    monkeypatch.setattr(media_repair, "validate_mpeg", reject)
    with pytest.raises(ValueError, match="unexpected decode damage"):
        media_repair.prepare(game, [replacement])
    assert not (directory / "repaired.iso").exists()


def test_recipe_change_during_validation_cannot_publish_a_receipt(tmp_path, monkeypatch):
    game, directory, recipe, replacement = fixture_game(tmp_path)
    monkeypatch.setattr(media_repair, "layout_inventory", lambda _: (
        Layout(), [Entry("CLIP.MPG", 2048, 8)],
    ))

    def change_recipe(root, path, expected):
        recipe["patches"][0]["expected_info"]["frames"] = 999
        (game.directory / "host/media-repair.json").write_text(json.dumps(recipe))
        return {"frames": 4}

    monkeypatch.setattr(media_repair, "validate_mpeg", change_recipe)
    with pytest.raises(ValueError, match="recipe changed during validation"):
        media_repair.prepare(game, [replacement])
    assert not (directory / "repaired.iso").exists()
    assert not (directory / "repaired.iso.json").exists()
    assert not (tmp_path / "build/media-repair/fixture/receipt.json").exists()

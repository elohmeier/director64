"""Synthetic preflight inputs; never depend on retail media."""

import json

import pytest

from director64.audit import audit_model, main


def model(*, ink=7, version=850, kind=1, external=False):
    data = [0] * (48 if version >= 700 else 24)
    data[0], data[1], data[5], data[7] = 16, ink, 1, 1
    member = {"number": 1, "cast": 1, "type": kind, "width": 4, "height": 1}
    movie = {
        "name": "MAIN.DXR",
        "directorVersion": version,
        "members": [] if external else [member],
        "casts": [{"number": 1, "file": "SHARED.CXT" if external else "MAIN.DXR"}],
        "score": {"frames": [{"number": 17, "channels": [{"channel": 8, "bytes": data}]}]},
    }
    movies = [movie]
    if external:
        movies.append({"name": "SHARED.CXT", "directorVersion": version, "members": [member]})
    return {"movies": movies}


def test_unsupported_ink_has_actionable_external_cast_location():
    report = audit_model(model(external=True), "web")
    assert report["findings"] == [
        {
            "code": "unsupported-bitmap-ink",
            "ink": 7,
            "records": 1,
            "examples": [{"movie": "MAIN.DXR", "frame": 17, "sprite": 3}],
        }
    ]


def test_target_and_version_capabilities_differ():
    assert not audit_model(model(ink=1), "web")["findings"]
    assert audit_model(model(ink=1), "n64")["findings"]
    assert not audit_model(model(ink=9), "android")["findings"]
    assert audit_model(model(ink=9, version=800), "web")["findings"]
    assert audit_model(model(ink=32, version=600), "web")["findings"]
    extended = model(ink=32, version=600)
    extended["extendedD6"] = True
    assert not audit_model(extended, "web")["findings"]


def test_nonbitmap_ink_is_not_reported_as_bitmap_failure():
    assert not audit_model(model(kind=8), "web")["findings"]
    assert audit_model(model(kind=9), "web")["findings"][0]["code"] == "linked-movie-not-rendered"


def test_invalid_input_cannot_pass():
    with pytest.raises(ValueError, match="versions"):
        audit_model(model(version=900), "web")
    fixture = model(external=True)
    fixture["movies"].pop()
    with pytest.raises(ValueError, match="missing external cast"):
        audit_model(fixture, "web")


def test_dynamic_member_requires_review_without_false_ink_failure():
    fixture = model()
    fixture["movies"][0]["members"] = []
    report = audit_model(fixture, "web")
    assert not report["findings"]
    assert report["unresolved_member_records"] == 1


def test_truncated_record_is_not_silently_skipped():
    fixture = model()
    fixture["movies"][0]["score"]["frames"][0]["channels"][0]["bytes"] = [16, 7]
    with pytest.raises(ValueError, match="record length"):
        audit_model(fixture, "web")


def test_strict_exit_and_approximation_inventory(tmp_path, capsys):
    path = tmp_path / "model.json"
    fixture = model()
    path.write_text(json.dumps(fixture))
    assert main([str(path), "--target", "web", "--strict"]) == 1
    assert main([str(path), "--target", "web"]) == 0
    fixture = model(ink=8)
    fixture["approximations"] = [{"kind": "text-layout"}]
    path.write_text(json.dumps(fixture))
    assert main([str(path), "--target", "web", "--strict"]) == 0
    assert '"text-layout": 1' in capsys.readouterr().out
    fixture["problems"] = [{"error": "conversion failed"}]
    path.write_text(json.dumps(fixture))
    assert main([str(path), "--target", "web", "--strict"]) == 1

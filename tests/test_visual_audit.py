"""Synthetic preflight inputs; never depend on retail media."""

import json

import pytest

from director64.audit import audit_model, main


def model(*, ink=63, version=850, kind=1, external=False):
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
            "ink": 63,
            "records": 1,
            "examples": [{"movie": "MAIN.DXR", "frame": 17, "sprite": 3, "cast": 1, "member": 1}],
        }
    ]


def test_target_and_version_capabilities_differ():
    assert not audit_model(model(ink=1), "web")["findings"]
    assert not audit_model(model(ink=1), "android")["findings"]
    assert not audit_model(model(ink=9), "android")["findings"]
    assert audit_model(model(ink=9, version=800), "web")["findings"]
    assert audit_model(model(ink=32, version=600), "web")["findings"]
    extended = model(ink=32, version=600)
    extended["extendedD6"] = True
    assert not audit_model(extended, "web")["findings"]


@pytest.mark.parametrize("target", ["web", "android"])
def test_unknown_inks_remain_blocked_for_every_target(target):
    for ink in (63,):
        assert audit_model(model(ink=ink), target)["findings"][0]["ink"] == ink


@pytest.mark.parametrize("ink", [2, 3, 4, 6, 7, 33, 34, 37, 38, 40, 41])
def test_implemented_inks_pass_shared_gate(ink):
    assert not audit_model(model(ink=ink), "web")["findings"]


def test_nonbitmap_ink_is_not_reported_as_bitmap_failure():
    assert not audit_model(model(kind=8), "web")["findings"]
    assert audit_model(model(kind=9), "web")["findings"][0]["code"] == "linked-movie-target-missing"


@pytest.mark.parametrize("runtime", [False, True])
def test_runtime_film_layers_are_checked_with_their_source_location(runtime):
    fixture = model(ink=63)
    movie = fixture["movies"][0]
    film = {"cast": 1, "number": 9, "type": 2, "filmRuntime": runtime,
            "filmScore": movie.pop("score")}
    film["filmScore"]["frames"][0]["channels"][0]["bytes"][4:6] = [255, 255]
    movie["members"].append(film)
    example = audit_model(fixture, "web")["findings"][0]["examples"][0]
    assert example == {"movie": "MAIN.DXR", "film_cast": 1, "film_member": 9,
                       "frame": 17, "sprite": 3, "cast": 1, "member": 1}


@pytest.mark.parametrize("kind", [5, 12])
def test_picture_and_rich_text_need_converted_assets(kind):
    fixture = model(kind=kind, ink=8)
    assert audit_model(fixture, "web")["findings"][0]["code"] == "raster-cast-asset-missing"
    fixture["movies"][0]["members"][0]["asset"] = "synthetic.fdi"
    assert not audit_model(fixture, "web")["findings"]
    fixture["movies"][0]["score"]["frames"][0]["channels"][0]["bytes"][1] = 63
    assert audit_model(fixture, "web")["findings"][0]["code"] == "unsupported-bitmap-ink"


def linked_model(**member_overrides):
    fixture = model(kind=9, ink=8)
    fixture["movies"][0]["members"][0].update({
        "linkedMovie": "CHILD.DXR", "linkedFlags": 30, **member_overrides,
    })
    fixture["movies"].append({"name": "CHILD.DXR", "directorVersion": 850})
    return fixture


@pytest.mark.parametrize("target", ["web", "android"])
@pytest.mark.parametrize("flags", [28, 29, 30, 31])
def test_supported_linked_movie_passes_shared_gate(target, flags):
    assert not audit_model(linked_model(linkedFlags=flags), target)["findings"]


@pytest.mark.parametrize(("override", "code"), [
    ({"linkedMovie": "MISSING.DXR"}, "linked-movie-target-missing"),
    ({"width": 801}, "unsupported-linked-movie-bounds"),
    ({"height": 601}, "unsupported-linked-movie-bounds"),
    ({"width": 0}, "unsupported-linked-movie-bounds"),
])
def test_unsupported_linked_movie_descriptor_fails(override, code):
    assert audit_model(linked_model(**override), "web")["findings"][0]["code"] == code


def test_linked_movie_mask_ink_is_not_silently_accepted():
    fixture = linked_model()
    fixture["movies"][0]["score"]["frames"][0]["channels"][0]["bytes"][1] = 9
    report = audit_model(fixture, "web")
    assert report["findings"][0]["code"] == "unsupported-linked-movie-ink"
    assert report["findings"][0]["ink"] == 9


def test_video_capability_follows_runtime_family():
    for version in (500, 850):
        fixture = model(kind=10, version=version)
        assert audit_model(fixture, "web")["findings"][0]["code"] == "video-asset-missing"
        fixture["movies"][0]["members"][0]["asset"] = "video.m1v"
        assert not audit_model(fixture, "web")["findings"]
    assert audit_model(model(kind=10, version=800), "web")["findings"]


@pytest.mark.parametrize("kind", [
    "deferred-conversion", "film-loop-unflattened-child", "film-loop-mask-missing",
    "film-loop-sounds-pending-native", "film-loop-unresolved-sound", "flash-actions-ignored",
])
def test_missing_output_blocks_even_with_recovery_disposition(kind):
    from director64.audit import require_capabilities

    fixture = model(ink=8)
    fixture["approximations"] = [{
        "kind": kind, "movie": "MAIN.DXR", "cast": 1, "member": 1,
        "disposition": "conversion pending; the member draws nothing",
    }]
    report = audit_model(fixture, "android")
    assert report["conversion_losses"] == fixture["approximations"]
    with pytest.raises(ValueError, match=f"{kind} at MAIN.DXR cast 1 member 1"):
        require_capabilities(fixture, "android", "fixture")


def test_new_explicitly_unimplemented_operation_cannot_hide_as_approximation():
    fixture = model(ink=8)
    fixture["approximations"] = [{"kind": "new-unsupported-feature", "implemented": False}]
    assert audit_model(fixture, "web")["findings"][0]["kind"] == "new-unsupported-feature"


def test_unresolved_dynamic_children_remain_review_inventory():
    fixture = model(ink=8)
    fixture["approximations"] = [{"kind": "film-loop-unresolved-child"}]
    report = audit_model(fixture, "web")
    assert not report["findings"]
    assert report["approximation_counts"] == {"film-loop-unresolved-child": 1}


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


def test_live_flash_font_needs_the_exact_face_and_requested_variant():
    fixture = model(ink=8)
    fixture["movies"][0]["members"][0]["flashFields"] = [
        {"fontName": "Original Face", "fontHeight": 12},
    ]
    assert audit_model(fixture, "web")["findings"][0]["code"] == "flash-font-variant-missing"
    fixture["fonts"] = [{"number": 1, "aliases": ["Similar Face"],
                         "variants": [{"size": 12, "asset": "fonts/f1-12.font64"}]}]
    assert audit_model(fixture, "web")["findings"]
    fixture["fonts"][0]["aliases"] = ["Original Face"]
    fixture["fonts"][0]["variants"][0]["size"] = 13
    assert audit_model(fixture, "web")["findings"]
    fixture["fonts"][0]["variants"][0]["size"] = 12
    assert not audit_model(fixture, "web")["findings"]


def test_retained_flash_requires_explicit_geometric_mask_inventory():
    data = model(ink=0, kind=2)
    member = data["movies"][0]["members"][0]
    layer = {"channel": 1, "asset": "regular.fda"}
    member["flashRuntime"] = {"clips": [{"frames": [[layer]]}]}
    finding = audit_model(data, "web")["findings"]
    assert any(item["code"] == "flash-geometric-mask-missing" for item in finding)
    layer["maskAsset"] = "mask.fda"
    assert not any(item["code"] == "flash-geometric-mask-missing"
                   for item in audit_model(data, "web")["findings"])


def test_retained_flash_runtime_is_supported_in_implemented_profiles():
    fixture = model(ink=8, kind=2)
    fixture["movies"][0]["members"][0]["flashRuntime"] = {"clips": []}
    fixture["approximations"] = [{"kind": "flash-runtime-retained", "implemented": True}]
    for target in ("web", "android"):
        report = audit_model(fixture, target)
        assert not report["findings"]
        assert report["approximation_counts"] == {"flash-runtime-retained": 1}
    fixture["movies"][0]["directorVersion"] = 800
    assert audit_model(fixture, "web")["findings"][0]["code"] == (
        "unsupported-flash-runtime-profile"
    )


@pytest.mark.parametrize("kind", ["film-loop-nested-cycle", "film-loop-absent-child"])
def test_discarded_film_layers_are_not_merely_inventory(kind):
    fixture = model(ink=8)
    fixture["approximations"] = [{"kind": kind}]
    assert audit_model(fixture, "web")["findings"][0]["kind"] == kind
    fixture["approximations"][0]["implemented"] = True
    assert not audit_model(fixture, "web")["findings"]


def test_runtime_film_preflight_resolves_zero_based_ccl_external_casts():
    fixture = model(ink=63, external=True)
    movie = fixture["movies"][0]
    movie["casts"].insert(0, {"number": 1, "file": "MAIN.DXR", "filmCastMap": [2]})
    movie["casts"][1]["number"] = 2
    film = {"cast": 1, "number": 9, "type": 2, "filmRuntime": True,
            "filmScore": movie.pop("score")}
    film["filmScore"]["version"] = 13
    film["filmScore"]["frames"][0]["channels"][0]["bytes"][4:6] = [0, 0]
    movie["members"].append(film)
    report = audit_model(fixture, "web")
    assert report["findings"][0]["code"] == "unsupported-bitmap-ink"
    assert report["findings"][0]["examples"][0]["cast"] == 2
    assert report["unresolved_member_records"] == 0


def test_invalid_unused_texture_slot_is_inventory_and_required_only_when_selected():
    fixture = model(kind=8, ink=0)
    movie = fixture["movies"][0]
    movie["casts"][0]["patternAssets"] = ["brush.fdi"] * 8
    movie["casts"][0]["patternAssets"][4] = None
    movie["casts"][0]["patternProblems"] = [
        {"pattern": 61, "kind": "source-texture-period-outside-bitmap"}
    ]
    movie["members"][0]["pattern"] = 63
    report = audit_model(fixture, "web")
    assert not report["findings"]
    assert report["source_pattern_problems"][0]["pattern"] == 61
    movie["members"][0]["pattern"] = 61
    movie["members"][0]["asset"] = "stale-other-pattern.fdi"
    assert audit_model(fixture, "web")["findings"][0]["code"] == "shape-pattern-asset-missing"

"""Independent visual/audio oracles and reviewed baseline workflow, no game data."""

import array
import json
import struct
import zlib

import pytest

from director64.visual import (
    approve_capture,
    assess_capture,
    audio_findings,
    audio_metrics,
    compare_capture,
    compare_pixels,
    read_png,
    selected_plan,
    sequence_findings,
    write_png,
)


def picture(width=32, height=24, offset=0):
    out = bytearray(bytes((30, 60, 90, 255)) * width * height)
    for y in range(7, 15):
        for x in range(5 + offset, 11 + offset):
            at = (y * width + x) * 4
            out[at : at + 4] = bytes((220, 180, 40, 255))
    return bytes(out)


def capture(path, frames=None, pcm=None):
    path.mkdir()
    frames = frames or [picture()]
    entries = []
    for i, frame in enumerate(frames):
        name = f"scene-{i:03d}.png"
        write_png(path / name, 32, 24, frame)
        entries.append({"png": name})
    checkpoint = {"label": "scene", "frames": entries, "observation": {}}
    if pcm is not None:
        (path / "scene.f32").write_bytes(array.array("f", pcm).tobytes())
        checkpoint["audio"] = {"file": "scene.f32", "rate": 8000, "channels": 1}
    report = {
        "schema_version": 1,
        "status": "captured",
        "stage": [32, 24],
        "checkpoints": [checkpoint],
        "environment": {"browser": "pinned"},
        "clock": "controlled-60hz",
        "provenance": {"source_sha256": "a" * 64, "plan_sha256": "b" * 64},
    }
    (path / "capture.json").write_text(json.dumps(report))
    return report


def test_png_round_trip_and_rejects_corruption(tmp_path):
    path = tmp_path / "image.png"
    write_png(path, 32, 24, picture())
    assert read_png(path) == (32, 24, picture())
    data = bytearray(path.read_bytes())
    data[-5] ^= 1
    path.write_bytes(data)
    with pytest.raises(ValueError, match="checksum"):
        read_png(path)


@pytest.mark.parametrize("filter_kind", [0, 1, 2, 3, 4])
def test_png_rgb_filters_used_by_device_screenshots(tmp_path, filter_kind):
    rows = [bytes((20, 60, 90, 20, 50, 30)), bytes((21, 61, 90, 22, 51, 31))]

    def chunk(kind, body):
        return (
            struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
        )

    filtered = bytearray()
    previous = bytes(6)
    for row in rows:
        filtered.append(filter_kind)
        for i, value in enumerate(row):
            left = row[i - 3] if i >= 3 else 0
            up = previous[i]
            diagonal = previous[i - 3] if i >= 3 else 0
            p = left + up - diagonal
            d = [abs(p - left), abs(p - up), abs(p - diagonal)]
            paeth = [left, up, diagonal][d.index(min(d))]
            predictor = [0, left, up, (left + up) // 2, paeth][filter_kind]
            filtered.append((value - predictor) & 255)
        previous = row
    path = tmp_path / "device.png"
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(filtered))
        + chunk(b"IEND", b"")
    )
    assert read_png(path) == (
        2,
        2,
        b"".join(row[i : i + 3] + b"\xff" for row in rows for i in range(0, 6, 3)),
    )


@pytest.mark.parametrize("fault", ["missing-font", "missing-mask"])
def test_region_oracle_detects_deliberate_visual_faults(fault):
    expected = picture()
    actual = bytearray(expected)
    for y in range(7, 15):
        for x in range(5, 11):
            at = (y * 32 + x) * 4
            actual[at : at + 4] = bytes(
                (30, 60, 90, 255) if fault == "missing-font" else (255, 255, 255, 255)
            )
    result = compare_pixels(
        expected, bytes(actual), 32, 24, [{"name": "label", "rect": [4, 6, 12, 16]}]
    )
    assert not result["passing"]
    assert result["regions"][0]["bounds"] == [5, 7, 11, 15]
    assert result["changed_pixels"] == 48


def test_local_region_detects_small_missing_control_hidden_by_global_tolerance():
    expected, actual = picture(), bytearray(picture())
    actual[0:4] = b"\xff\xff\xff\xff"
    assert compare_pixels(expected, actual, 32, 24, [{"max_changed_ratio": 0.01}])["passing"]
    assert not compare_pixels(expected, actual, 32, 24, [{"rect": [0, 0, 2, 2]}])["passing"]


def test_temporal_oracle_detects_deliberate_frozen_and_blank_frames():
    frames = [picture(offset=i) for i in range(3)]
    rules = {"min_changed_frames": 2, "max_blank_frames": 0, "max_identical_run": 1}
    assert sequence_findings(frames, 32, 24, rules) == []
    assert any("frozen" in f for f in sequence_findings([frames[0]] * 3, 32, 24, rules))
    blank = bytes((0, 0, 0, 255)) * 32 * 24
    assert any("blank" in f for f in sequence_findings([blank], 32, 24, rules))
    assert sequence_findings([frames[0]] * 3, 32, 24, {}) == []  # static scenes are valid


def test_audio_oracle_detects_muted_output_and_truncated_tap(tmp_path):
    audible = tmp_path / "audible.f32"
    audible.write_bytes(array.array("f", [0.2, -0.2] * 4000).tobytes())
    muted = tmp_path / "muted.f32"
    muted.write_bytes(bytes(8000 * 4))
    original = audio_metrics(audible, 8000, 1)
    assert original["duration"] == 1
    assert audio_findings(original, {"min_rms": 0.01}) == []
    assert audio_findings(audio_metrics(muted, 8000, 1), {}, original)
    muted.write_bytes(b"\0")
    with pytest.raises(ValueError, match="truncated"):
        audio_metrics(muted, 8000, 1)


def test_capture_needs_explicit_review_and_baseline_is_never_overwritten(tmp_path):
    current, baseline = tmp_path / "current", tmp_path / "baseline"
    capture(current)
    assert assess_capture(current)["status"] == "unbaselined"
    with pytest.raises(ValueError, match="reviewer"):
        approve_capture(current, baseline, "")
    approve_capture(current, baseline, "Local reviewer")
    assert compare_capture(current, baseline, tmp_path / "comparison")["status"] == "passing"
    with pytest.raises(ValueError, match="already exists"):
        approve_capture(current, baseline, "Local reviewer")


def test_comparison_detects_missing_sprite_and_muted_mix_with_diff_evidence(tmp_path):
    good, bad, baseline = tmp_path / "good", tmp_path / "bad", tmp_path / "baseline"
    capture(good, pcm=[0.2] * 8000)
    capture(bad, frames=[bytes((30, 60, 90, 255)) * 32 * 24], pcm=[0.0] * 8000)
    approve_capture(good, baseline, "Reviewer")
    result = compare_capture(bad, baseline, tmp_path / "diff")
    assert result["status"] == "failed"
    assert any("visual difference" in message for message in result["findings"])
    assert any("quieter" in message for message in result["findings"])
    assert list((tmp_path / "diff").glob("*-diff.png"))


def test_original_reference_cannot_be_inferred_from_a_port_baseline(tmp_path):
    current = tmp_path / "current"
    capture(current)
    with pytest.raises(ValueError, match="cannot be relabelled"):
        approve_capture(current, tmp_path / "original", "Reviewer", "original-wine")
    baseline = tmp_path / "baseline"
    approve_capture(current, baseline, "Reviewer")
    result = compare_capture(current, baseline, tmp_path / "diff")
    assert not result["original_projector_compared"] and not result["hardware_qualified"]


def test_incomparable_sources_and_browser_environments_cannot_pass(tmp_path):
    current, baseline = tmp_path / "current", tmp_path / "baseline"
    report = capture(current)
    approve_capture(current, baseline, "Reviewer")
    report["environment"]["browser"] = "different"
    (current / "capture.json").write_text(json.dumps(report))
    with pytest.raises(ValueError, match="environment"):
        compare_capture(current, baseline, tmp_path / "diff")
    report["environment"]["browser"] = "pinned"
    report["provenance"]["source_sha256"] = "c" * 64
    (current / "capture.json").write_text(json.dumps(report))
    with pytest.raises(ValueError, match="source_sha256"):
        compare_capture(current, baseline, tmp_path / "diff")


def test_artifact_paths_cannot_escape_capture_directory(tmp_path):
    current = tmp_path / "current"
    report = capture(current)
    report["checkpoints"][0]["frames"][0]["png"] = "../outside.png"
    (current / "capture.json").write_text(json.dumps(report))
    with pytest.raises(ValueError, match="outside"):
        assess_capture(current)


def test_checkpoint_capture_preserves_only_the_reachable_route_prefix():
    from director64.project import GameSpec

    game = GameSpec.load("findus-birthday")
    plan = selected_plan(game, checkpoint="name-popup")
    assert plan["steps"][-1]["label"] == "name-popup"
    assert plan["scope"]["remaining_journey_steps"] > 0
    assert "name-popup" in plan["observations"]
    assert not any(step.get("label") == "room-playback" for step in plan["steps"])


def test_requested_audio_and_sequence_frames_cannot_be_omitted(tmp_path):
    current = tmp_path / "current"
    report = capture(current)
    report["checkpoints"][0]["observation"] = {"sequence": {"frames": 13}, "audio": {"seconds": 1}}
    (current / "capture.json").write_text(json.dumps(report))
    result = assess_capture(current)
    assert result["status"] == "failed"
    assert any("requested 13 frames" in f for f in result["findings"])
    assert any("audio observation is missing" in f for f in result["findings"])
    with pytest.raises(ValueError, match="failed observations"):
        approve_capture(current, tmp_path / "baseline", "Reviewer")


def test_equal_rms_truncated_or_wrong_tone_audio_cannot_pass(tmp_path):
    import math

    def tone(path, frequency, seconds):
        values = array.array(
            "f",
            [
                0.2 * math.sin(2 * math.pi * frequency * i / 8000)
                for i in range(int(8000 * seconds))
            ],
        )
        path.write_bytes(values.tobytes())
        return audio_metrics(path, 8000, 1)

    expected = tone(tmp_path / "expected.f32", 440, 1)
    truncated = tone(tmp_path / "truncated.f32", 440, 0.25)
    wrong = tone(tmp_path / "wrong.f32", 880, 1)
    assert abs(expected["rms"] - truncated["rms"]) < 0.001
    assert abs(expected["rms"] - wrong["rms"]) < 0.001
    assert any("duration" in f for f in audio_findings(truncated, {}, expected))
    assert any("truncated" in f for f in audio_findings(truncated, {"seconds": 1}))
    assert any("spectral" in f for f in audio_findings(wrong, {}, expected))


def test_port_capture_cannot_claim_original_even_with_unrelated_engine_metadata(tmp_path):
    current = tmp_path / "current"
    capture(current)
    unrelated = {
        "loaded_modules": {"DIRAPI.DLL": {"sha256": "c" * 64}},
        "environment": {"wine": "arbitrary"},
    }
    for kind in ("original-wine", "original-windows"):
        with pytest.raises(ValueError, match="cannot be relabelled"):
            approve_capture(current, tmp_path / kind, "Reviewer", kind, unrelated)
    baseline = tmp_path / "baseline"
    approve_capture(current, baseline, "Reviewer")
    report = json.loads((baseline / "baseline.json").read_text())
    report["kind"] = "original-wine"
    (baseline / "baseline.json").write_text(json.dumps(report))
    with pytest.raises(ValueError, match="artifact-bound"):
        compare_capture(current, baseline, tmp_path / "diff")


def reference_fixture(tmp_path):
    from director64.visual import sha256

    port, reference = tmp_path / "port", tmp_path / "reference"
    report = capture(port)
    report["provenance"]["game"] = "synthetic-unit-fixture"
    (port / "capture.json").write_text(json.dumps(report))
    reference.mkdir()
    write_png(reference / "original.png", 32, 24, picture())
    (reference / "replay.json").write_text(json.dumps({"synthetic": "test-only"}))
    original = {
        "capture_complete": True,
        "loaded_modules": {
            "C:/test/DIRAPI.DLL": {"sha256": "c" * 64, "file_versions": ["8.5.1r104"]},
            "C:/test/IML32.DLL": {"sha256": "e" * 64, "file_versions": ["8.5.1r104"]},
        },
        "checkpoints": [
            {
                "id": "original-scene",
                "path": "original.png",
                "sha256": sha256(reference / "original.png"),
                "at_ms": 1000,
            }
        ],
    }
    provenance = {
        "kind": "original-reference-capture",
        "game": "synthetic-unit-fixture",
        "runner": {"name": "wine-xvfb"},
        "layout": {},
        "source": {
            "iso_sha256": "a" * 64,
            "files": {"DIRAPI.DLL": "c" * 64, "IML32.DLL": "e" * 64},
        },
        "declared_director_version": 850,
        "replay": {"sha256": sha256(reference / "replay.json")},
    }
    (reference / "capture.json").write_text(json.dumps(original))
    (reference / "provenance.json").write_text(json.dumps(provenance))
    mapping = {
        "schema_version": 1,
        "reviewed_alignment": True,
        "alignment_note": "Synthetic unit fixture, no actual original validation is claimed.",
        "port_plan_sha256": "b" * 64,
        "checkpoints": {
            "scene": {"frames": [{"reference_id": "original-scene", "port_sample": 0}]}
        },
    }
    alignment = tmp_path / "alignment.json"
    alignment.write_text(json.dumps(mapping))
    return port, reference, alignment, provenance, original, mapping


def test_original_import_binds_actual_reference_artifacts_and_reviewed_mapping(tmp_path):
    from director64.visual import import_original_reference

    port, reference, alignment, *_ = reference_fixture(tmp_path)
    baseline = tmp_path / "baseline"
    result = import_original_reference(reference, port, alignment, baseline, "Unit test reviewer")
    assert result["kind"] == "original-wine"
    compared = compare_capture(port, baseline, tmp_path / "comparison")
    assert compared["status"] == "passing"
    assert compared["original_projector_compared"]
    assert not compared["hardware_qualified"]
    (baseline / "scene-000.rgba").write_bytes(bytes((0, 0, 0, 255)) * 32 * 24)
    with pytest.raises(ValueError, match="binding changed"):
        compare_capture(port, baseline, tmp_path / "tampered-comparison")


@pytest.mark.parametrize("fault", ["source", "experiment", "png", "replay", "modules", "mapping"])
def test_original_import_rejects_unrelated_or_unverified_evidence(tmp_path, fault):
    from director64.visual import import_original_reference

    port, reference, alignment, provenance, original, mapping = reference_fixture(tmp_path)
    if fault == "source":
        provenance["source"]["iso_sha256"] = "d" * 64
    if fault == "experiment":
        provenance["layout"]["experiment"] = {"name": "ink-precision"}
    if fault == "png":
        original["checkpoints"][0]["sha256"] = "d" * 64
    if fault == "replay":
        provenance["replay"]["sha256"] = "d" * 64
    if fault == "modules":
        original["loaded_modules"] = {}
    if fault == "mapping":
        mapping["port_plan_sha256"] = "d" * 64
    (reference / "provenance.json").write_text(json.dumps(provenance))
    (reference / "capture.json").write_text(json.dumps(original))
    alignment.write_text(json.dumps(mapping))
    with pytest.raises(ValueError):
        import_original_reference(
            reference, port, alignment, tmp_path / "baseline", "Unit reviewer"
        )
    assert not (tmp_path / "baseline/baseline.json").exists()


def test_unexpected_audible_output_against_silent_reference_is_a_failure(tmp_path):
    path = tmp_path / "noise.f32"
    path.write_bytes(array.array("f", [0.2] * 8000).tobytes())
    metrics = audio_metrics(path, 8000, 1)
    silent = {**metrics, "rms": 0.0, "peak": 0.0}
    assert any("unexpected audible" in f for f in audio_findings(metrics, {}, silent))


def test_silence_contract_and_unknown_audio_rules_cannot_silently_pass(tmp_path):
    path = tmp_path / "audible.f32"
    path.write_bytes(array.array("f", [0.2] * 8000).tobytes())
    metrics = audio_metrics(path, 8000, 1)
    assert audio_findings(metrics, {"max_rms": 0.00001})
    with pytest.raises(ValueError, match="unknown audio"):
        audio_findings(metrics, {"max_rmz": 0.01})


@pytest.mark.parametrize("fault", ["launcher-only", "wrong-playback-version"])
def test_original_import_rejects_thin_launcher_version_as_engine_evidence(tmp_path, fault):
    from director64.visual import import_original_reference

    port, reference, alignment, provenance, original, _ = reference_fixture(tmp_path)
    if fault == "launcher-only":
        original["loaded_modules"] = {
            "C:/test/PF3.EXE": {"sha256": "c" * 64, "file_versions": ["9.0r383"]}
        }
    else:
        original["loaded_modules"]["C:/test/IML32.DLL"]["file_versions"] = ["9.0r383"]
    (reference / "capture.json").write_text(json.dumps(original))
    with pytest.raises(ValueError, match="launcher versions are insufficient"):
        import_original_reference(reference, port, alignment, tmp_path / "baseline", "Reviewer")


def test_wine_artifact_binding_cannot_be_relabelled_native_windows(tmp_path):
    from director64.visual import import_original_reference

    port, reference, alignment, *_ = reference_fixture(tmp_path)
    baseline = tmp_path / "baseline"
    report = import_original_reference(reference, port, alignment, baseline, "Unit reviewer")
    report["kind"] = "original-windows"
    (baseline / "baseline.json").write_text(json.dumps(report))
    with pytest.raises(ValueError, match="cannot qualify a native Windows"):
        compare_capture(port, baseline, tmp_path / "comparison")


def test_exploratory_audio_is_explicitly_unbaselined_without_weakening_content_oracles(tmp_path):
    good, bad, baseline = tmp_path / "good", tmp_path / "bad", tmp_path / "baseline"
    expected = capture(good, pcm=[0.2] * 8000)
    actual = capture(bad, pcm=[0.0] * 8000)
    for path, report in ((good, expected), (bad, actual)):
        report["checkpoints"][0]["observation"]["audio"] = {
            "comparison": "exploratory",
            "comparison_note": "No anchored cue phase in this window.",
        }
        (path / "capture.json").write_text(json.dumps(report))
    approve_capture(good, baseline, "Image-only reviewer")
    result = compare_capture(bad, baseline, tmp_path / "comparison")
    assert result["status"] == "passing"
    assert not result["audio_content_compared"]
    assert result["audio_comparisons"][0]["status"] == "unbaselined"
    with pytest.raises(ValueError, match="scope note"):
        audio_findings({"frames": 1}, {"comparison": "exploratory"})

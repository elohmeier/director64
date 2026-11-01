"""Renderer cost must fail independently of a fast machine or correct pixels."""
import json
import runpy

import pytest

from director64 import journeys, render_work
from director64.project import repository


def work():
    return {"version": 2, "inverse_scan_ratio": 4, "stage_pixels": 480000,
            "image_budget": 96 * 1024 * 1024,
            **dict.fromkeys((*render_work.COUNTERS, *render_work.PEAKS), 0),
            "frames": 1, "scene_frames": 1, "bitmap_pixels": 500000,
            "peak_frame_pixels": 500000, "peak_render_us": 300,
            "peak_render_frame": 75, "peak_render_movie": "LOGIN.DXR",
            "worst_inverse": {"pixels": 0, "sampled_pixels": 0, "channel": 0,
                                  "member": 0, "frame": 0, "movie": ""},
            "wasteful_inverse": {"pixels": 0, "sampled_pixels": 0, "channel": 0,
                                  "member": 0, "frame": 0, "movie": ""}}


def test_clipping_regression_fails_even_with_fast_host_timing():
    value = work()
    assert render_work.findings(value) == []
    value["wasteful_inverse"] = {"pixels": 960000, "sampled_pixels": 6800,
                                  "channel": 16, "member": 123, "frame": 75,
                                  "movie": "LOGIN.DXR"}
    result = render_work.summarize([{"movie": "MENU.DXR", "frame": 1,
                                     "elapsed_ms": .1, "command": "step 120 0 0 0",
                                     "render_work": value}])
    assert result["available"]
    error = result["findings"][0]["error"]
    assert "960000" in error and "LOGIN.DXR" in error and "channel 16" in error
    # The oracle measures actual rejected sampling, even without recognizing
    # an integer translation or depending on the optimized clipping helper.
    assert value["translated_pixels"] == 0


def test_legitimate_larger_draw_cannot_hide_a_smaller_fault():
    value = work()
    value["worst_inverse"] = {"pixels": 480000, "sampled_pixels": 200000,
                              "channel": 1, "member": 1, "frame": 1, "movie": "CROP"}
    value["wasteful_inverse"] = {"pixels": 200000, "sampled_pixels": 1000,
                                 "channel": 2, "member": 2, "frame": 1, "movie": "FAULT"}
    assert "FAULT" in render_work.findings(value)[0]
    value["wasteful_inverse"]["pixels"] = 100000
    assert render_work.findings(value) == []  # Below the absolute waste budget.


def test_runtime_and_evaluator_must_use_the_same_scan_ratio_policy():
    value = work()
    value["inverse_scan_ratio"] = 8
    assert render_work.findings(value)


@pytest.mark.parametrize(("key", "bad"), [
    ("peak_frame_pixels", 480000 * 17),
    ("peak_frame_inverse_pixels", 480000 * 9),
    ("peak_cache_pressure_run", 3),
])
def test_work_and_cache_limits(key, bad):
    value = work()
    value[key] = bad
    assert render_work.findings(value)


def test_full_screen_layers_are_not_spatial_waste():
    value = work()
    value["worst_inverse"].update(pixels=480000, sampled_pixels=480000)
    assert render_work.findings(value) == []


def test_one_time_cache_turnover_is_reported_without_claiming_sustained_thrash():
    value = work()
    value.update(peak_frame_evictions=105, peak_frame_pressure_evictions=105,
                 peak_cache_pressure_run=1)
    assert render_work.findings(value) == []


@pytest.mark.parametrize("change", ["version", "negative", "bool", "stage", "worst"])
def test_malformed_metrics_do_not_pass(change):
    value = work()
    if change == "version":
        value["version"] = 1
    elif change == "negative":
        value["bitmap_pixels"] = -1
    elif change == "bool":
        value["frames"] = True
    elif change == "stage":
        value["stage_pixels"] = 0
    else:
        value["worst_inverse"] = None
    assert render_work.findings(value)


def test_absent_metrics_are_unavailable_and_peaks_survive_sessions(tmp_path):
    first, second = work(), work()
    second.update(peak_render_us=900, peak_frame_pixels=800000)
    base = {"command": "step 10 0 0 0", "movie": "LOGIN.DXR", "frame": 75,
            "elapsed_ms": 1, "cpu_ms": 1, "peak_rss_bytes": 100,
            "wasm_bytes": 100, "script_errors": 0}
    for index, value in enumerate((first, second)):
        (tmp_path / f"{index}.jsonl").write_text(json.dumps(base | {"render_work": value}))
    result = journeys.performance(tmp_path)["render_work"]
    assert result["available"] and result["totals"]["frames"] == 2
    assert result["peaks"]["peak_frame_pixels"] == 800000
    assert result["hotspots"][0]["peak_render_ms"] == .9
    assert not render_work.summarize([base])["available"]


def test_matrix_audit_rejects_missing_costs_and_wrong_cache_budget():
    audit = runpy.run_path(repository() / "tools/validation/performance.py")["audit"]
    assert audit({"results": []})["status"] == "failed"
    row = {"game": "fixture", "journey": "opening", "status": "passing",
           "performance": {"render_work": render_work.summarize([{"render_work": work()}])}}
    assert audit({"results": [row]})["status"] == "passed"
    assert audit({"results": [row]}, {("other-game", "route")})["status"] == "failed"
    row["performance"]["render_work"]["totals"]["scene_frames"] = 0
    assert audit({"results": [row]})["status"] == "failed"
    row["performance"]["render_work"]["totals"]["scene_frames"] = 1
    row["performance"]["render_work"]["hotspots"][0]["image_budget"] = 192 * 1024 * 1024
    assert audit({"results": [row]})["status"] == "failed"

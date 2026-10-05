"""Contracts for the guest pacing analyzer over synthetic NATIVE_* logs."""

import pytest

from director64.pacing import DEFAULT_BUDGET, analyze, assert_pacing, parse_windows


def report_lines(
    index,
    *,
    movie="MAINSCR.DXR",
    wall_per_window=5_000_000,
    objects=1000,
    heap=100_000,
    gc=0,
    egc=0,
    dropped=0,
    renders=290,
    ticks_us=400_000,
    render_us=900_000,
    audio=(),
    overrun=False,
):
    lines = []
    if overrun:
        lines.append("DIRECTOR64 NATIVE_TIMING_OVERRUN us=120000")
    lines.append(
        f"DIRECTOR64 NATIVE_TICK movie={movie} frame=4 tick={300 * (index + 1)} "
        f"free=778152 cache=3118066 heap={heap} depth=0 objects={objects} "
        f"high=200000 frag=12345"
    )
    lines.append(
        f"DIRECTOR64 NATIVE_COST ticks_us={ticks_us} render_us={render_us} "
        f"audio_us=45168 renders={renders} steps=283951 gc={gc} egc={egc} "
        f"dropped_us={dropped} wall={1_000_000 + wall_per_window * (index + 1)}"
    )
    lines.append("DIRECTOR64 NATIVE_POINTER_COST us=123")
    for channel, pos, rate in audio:
        lines.append(f"DIRECTOR64 NATIVE_AUDIO_POS channel={channel} pos={pos} rate={rate}")
    return lines


def log_from(windows):
    return "\n".join(line for window in windows for line in window) + "\n"


def test_healthy_run_measures_delivery_fps_and_sync():
    log = log_from(
        report_lines(i, audio=[(2, 110_250 * (i + 1), 22050)]) for i in range(6)
    )
    report = analyze(log)
    assert report["summary"]["windows"] == 6
    assert report["summary"]["measurable_windows"] == 5
    assert report["summary"]["min_tick_delivery"] == pytest.approx(1.0)
    assert report["summary"]["min_audio_sync"] == pytest.approx(1.0)
    assert report["summary"]["worst_sustained_slow_run"] == 0
    row = report["windows"][1]
    assert row["wall_delta_us"] == 5_000_000
    assert row["fps"] == pytest.approx(58.0)
    assert row["work_ratio"] == pytest.approx((400_000 + 900_000 + 45_168) / 5_000_000)
    assert report["summary"]["movies"]["MAINSCR.DXR"]["windows"] == 6
    assert_pacing(report)


def test_starved_run_shows_slow_delivery_and_desynced_audio():
    # 300 ticks take 7.5 s of guest wall time: the score runs at 0.66 speed
    # while a 22050 Hz stream advances in real time.
    log = log_from(
        report_lines(
            i,
            wall_per_window=7_500_000,
            dropped=800_000 * i,
            overrun=i > 0,
            audio=[(2, 165_375 * (i + 1), 22050)],
        )
        for i in range(5)
    )
    report = analyze(log)
    summary = report["summary"]
    assert summary["min_tick_delivery"] == pytest.approx(2 / 3)
    assert summary["worst_sustained_slow_run"] == 4
    assert summary["min_audio_sync"] == pytest.approx(2 / 3)
    assert summary["total_overruns"] == 4
    assert summary["total_dropped_us"] == 3_200_000
    with pytest.raises(ValueError, match="tick delivery below"):
        assert_pacing(report)


def test_monotonic_object_growth_fails_the_degradation_budget():
    log = log_from(report_lines(i, objects=3000 + 400 * i) for i in range(12))
    report = analyze(log)
    growth = report["summary"]["object_growth"]
    assert growth["ratio"] > DEFAULT_BUDGET["max_object_growth"]
    with pytest.raises(ValueError, match="object growth"):
        assert_pacing(report)
    # The same run passes a scenario budget that allows its measured growth.
    assert_pacing(report, {"max_object_growth": 3.0})


def test_legacy_logs_without_extended_fields_still_parse():
    log = (
        "DIRECTOR64 NATIVE_TICK movie=TS.DXR frame=4 tick=7800 free=778152 "
        "cache=3118066 heap=191112 depth=0\n"
        "DIRECTOR64 NATIVE_COST ticks_us=14987240 render_us=8491570 "
        "audio_us=145168 renders=112 steps=283951\n"
        "DIRECTOR64 NATIVE_TICK movie=TS.DXR frame=4 tick=8100 free=778152 "
        "cache=3118066 heap=189608 depth=0\n"
        "DIRECTOR64 NATIVE_COST ticks_us=5324201 render_us=1878015 "
        "audio_us=145168 renders=112 steps=300083\n"
    )
    report = analyze(log)
    assert report["summary"]["windows"] == 2
    assert report["summary"]["measurable_windows"] == 0
    assert report["summary"]["min_tick_delivery"] is None
    assert report["windows"][1]["work_us"] == 5_324_201 + 1_878_015 + 145_168
    assert_pacing(report)


def test_events_attribute_to_the_following_window_and_streams_reset_safely():
    windows = [
        report_lines(0, audio=[(2, 500_000, 22050)]),
        report_lines(1, overrun=True, audio=[(2, 1_000, 22050)]),
    ]
    report = analyze(log_from(windows))
    assert [row["overruns"] for row in report["windows"]] == [0, 1]
    # The restarted stream's position went backwards; no sync ratio is faked.
    assert report["windows"][1]["audio_sync"] is None


def test_parse_windows_requires_paired_reports():
    orphan = "DIRECTOR64 NATIVE_COST ticks_us=1 render_us=1 audio_us=1 renders=1 steps=1\n"
    assert parse_windows(orphan) == []


def test_service_cost_per_step_gates_an_interpreter_regression():
    log = log_from(report_lines(i, ticks_us=500_000) for i in range(6))
    report = analyze(log)
    # report_lines keeps the step counter flat, so no window measures a cost.
    assert report["summary"]["service_us_per_step"] is None
    log = log_from(report_lines(i, ticks_us=500_000) for i in range(6)).replace(
        "steps=283951", "steps=STEPS"
    )
    lines = []
    for index, line in enumerate(log.splitlines()):
        lines.append(line.replace("STEPS", str(10_000 * (index // 3 + 1))))
    report = analyze("\n".join(lines) + "\n")
    assert report["summary"]["service_us_per_step"] == pytest.approx(50.0)
    assert_pacing(report, {"max_service_us_per_step": 50.5})
    with pytest.raises(ValueError, match="us per Lingo step exceeds 45.00"):
        assert_pacing(report, {"max_service_us_per_step": 45.0})

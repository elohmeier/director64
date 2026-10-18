import pytest

from director64.android_pacing import analyze, assert_pacing, main


def marker(sequence, *, ticks=60, renders=12, window_us=1_000_000, dropped_us=0, alerts=0):
    return (
        f"10-18 14:00:00 I Director64: DIRECTOR64 ANDROID_PACING sequence={sequence} "
        f"movie=R0021.DXR frame=470 window_us={window_us} ticks={ticks} renders={renders} "
        f"service_us=100 render_us=200 present_us=300 dropped_us={dropped_us} alerts={alerts}\n"
    )


def test_authored_twelve_hz_and_static_scene_pass_with_sixty_hz_service():
    report = analyze("".join(marker(i, renders=12 if i < 5 else 0) for i in range(10)))
    assert_pacing(report)
    assert report["summary"]["tick_delivery"] == 1
    assert report["summary"]["render_fps"] == 6


def test_sustained_slow_render_fails_even_without_script_alerts():
    report = analyze("".join(marker(i, ticks=9, dropped_us=i * 850_000) for i in range(10)))
    assert report["summary"]["observed_dropped_us"] == 7_650_000
    with pytest.raises(ValueError, match="10 consecutive windows"):
        assert_pacing(report)


def test_one_transition_does_not_hide_a_later_sustained_slow_run():
    report = analyze("".join(marker(i, ticks=9 if i in (0, 5, 6, 7) else 60) for i in range(10)))
    with pytest.raises(ValueError, match="3 consecutive windows"):
        assert_pacing(report)
    assert_pacing(report, sustained_windows=4)


@pytest.mark.parametrize("log", ["", marker(1), "DIRECTOR64 ANDROID_PACING window_us=1000000"])
def test_incomplete_capture_cannot_pass(log):
    with pytest.raises(ValueError, match="qualification failed"):
        assert_pacing(analyze(log))


def test_malformed_window_is_not_silently_discarded_from_healthy_capture():
    report = analyze("".join(marker(i) for i in range(10)) + marker(11, ticks=-1))
    with pytest.raises(ValueError, match="invalid pacing report"):
        assert_pacing(report)


def test_restart_resets_counter_differences_and_sustained_run():
    report = analyze(
        marker(10, ticks=9, dropped_us=900_000)
        + marker(11, ticks=9, dropped_us=1_800_000)
        + marker(0, ticks=9)
        + marker(1)
    )
    assert_pacing(report, min_windows=4)
    assert report["windows"][2]["dropped_delta_us"] is None


def test_hold_time_is_not_inferred_between_independent_active_windows():
    # No wall timestamps are subtracted: a minute in a menu between reports
    # must not make healthy active service look like a dropped minute.
    report = analyze(marker(0) + "10-18 14:01:00 menu dismissed\n" + marker(1))
    assert_pacing(report, min_windows=2)
    assert report["summary"]["active_seconds"] == 2


def test_alert_and_cli_exit_are_actionable(tmp_path, capsys):
    log = tmp_path / "logcat.txt"
    log.write_text("".join(marker(i, alerts=1) for i in range(10)))
    assert main([str(log)]) == 1
    assert "script alerts: 1" in capsys.readouterr().err
    log.write_text("".join(marker(i) for i in range(10)))
    assert main([str(log)]) == 0

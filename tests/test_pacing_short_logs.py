"""A capture that fails closed before it paces anything still has to analyze.

`analyze` zips each window against its predecessor to difference the monotonic
counters, which has no meaning for a log carrying fewer than two complete
report windows. Workshop's corrupt-archive boot is exactly that: it refuses the
save and stops, so its log has no NATIVE_TICK at all, and the boot gate reads
every case through the same path.
"""

from director64.pacing import analyze, assert_pacing

COMPLETE = (
    "DIRECTOR64 NATIVE_TICK movie=X frame=1 tick=300 free=1 cache=1 heap=1 "
    "depth=0 objects=1 high=1 frag=0 paused=0\n"
    "DIRECTOR64 NATIVE_COST ticks_us=1 render_us=1 audio_us=1 renders=1 "
    "steps=1 gc=0 egc=0 gc_us=0 dropped_us=0 wall=1000000\n"
)


def test_log_without_any_window_summarizes_to_nothing():
    report = analyze("nothing here")
    assert report["windows"] == []
    assert report["summary"]["windows"] == 0
    assert report["summary"]["min_tick_delivery"] is None
    # A budget cannot be violated by a run that reported no pacing at all.
    assert_pacing(report, {"min_tick_delivery": 0.99, "sustained_windows": 2})


def test_partial_window_is_not_a_window():
    """A NATIVE_TICK with no NATIVE_COST behind it carries no interval."""
    report = analyze(COMPLETE.splitlines()[0])
    assert report["summary"]["windows"] == 0


def test_single_window_has_no_predecessor_to_difference():
    report = analyze(COMPLETE)
    assert report["summary"]["windows"] == 1
    assert report["summary"]["measurable_windows"] == 0
    assert report["windows"][0]["tick_delivery"] is None
    assert_pacing(report, {"min_tick_delivery": 0.99, "sustained_windows": 2})

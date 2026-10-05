"""Contracts for the perf sweep's window retention and degradation check."""

from director64.perf import Crawl, degradation


def test_merge_accumulates_totals_and_retains_measured_windows():
    crawl = Crawl(dwell=600, limit=50)
    crawl.merge(
        [
            {"movie": "TS.DXR", "ticks": 300, "tick_us": 3000, "stage_us": 60, "max_us": 40},
            # The probe reprints reset rows with zero ticks; they must not
            # join the window series or divide anything later.
            {"movie": "LO.DXR", "ticks": 0, "tick_us": 0, "stage_us": 0, "max_us": 0},
        ]
    )
    crawl.merge(
        [{"movie": "TS.DXR", "ticks": 300, "tick_us": 4500, "stage_us": 60, "max_us": 55}]
    )
    assert crawl.rows["TS.DXR"]["ticks"] == 600
    assert crawl.rows["TS.DXR"]["tick_us"] == 7500
    assert crawl.rows["TS.DXR"]["max_us"] == 55
    assert [w["movie"] for w in crawl.windows] == ["TS.DXR", "TS.DXR"]
    assert [w["seq"] for w in crawl.windows] == [1, 2]


def test_degradation_compares_early_and_late_quartiles():
    flat = [{"ticks": 300, "tick_us": 3000} for _ in range(8)]
    assert degradation(flat)["ratio"] == 1.0
    growing = [{"ticks": 300, "tick_us": 3000 + 1000 * i} for i in range(8)]
    trend = degradation(growing)
    assert trend["first_mean_us"] < trend["last_mean_us"]
    assert trend["ratio"] > 2.0
    assert trend["windows"] == 8
    assert degradation(growing[:3]) is None

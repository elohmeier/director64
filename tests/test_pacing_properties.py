"""Properties of the guest pacing analyzer over generated NATIVE_* logs."""

import pytest
from hypothesis import given
from hypothesis import strategies as st

from director64.pacing import analyze, sustained_slow_run

windows = st.lists(
    st.tuples(
        st.integers(1, 600),  # ticks the score advanced
        st.integers(0, 300),  # steps held on purpose
        st.integers(1_000_000, 20_000_000),  # guest wall microseconds
    ),
    min_size=2,
    max_size=12,
)


def log_for(samples: list[tuple[int, int, int]]) -> str:
    lines, tick, paused, wall = [], 0, 0, 1_000_000
    for ticks, held, wall_delta in samples:
        tick, paused, wall = tick + ticks, paused + held, wall + wall_delta
        lines.append(
            f"DIRECTOR64 NATIVE_TICK movie=A.DXR frame=1 tick={tick} free=1 cache=1 "
            f"heap=1 depth=0 objects=1 high=1 frag=1 paused={paused}"
        )
        lines.append(
            f"DIRECTOR64 NATIVE_COST ticks_us=1 render_us=1 audio_us=1 renders=1 "
            f"steps=1 gc=0 egc=0 gc_us=0 dropped_us=0 wall={wall}"
        )
    return "\n".join(lines) + "\n"


@given(windows)
def test_delivery_credits_held_steps(samples):
    rows = analyze(log_for(samples))["windows"]
    assert rows[0]["tick_delivery"] is None
    for row, (ticks, held, wall) in zip(rows[1:], samples[1:], strict=True):
        assert row["paused_steps"] == held
        assert row["tick_delivery"] == pytest.approx((ticks + held) / 60 / (wall / 1e6))


@given(st.lists(st.one_of(st.none(), st.floats(0, 2)), max_size=30), st.floats(0, 2))
def test_sustained_slow_run_is_the_longest_run_below_limit(deliveries, limit):
    rows = [{"tick_delivery": value} for value in deliveries]
    flags = "".join("1" if v is not None and v < limit else "0" for v in deliveries)
    assert sustained_slow_run(rows, limit) == max(map(len, flags.split("0")))


marker_line = st.builds(
    lambda marker, fields: f"DIRECTOR64 {marker} {fields}",
    st.sampled_from(["NATIVE_TICK", "NATIVE_COST", "NATIVE_AUDIO_POS", "NATIVE_TIMING_OVERRUN"]),
    st.lists(
        st.builds(
            "{}={}".format,
            st.sampled_from(["tick", "wall", "paused", "renders", "channel", "pos", "rate", "us"]),
            st.one_of(st.integers(-5, 10**7).map(str), st.sampled_from(["x", "1.5", "-", "?"])),
        ),
        max_size=6,
    ).map(" ".join),
)


@given(st.lists(st.one_of(marker_line, st.text(max_size=40)), max_size=40))
def test_analyze_survives_corrupted_capture_lines(lines):
    # A capture can stop mid-line or interleave other output with a marker;
    # the analyzer reads what it can instead of failing the whole report.
    analyze("\n".join(lines))


@given(
    st.lists(
        st.tuples(st.integers(0, 5_000_000), st.integers(1, 100_000)), min_size=2, max_size=12
    )
)
def test_service_cost_per_step_is_total_service_over_total_steps(samples):
    lines, steps, wall = [], 0, 1_000_000
    for index, (ticks_us, step_delta) in enumerate(samples):
        steps, wall = steps + step_delta, wall + 5_000_000
        lines.append(f"DIRECTOR64 NATIVE_TICK movie=A.DXR frame=1 tick={300 * (index + 1)}")
        lines.append(f"DIRECTOR64 NATIVE_COST ticks_us={ticks_us} steps={steps} wall={wall}")
    summary = analyze("\n".join(lines) + "\n")["summary"]
    # The first window has no predecessor to diff its step counter against.
    expected = sum(t for t, _ in samples[1:]) / sum(s for _, s in samples[1:])
    assert summary["service_us_per_step"] == pytest.approx(expected)

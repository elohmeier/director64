import os

import pytest

from director64.captures import CaptureError
from director64.project import GameSpec


@pytest.fixture
def target(monkeypatch):
    for key in ("DIRECTOR64_GAME", "DIRECTOR64_SOURCE", "DIRECTOR64_WORK_DIR"):
        monkeypatch.setenv(key, os.environ.get(key, ""))
    return GameSpec.load("findus-mucklas").host("full_target")


def log():
    # Synthetic telemetry tests the gate, never counts as N64 evidence.
    lines = [
        "ACTIVITY_NAME_TYPED value=A",
        "ACTIVITY_READY movie=TS.DXR frame=7 tick=1000",
        "ACTIVITY_INTERACTION movie=TS.DXR",
        "Capture complete: 3600 frames",
    ]
    for tick in range(1500, 3900, 300):
        lines += [
            f"NATIVE_TICK movie=TS.DXR frame=7 tick={tick} free=700000",
            "NATIVE_COST ticks_us=3000000 render_us=900000 audio_us=100000 renders=30 steps=1000",
            "NATIVE_CACHE loads=0 allocation_retries=0 peak_bytes=3100000 min_free=700000",
        ]
    return "\n".join(lines)


def test_activity_gate(target):
    assert target.validate_log(log(), "TS.DXR")["maximum_work_us"] == 4_000_000


def test_departure_requires_motion(target):
    markers = "\nACTIVITY_TRAIN_ANSWER number=5\nACTIVITY_TRAIN_MOTION ticks=120 updates=24"
    assert target.validate_log(log() + markers, "TS.DXR", departure=True)
    slow = log().replace("ticks_us=3000000", "ticks_us=4500000") + markers
    assert not target.validate_log(slow, "TS.DXR", departure=True)["realtime_budget_met"]
    with pytest.raises(CaptureError, match="guest budget"):
        target.validate_log(slow, "TS.DXR")
    reloaded = log().replace("allocation_retries=0", "allocation_retries=1") + markers
    assert target.validate_log(reloaded, "TS.DXR", departure=True)["allocation_retries"] == 8
    with pytest.raises(CaptureError, match="allocation"):
        target.validate_log(reloaded, "TS.DXR")
    for failed in (
        reloaded.replace("min_free=700000", "min_free=0"),
        reloaded.replace("peak_bytes=3100000", "peak_bytes=4000000"),
    ):
        with pytest.raises(CaptureError, match="allocation"):
            target.validate_log(failed, "TS.DXR", departure=True)
    for missing in ("ACTIVITY_TRAIN_ANSWER", "ACTIVITY_TRAIN_MOTION"):
        with pytest.raises(CaptureError, match="piston motion"):
            target.validate_log(
                log() + markers.replace(missing, "missing"), "TS.DXR", departure=True
            )
    for short in (
        markers.replace("ticks=120", "ticks=119"),
        markers.replace("updates=24", "updates=1"),
    ):
        with pytest.raises(CaptureError, match="piston motion"):
            target.validate_log(log() + short, "TS.DXR", departure=True)


@pytest.mark.parametrize(
    "old,new",
    [
        ("ticks_us=3000000", "ticks_us=4500000"),
        ("tick=2400", "tick=2401"),
        ("allocation_retries=0", "allocation_retries=1"),
        ("ACTIVITY_NAME_TYPED value=A", "missing"),
        ("ACTIVITY_INTERACTION movie=TS.DXR", "missing"),
        ("Capture complete: 3600 frames", "ASSERTION FAILED"),
    ],
)
def test_activity_gate_rejects_failures(target, old, new):
    with pytest.raises(CaptureError):
        target.validate_log(log().replace(old, new), "TS.DXR")

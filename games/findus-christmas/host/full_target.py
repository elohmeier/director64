"""Capture construction controls, calendar return, and the Christmas Eve story."""

from director64.project import selected_game

from .full_boot import capture

# Measured on this capture: every window delivers at least 0.964 and the mean
# is 0.998, so the floor is set where a real regression shows rather than where
# the current run happens to sit. Growth is recorded, not judged tightly: the
# run starts on the boot screen and ends in a built scene, so the quartile
# ratio is the scene difference (5.3x handles, 2.1x heap), and the limits are
# only here to catch something running away.
PACING_BUDGET = {
    "min_tick_delivery": 0.95,
    "sustained_windows": 2,
    "max_object_growth": 8.0,
    "max_heap_growth": 4.0,
    # Measured 83.42 us per Lingo step on 2026-09-26; about 5% headroom, so a
    # runtime or layout regression fails here even where delivery has slack.
    "max_service_us_per_step": 88.0,
}


def main():
    capture(
        selected_game(),
        profile="probe",
        duration=90,
        marker="DIRECTOR64 SCENE_RENDERED name=DAG16.DXR frame=4",
        required=("START.DXR", "KICKER.DXR", "KALENDER.DXR", "DAG16.DXR", "DAG24.DXR"),
        completion="DIRECTOR64 REPLAY_COMPLETE id=christmas-activities",
        pacing_budget=PACING_BUDGET,
    )

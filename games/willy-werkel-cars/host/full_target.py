"""Capture car saves, toolbox navigation and certificate notice dismissal."""

from director64.project import selected_game

from .full_boot import capture

# What this run has been measured to sustain, asserted so a regression shows
# up here rather than in play. Steady play in every scene it visits delivers
# ticks at 1.000, and the only windows that fall below 0.90 are the ones a
# scene change lands in -- never two in a row, and never below 0.80. The three
# windows that used to read worst were the port holding the score on purpose,
# for the controller keyboard and the certificate notice; those are counted
# now rather than charged to the console.
#
# The run starts on the login screen and ends in the workshop with a saved
# car, so it legitimately holds more by the end than at the start: handles
# grow about 1.42x across the quartiles and the value heap about 1.35x. The
# limits sit above that to catch a leak rather than the scene difference.
PACING_BUDGET = {
    "min_tick_delivery": 0.90,
    "sustained_windows": 2,
    # Symbols left the object count on 2026-09-26 (they are ids now), which
    # took about a thousand of them out of the baseline; the same scene
    # growth measured 1.71x after that where it had measured 1.54x before.
    "max_object_growth": 1.9,
    "max_heap_growth": 1.7,
    # Measured 106.62 us per Lingo step on 2026-09-26; about 5% headroom, so a
    # runtime or layout regression fails here even where delivery has slack.
    "max_service_us_per_step": 112.0,
}


def main():
    capture(
        selected_game(),
        profile="probe",
        duration=150,
        start_timeout=600,
        marker="DIRECTOR64 SCENE_RENDERED name=08.DXR frame=1",
        required=("START.DXR", "LBSTART.DXR", "10.DXR", "03.DXR", "06.DXR", "08.DXR"),
        completion="DIRECTOR64 REPLAY_COMPLETE id=car-save-load",
        pacing_budget=PACING_BUDGET,
    )

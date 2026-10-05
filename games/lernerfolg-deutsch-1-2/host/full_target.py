"""Capture the showcase: intro, login, class pick, a room visit and a task."""

from director64.project import selected_game

from .full_boot import BOOT_SCENES, capture


def main():
    capture(
        selected_game(),
        profile="probe",
        duration=220,
        marker="DIRECTOR64 SCENE_RENDERED name=TIVINTRO.DXR frame=1",
        required=BOOT_SCENES + ("MAINSCR.DXR", "ROOM08.DXR", "TEST2A.DXR"),
        completion="DIRECTOR64 REPLAY_COMPLETE id=deutsch-login-keyboard",
    )

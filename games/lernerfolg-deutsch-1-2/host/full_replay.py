"""Generate the keyboard login replay after the sanitized native journey."""

import hashlib
import json

from director64.aot import cstring
from director64.project import selected_game


def main():
    game = selected_game()
    root = game.work / "native/journey"
    report = json.loads((root / "validation.json").read_text())
    if (
        report.get("status") != "passing"
        or not report.get("sanitizers")
        or report["source_sha256"] != game.source["sha256"]
        or "login-name-class-demoskip-room" not in report["scenarios"]
    ):
        raise ValueError("probe requires the passing sanitized login journey")
    for name, digest in report["evidence"].items():
        if hashlib.sha256((root / name).read_bytes()).hexdigest() != digest:
            raise ValueError("native journey evidence changed")
    identity = hashlib.sha256((root / "validation.json").read_bytes()).hexdigest()
    steps = []

    def step(ticks, x=320, y=240, buttons="0", movie="", frame=0, save=0):
        steps.append(f"{{{cstring(movie)},{frame},{ticks},{x},{y},{buttons},{save}}},")

    def click(x, y, ticks=90):
        step(40, x, y)
        step(8, x, y, "INPUT_A")
        step(ticks, x, y)

    def key(button, ticks=20):
        step(10, 294, 190, button)
        step(ticks, 294, 190)

    # The validated journey's authored 800x600 coordinates map onto the
    # 640x480 screen through the stage prescale (x4/5).
    step(120, movie="LOGIN.DXR", frame=8)
    # Pressing A on the editable name line opens the on-screen keyboard.
    step(40, 294, 190)
    step(8, 294, 190, "INPUT_A")
    step(40, 294, 190)
    key("INPUT_A", 30)  # A (cell 0)
    key("INPUT_DOWN")
    key("INPUT_RIGHT")
    key("INPUT_RIGHT")
    key("INPUT_RIGHT")  # N (cell 13)
    key("INPUT_A", 30)  # AN
    key("INPUT_A", 30)  # ANN
    key("INPUT_UP")
    key("INPUT_LEFT")
    key("INPUT_LEFT")
    key("INPUT_LEFT")  # back to A
    key("INPUT_A", 60)  # ANNA held on the preview line
    step(10, 294, 190, "INPUT_START")  # commit through dg_edit_text
    step(150)  # the keyDown replacement types into the field
    click(378, 241)  # green confirm arrow
    step(480, save=1)  # profile persisted; the class buttons settle
    click(228, 362, 60)  # class 1
    step(240, movie="MAINSCR.DXR")
    # Skip the map demo promptly — its unskipped tail overruns the service
    # clock on the console — then enter the castle's centre room and let
    # its introduction and ambience play.
    step(30)
    click(320, 240, 300)  # skip the map demo
    click(320, 240, 600)  # the centre room on the castle map
    step(60, movie="ROOM08.DXR")
    step(1500)  # the room narration and ambience
    # The room intro releases its clickpoints after the speech, so the
    # exercise book takes two activations; the spelling task then writes its
    # prompt cards and the first answer card is picked, mirroring the
    # sanitized host journey's exercise leg.
    click(118, 389, 800)  # exercise book, first activation
    click(118, 389, 800)  # exercise book again: into the spelling task
    step(2500, movie="TEST2A.DXR")  # the task writes its prompt cards
    click(341, 210, 600)  # the first answer card
    (game.work / "director/c/replay.inc").write_text(
        f"static const char replay_source_sha256[]={cstring(identity)};\n"
        "static const replay_step_t replay_steps[]={\n" + "\n".join(steps) + "\n};\n"
    )

"""Prepare a controller probe for car saves, toolbox navigation and the print notice."""

import hashlib
import json

from director64.aot import cstring
from director64.project import selected_game


def main():
    game = selected_game()
    root = game.work / "native/journey"
    report = json.loads((root / "validation.json").read_text())
    replay_hash = hashlib.sha256((root / "replay.json").read_bytes()).hexdigest()
    if (
        report.get("status") != "passing"
        or not report.get("sanitizers")
        or report["source_sha256"] != game.source["sha256"]
        or report["replay_sha256"] != replay_hash
    ):
        raise ValueError("probe requires the matching passing sanitized native journey")
    steps = []

    def step(ticks, x=200, y=80, buttons="0", movie="", frame=0):
        steps.append(f"{{{cstring(movie)},{frame},{ticks},{x},{y},{buttons}}},")

    def press(button):
        step(2, buttons=button)
        step(2)

    step(120, movie="10.DXR", frame=3)
    press("INPUT_A")  # Original editable source field opens the keyboard.
    def type_name(name):
        cell = 0
        for letter in name:
            target = ord(letter) - ord("A")
            while cell // 10 != target // 10:
                press("INPUT_DOWN")
                cell = (cell + 10) % 40
            while cell % 10 != target % 10:
                press("INPUT_RIGHT")
                cell = (cell // 10) * 10 + (cell + 1) % 10
            press("INPUT_A")

    def click(x, y):
        step(30, x, y)
        step(2, x, y, "INPUT_A")
        step(2, x, y)

    type_name("WILLY")
    step(180)
    press("INPUT_START")
    step(120)
    step(2, 100, 250, "INPUT_A")
    step(2, 100, 250)
    step(180, movie="10.DXR", frame=5)
    step(2, 320, 240, "INPUT_A")
    step(2, 320, 240)
    step(300, movie="03.DXR", frame=2)
    step(30, 320, 200)
    step(2, 320, 200, "INPUT_A")
    step(30, 610, 120, "INPUT_A")
    step(60, 610, 120)
    step(30, 320, 200, movie="03.DXR", frame=2)
    click(610, 120)
    step(600, movie="06.DXR", frame=1)
    click(300, 200)
    step(180)
    click(320, 440)
    type_name("ROADSTER")
    press("INPUT_START")
    step(120)
    click(575, 440)
    step(180, movie="03.DXR", frame=2)
    click(409, 55)
    step(180, movie="06.DXR", frame=13)
    click(300, 200)
    step(300, movie="03.DXR", frame=2)
    click(615, 430)
    step(120)
    click(310, 200)
    step(300, movie="08.DXR", frame=1)
    click(610, 375)
    step(180)
    step(2, buttons="INPUT_B")
    step(60)
    click(612, 441)
    step(180, movie="03.DXR", frame=2)
    click(615, 430)
    step(120)
    click(490, 245)
    step(120)
    click(610, 120)
    step(180, movie="06.DXR", frame=1)
    click(575, 440)
    step(180, movie="03.DXR", frame=2)
    (game.work / "director/c/replay.inc").write_text(
        f"static const char replay_source_sha256[]={cstring(replay_hash)};\n"
        "static const replay_step_t replay_steps[]={\n" + "\n".join(steps) + "\n};\n"
    )

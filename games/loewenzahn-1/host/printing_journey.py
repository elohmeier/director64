"""Pointer-only checks for every print stamp, pause, dismissal and chapter return."""

import hashlib
import json

from director64.project import selected_game

from .full_journey import DOOR, RIGHT, Replay


def open_crafts(replay):
    replay.step(4000)
    replay.click(*RIGHT)
    replay.click(120, 310)
    for _ in range(300):
        if replay.state["movie"] == "UNK.DXR" and any(
            s["id"] == 10 for s in replay.state["sprites"]
        ):
            break
        replay.step(60)
    replay.sprite(10)
    replay.step(600)
    assert replay.state["movie"] == "BAS.DXR"
    assert replay.state["objects"]["gspiel"]["mkapitel"] == "8"


def print_chapter(replay, book, chapter, pause=600):
    state = replay.state
    assert state["movie"] == book + ".DXR"
    assert state["objects"]["gspiel"]["mkapitel"] == str(chapter)
    expected = chapter + (8 if book == "REZ" else 0)
    replay.sprite(43)
    state = replay.state
    assert state["print_id"] == expected and state["print_paused"]
    frozen = (state["tick"], state["frame"], state["objects"]["gspiel"])
    # Other input cannot turn a page or dismiss the QR. There is no timeout.
    replay.step(pause, 40, 40, 1)
    state = replay.state
    assert state["print_id"] == expected
    assert frozen == (state["tick"], state["frame"], state["objects"]["gspiel"])
    replay.step(6, 40, 40, 2)  # INPUT_B, also held for several samples.
    assert replay.state["print_id"] == 0 and replay.state["print_paused"]
    replay.step(1, 40, 40, 0)
    state = replay.state
    assert not state["print_paused"]
    assert frozen == (state["tick"], state["frame"], state["objects"]["gspiel"])
    replay.step(120)
    assert replay.state["tick"] > frozen[0]


def run(executable, output):
    game = selected_game()
    hashes = {}
    for book in ("BAS", "REZ", "PRINT"):
        replay = Replay(executable, output / book)
        try:
            if book in {"BAS", "PRINT"}:
                open_crafts(replay)
                chapters = (8, 7) if book == "PRINT" else range(8, 0, -1)
                for chapter in chapters:
                    print_chapter(replay, "BAS", chapter, pause=120 if book == "PRINT" else 600)
                    if chapter != chapters[-1]:
                        replay.sprite(41)
            else:
                replay.step(4000)
                replay.click(*DOOR)
                for _ in range(3):
                    replay.click(*RIGHT)
                flour = next(s for s in replay.state["sprites"] if s["member"] & 65535 == 83)
                replay.sprite(flour["id"])
                for chapter in range(1, 9):
                    print_chapter(replay, "REZ", chapter)
                    if chapter != 8:
                        replay.sprite(40)
            replay.sprite(46)
            replay.step(300)
            assert replay.state["movie"] == "PANO.DXR"
        finally:
            replay.close()
        hashes[book] = hashlib.sha256((output / book / "replay.json").read_bytes()).hexdigest()
    report = {
        "status": "passing",
        "source_sha256": game.source["sha256"],
        "documents": 16,
        "replay_sha256": hashes,
        "sanitizers": True,
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "scope": "source pointer input; correct document, frozen clock, B release gate and return",
        "hardware_qualified": False,
    }
    (output / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    return report

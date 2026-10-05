"""Exercise source navigation with pointer input; no script or state injection."""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
from pathlib import Path

from director64.project import selected_game

from .full_boot import validate_intro_stops

DOOR = (309, 270)
RIGHT = (610, 240)
ROUTES = {
    "HOL": ([], (205, 400)),
    "MAU": ([], (115, 459)),
    "UNK": ([RIGHT], (120, 310)),
    "KAR": ([RIGHT], (519, 397)),
    "SCH": ([DOOR, RIGHT], (524, 124)),
    "HAM": ([DOOR, RIGHT], (444, 220)),
    "SAL": ([DOOR, RIGHT, RIGHT, RIGHT], (369, 272)),
    "FIL": ([DOOR, RIGHT, RIGHT], (188, 65)),
    "HOE": ([DOOR, RIGHT, RIGHT], (460, 208)),
}


class Replay:
    def __init__(self, executable, output):
        self.output = output
        output.mkdir(parents=True, exist_ok=True)
        self.log = (output / "native.log").open("w")
        self.commands = []
        self.states = []
        self.process = subprocess.Popen(
            [str(executable), "START", "rpc"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.log,
            text=True,
            env=os.environ | {"DIRECTOR64_WORK_DIR": str(selected_game().work)},
        )
        self.read()

    def read(self):
        line = self.process.stdout.readline()
        if not line:
            raise ValueError(f"native probe exited: {self.output}")
        state = json.loads(line)
        self.states.append(state)
        if state["error"] or state["quit"]:
            raise ValueError(state["error"] or "unexpected quit")
        self.state = state
        return state

    def step(self, ticks, x=320, y=240, down=0):
        command = f"step {ticks} {x} {y} {down}"
        self.commands.append(command)
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        return self.read()

    def click(self, x, y, settle=240):
        self.step(30, x, y)
        self.step(6, x, y, 1)
        return self.step(settle, x, y)

    def sprite(self, channel):
        sprite = next(s for s in self.state["sprites"] if s["id"] == channel)
        left, top, right, bottom = sprite["bounds"]
        return self.click((left + right) // 2, (top + bottom) // 2)

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.write("quit\n")
            self.process.stdin.flush()
        self.process.wait(timeout=10)
        self.log.close()
        data = {"commands": self.commands, "states": self.states}
        (self.output / "replay.json").write_text(json.dumps(data, indent=2) + "\n")
        if self.process.returncode:
            raise ValueError(f"native exit {self.process.returncode}: {self.output}")


def run(executable: Path, output: Path):
    game = selected_game()
    model = json.loads((game.work / "director/model.json").read_text())
    movies = {m["name"]: m for m in model["movies"]}
    checkpoints = {}
    replays = {}
    for name, (path, point) in ROUTES.items():
        replay = Replay(executable, output / name)
        try:
            state = replay.step(4000)
            if state["movie"] != "PANO.DXR":
                raise ValueError("source launcher did not reach panorama")
            if name == "HOL":
                before = state["frame"]
                replay.click(615, 460)
                if replay.state["movie"] != "DIALOG.DXR":
                    raise ValueError("source flower button did not open dialog")
                # Grab the mower handle, then drag beyond both authored limits.
                replay.step(30, 350, 184)
                replay.step(120, 350, 184, 1)
                for x, expected in ((180, 0), (297, 128), (430, 255), (297, 128)):
                    state = replay.step(120, x, 184, 1)
                    if int(state["objects"]["gvolume"]["msoundlevel"]) != expected:
                        raise ValueError("source volume slider did not follow the drag")
                    if state["channel_volumes"][:4] != [expected] * 4:
                        raise ValueError("volume drag did not update all four sound channels")
                replay.step(120, 297, 184)
                replay.click(40, 40)
                if replay.state["movie"] != "PANO.DXR" or replay.state["frame"] != before:
                    raise ValueError("closing dialog did not restore the suspended frame")
                if replay.state["channel_volumes"][:4] != [128] * 4:
                    raise ValueError("closing dialog lost the selected channel volumes")
                replay.click(615, 460)
                if int(replay.state["objects"]["gvolume"]["mposh"]) != 185:
                    raise ValueError("reopening dialog lost the volume slider position")
                replay.click(40, 40)
            for x, y in path:
                replay.click(x, y)
            replay.click(*point)
            target = name + ".DXR"
            menu = next(
                label["frame"]
                for label in movies[target]["score"]["labels"]
                if label["name"] == "Menue"
            )
            for _ in range(300):
                state = replay.state
                if state["movie"] == target and menu <= state["frame"] < menu + 4:
                    break
                replay.step(60, *point)
            else:
                raise ValueError(f"{name}: source intro did not reach its menu")
            if state["objects"]["gcontrol"]["mlastmovie"] != "PANO.DXR":
                raise ValueError(f"{name}: shared stopMovie did not retain the previous movie")
            checkpoints[name] = {"menu_frame": state["frame"], "tick": state["tick"]}
            replay.sprite(10)
            replay.step(600)
            topic_movie = "BAS.DXR" if name == "UNK" else target
            if replay.state["movie"] != topic_movie or (
                topic_movie == target and replay.state["frame"] < menu + 4
            ):
                raise ValueError(f"{name}: first topic did not open")
            checkpoints[name]["topic_frame"] = replay.state["frame"]
            checkpoints[name]["topic_movie"] = replay.state["movie"]
            # The common wagon control returns to the authored panorama view.
            replay.sprite(46)
            replay.step(300)
            if replay.state["movie"] != "PANO.DXR":
                raise ValueError(f"{name}: wagon did not return to panorama")
            checkpoints[name]["return_frame"] = replay.state["frame"]
        finally:
            replay.close()
        validate_intro_stops((output / name / "native.log").read_text(), model)
        replays[name] = hashlib.sha256((output / name / "replay.json").read_bytes()).hexdigest()
    report = {
        "status": "passing",
        "platform": "host-native",
        "scenario": "complete-intro-volume-drag-nine-topic-menus-first-topics-return",
        "checkpoints": checkpoints,
        "replay_sha256": replays,
        "source_sha256": game.source["sha256"],
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "hardware_qualified": False,
        "original_projector_compared": False,
    }
    (output / "journey.json").write_text(json.dumps(report, indent=2) + "\n")
    return report

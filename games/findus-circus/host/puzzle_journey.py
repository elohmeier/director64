"""Reach and solve the imaging puzzle with pointer drags and keyboard turns.

Read-only observations check the game's own filled slots. They never invoke a
handler, replace an image, jump a movie or change a property.
"""
import json
import os
import re

from director64.fuzz import Probe
from director64.journeys import expect, plans
from director64.project import GameSpec


def run(executable, output):
    game = GameSpec.load("findus-circus")
    probe = Probe(executable, "PF2", output / "probe.stderr", os.environ.copy())
    observations = []
    failure = None

    def send(command):
        ticks = int(command.split()[1]) if command.startswith("step ") else 0
        state = probe.send(command, ticks)
        if state.get("error") or state.get("script_errors") or state.get("quit"):
            raise ValueError(state.get("last_script_error") or state.get("error") or "quit")
        return state

    def click(x, y, settle):
        send(f"step 12 {x} {y} 0")
        send(f"step 4 {x} {y} 1")
        send(f"step {settle} {x} {y} 0")

    def inspect(path):
        before = probe.state["tick"]
        value = send("inspect gflickenpuzzleobj ppuzzleobj " + path)["inspect"]
        state = send("state")
        if state["tick"] != before:
            raise ValueError("read-only observation advanced the game clock")
        observations.append({"tick": before, "path": path, "value": value})
        return value

    try:
        probe.receive(60)
        # Reuse the registered player's ordinary startup/profile/yard inputs.
        plan = next(p for p in plans(game) if p["name"] == "player-yard")
        for action in plan["steps"]:
            if "command" in action:
                send(action["command"])
            elif "idle" in action:
                send(f"step {action['idle']} 400 300 0")
            elif "click" in action or "sprite" in action:
                if "sprite" in action:
                    sprite = next(s for s in probe.state["sprites"] if s["id"] == action["sprite"])
                    left, top, right, bottom = sprite["bounds"]
                    x = (max(0, left) + min(800, right)) // 2
                    y = (max(0, top) + min(600, bottom)) // 2
                else:
                    x, y = action["click"]
                click(x, y, action.get("settle", 120))
            if "wait" in action:
                for elapsed in range(0, action.get("ticks", 6000) + 1, 60):
                    try:
                        expect(probe.state, action["wait"])
                        break
                    except ValueError:
                        if elapsed + 60 > action.get("ticks", 6000):
                            raise
                        send("step 60 400 300 0")
            expect(probe.state, action.get("expect", {}))
        click(415, 376, 2400)
        click(415, 376, 2400)
        expect(probe.state, {"movie": "FLICKENP.DXR", "frame": 35})
        channels = {int(n): int(ch) for n, ch in
                    re.findall(r"#patch_(\d+):(\d+)", inspect("pspritelist"))}
        if set(channels) != set(range(1, 17)):
            raise ValueError("puzzle did not create all 16 imaged pieces")
        filled = json.loads(inspect("pfilledlist"))
        if filled != [1] + [0] * 15:
            raise ValueError("puzzle did not start with its authored fixed piece")
        for piece in range(2, 17):
            channel = channels[piece]
            dx, dy = 280 + ((piece - 1) % 4) * 80, 140 + ((piece - 1) // 4) * 80
            for turn in range(4):
                sprite = next(s for s in probe.state["sprites"] if s["id"] == channel)
                sx, sy = sprite["loc"]
                point = send(f"point {channel}")["point"]
                if point is None:
                    raise ValueError(f"puzzle piece {piece} has no visible hit-test point")
                x, y = point
                send(f"step 12 {x} {y} 0")
                send(f"step 4 {x} {y} 1")
                if inspect("pdragpatchnum") != str(piece):
                    raise ValueError(f"pointer did not pick up puzzle piece {piece}")
                if turn:
                    send("key 124 29 1")
                    send("key 124 29 0")
                x, y = dx + x - sx, dy + y - sy
                send(f"step 30 {x} {y} 1")
                send(f"step 180 {x} {y} 0")
                filled = json.loads(inspect("pfilledlist"))
                if filled[piece - 1]:
                    break
            else:
                raise ValueError(f"piece {piece} did not snap after all four orientations")
            if filled[:piece] != [1] * piece:
                raise ValueError("placing a piece lost an earlier completed slot")
        send("step 180 400 300 0")
        expect(probe.state, {"movie": "FLICKENP.DXR"})
        report = {"status": "passing", "pieces": sum(filled),
                  "read_only_observations": observations, "state": probe.state}
    except BaseException as error:
        failure = str(error)
        raise
    finally:
        outcome = probe.close()
        # Keep an input-only replay usable by parity/metamorphic/Android tools.
        commands = [c for c in probe.commands if not c.startswith(("inspect ", "point "))
                    and c != "state"]
        (output / "replay.json").write_text(json.dumps({"commands": commands,
            "observations": observations, "error": failure}, indent=2) + "\n")
        if failure is None and outcome.kind != "ok":
            raise ValueError(f"probe {outcome.kind}: {outcome.error}")
    return report

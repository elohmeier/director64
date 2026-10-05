"""Drive the original calendar and activities with pointer input under sanitizers."""

import hashlib
import json
import os
import re
import subprocess
from contextlib import contextmanager

from director64.project import selected_game


@contextmanager
def session(executable, output, game):
    commands, states = [], []
    with output.with_suffix(".log").open("w") as log:
        process = subprocess.Popen(
            [str(executable), "START", "rpc"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=log,
            text=True,
            env=os.environ
            | {
                "DIRECTOR64_WORK_DIR": str(game.work),
                "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=1",
                "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
            },
        )

        def receive():
            line = process.stdout.readline()
            if not line:
                raise ValueError(f"native process ended; see {output.with_suffix('.log')}")
            state = json.loads(line)
            states.append(state)
            if state["error"] or state["quit"] or state["script_errors"]:
                raise ValueError(f"activity failed: {state['error'] or state['last_script_error']}")
            return state

        def command(text):
            commands.append(text)
            process.stdin.write(text + "\n")
            process.stdin.flush()
            return receive()

        try:
            receive()
            yield command, states
        finally:
            if process.poll() is None:
                process.stdin.write("quit\n")
                process.stdin.flush()
            process.wait(timeout=30)
            output.with_suffix(".json").write_text(
                json.dumps({"commands": commands, "states": states}, indent=2) + "\n"
            )
        if process.returncode:
            raise ValueError("native journey process failed")


def click(command, x, y, ticks=1200):
    command(f"step 60 {x} {y} 0")
    command(f"step 10 {x} {y} 1")
    return command(f"step {ticks} {x} {y} 0")


def open_day(command, x, y, day):
    state = command("step 1200 320 240 0")
    assert (state["movie"], state["frame"]) == ("KALENDER.DXR", 5)
    assert state["globals"]["decdate"] == "24"
    click(command, x, y, 120)
    state = click(command, x, y, 6000)
    assert state["movie"] == f"DAG{day:02}.DXR", state["movie"]
    return state


def doors(game):
    source = (game.work / "analysis/lingo/KALENDER.DXR.lingo").read_text()
    found = re.findall(r'pos = point\((\d+), (\d+)\)\s+dag = "dag(\d+)"', source)
    result = [(int(x), int(y), int(day)) for x, y, day in found]
    assert [day for _, _, day in result] == list(range(1, 25))
    return result


def run(executable, output):
    game = selected_game()
    output.mkdir(parents=True, exist_ok=True)
    checkpoints = []
    for x, y, day in doors(game):
        with session(executable, output / f"day{day:02}", game) as (command, states):
            state = open_day(command, x, y, day)
            checkpoints.append(
                {
                    "day": day,
                    "movie": state["movie"],
                    "frame": state["frame"],
                    "sounds": state["sounds"],
                    "heap_high_water": state["heap_high_water"],
                }
            )
            if day == 16:
                command("step 60 560 200 0")
                command("step 60 560 200 1")
                command("step 60 320 240 1")
                state = command("step 1200 320 240 0")
                assert state["globals"]["gantalobj"] == "1"
                # Symbols compare without case; the list prints the corpus's
                # canonical spelling (#xPos).
                assert "#xpos:320,#ypos:240" in state["globals"]["fillista"].lower()
                state = click(command, 400, 450)
                assert state["globals"]["gantalobj"] == "0"
            else:
                state = click(command, 400, 450, 3000)
                if day == 4:
                    state = click(command, 320, 200, 3000)
                if day == 6:
                    state = click(command, 510, 359, 3000)
                checkpoints[-1]["play_frame"] = state["frame"]
            state = click(command, 220, 450, 6000)
            assert (state["movie"], state["frame"]) == ("KALENDER.DXR", 5), (
                day,
                state["movie"],
                state["frame"],
            )
            checkpoints[-1]["returned_to_calendar"] = True
            print(f"Day {day:02}: entered, played and returned", flush=True)
    evidence = {
        p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(output.glob("day*.json"))
    }
    return {
        "status": "passing",
        "platform": "host-native",
        "source_sha256": game.source["sha256"],
        "scenarios": [
            "all-24-calendar-doors",
            "all-24-start-and-return",
            "construction-drag-clear",
        ],
        "checkpoints": checkpoints,
        "evidence": evidence,
        "hardware_qualified": False,
        "original_projector_compared": False,
    }

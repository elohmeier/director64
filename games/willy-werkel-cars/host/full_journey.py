"""Exercise workshop UI and saves, then driving with a recovered makeCar fixture."""

import hashlib
import json
import os
import subprocess

from director64.project import selected_game


def validate_state(state):
    if state["error"] or state["quit"] or state["script_errors"]:
        raise ValueError(
            f"source journey failed: {state['error'] or state['last_script_error'] or 'quit'}"
        )


def run(executable, output):
    game = selected_game()
    output.mkdir(parents=True, exist_ok=True)
    commands, states = [], []
    with (output / "native.log").open("w") as log:
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
            state = json.loads(process.stdout.readline())
            states.append(state)
            validate_state(state)
            return state

        def command(text):
            commands.append(text)
            process.stdin.write(text + "\n")
            process.stdin.flush()
            return receive()

        def click(x, y, ticks=3000):
            command(f"step 60 {x} {y} 0")
            command(f"step 1 {x} {y} 1")
            return command(f"step {ticks} {x} {y} 0")

        def field(state, sprite=65):
            return next(s for s in state["sprites"] if s["id"] == sprite)

        try:
            receive()
            for attempt in range(2):
                state = command("step 18000 320 240 0")
                # Frame 4 is the login screen's "Wait" idle. Getting there means
                # frame 3 ("Talk") played Willy's instruction and released: the
                # score used to park on 3 forever, and this read 3 to match.
                assert (state["movie"], state["frame"]) == ("10.DXR", 4)
                command("text 14 WILLY")
                command("step 1 100 250 1")
                state = command("step 6000 100 250 0")
                assert state["player"]["userid"] == "WILLY"
                assert state["save_generation"] >= 1
                command("step 1 320 240 1")
                state = command("step 6000 320 240 0")
                assert (state["movie"], state["frame"]) == ("03.DXR", 2)
                assert state["depth"] == 0 and state["sounds"] > 4
                assert "#shopFloor:[:]" in state["player"]["junk"]
                if not attempt:
                    first_player = state["player"]
                    command("reboot")
                else:
                    assert state["player"]["junk"] == first_player["junk"]
                    assert state["player"]["userid"] == first_player["userid"]
            for x, y, expected in ((85, 150, "04.DXR"), (490, 150, "03.DXR"), (520, 100, "02.DXR")):
                command(f"step 60 {x} {y} 0")
                command(f"step 1 {x} {y} 1")
                state = command(f"step 6000 {x} {y} 0")
                assert state["movie"] == expected and state["frame"] == 2
            command("step 60 280 230 0")
            command("step 60 280 230 1")
            command("step 120 30 200 1")
            state = command("step 1200 30 200 0")
            assert "#shopFloor:[66:" in state["player"]["junk"]
            assert "#Pile1:[29:" in state["player"]["junk"]
            command("step 1 30 200 1")
            state = command("step 3000 30 200 0")
            assert (state["movie"], state["frame"]) == ("03.DXR", 2)
            # Restored sprite callbacks advance the source RNG. Follow the
            # collected part's live position instead of a former random slot.
            part = field(state, 46)
            left, top, right, bottom = part["bounds"]
            x, y = (left + right) // 2, (top + bottom) // 2
            command(f"step 60 {x} {y} 0")
            command(f"step 60 {x} {y} 1")
            command("step 180 350 285 1")
            state = command("step 1200 350 285 0")
            assert "#shopFloor:[66:[350,460]]" in state["player"]["junk"]
            # Source mouseEnter records the held button and suppresses this release.
            command("step 60 320 200 0")
            command("step 1 320 200 1")
            command("step 60 610 120 1")
            state = command("step 600 610 120 0")
            assert state["movie"] == "03.DXR"
            command("step 60 320 200 0")
            state = click(610, 120)
            assert (state["movie"], state["frame"]) == ("06.DXR", 1)
            assert not field(state)["editable"]
            state = click(300, 200)
            assert field(state)["editable"]
            command("text 65 " + "W" * 20)
            state = command("step 120 320 440 0")
            assert 0 < len(field(state)["text"]) < 20  # Original pixel-width check.
            command("text 65 ROADSTER")
            state = command("step 120 320 440 0")
            assert field(state)["text"] == "ROADSTER"
            assert state["objects"]["gdir"]["changedname"] == "1"
            before_save = state["save_generation"]
            state = click(575, 440)
            assert state["movie"] == "03.DXR" and state["save_generation"] > before_save
            assert '#name:"ROADSTER"' in state["player"]["saves"]
            # Reopen an occupied slot and change its name through the source UI.
            click(610, 120)
            state = click(618, 20)
            assert field(state)["editable"] and field(state)["text"] == "ROADSTER"
            command("text 65 RUNABOUT")
            command("step 120 320 440 0")
            state = click(575, 440)
            assert '#name:"RUNABOUT"' in state["player"]["saves"]
            saved_cars = state["player"]["saves"]
            for attempt in range(2):
                if attempt:
                    command("reboot")
                    command("step 18000 320 240 0")
                    command("text 14 WILLY")
                    click(100, 250, 6000)
                    state = click(320, 240, 6000)
                    assert state["movie"] == "03.DXR"
                    assert state["player"]["saves"] == saved_cars
                state = click(409, 55)
                assert (state["movie"], state["frame"]) == ("06.DXR", 13)
                assert not field(state)["editable"] and field(state)["text"] == "RUNABOUT"
                state = click(300, 200)
                assert (state["movie"], state["frame"]) == ("03.DXR", 2)
                assert state["car_name"] == "RUNABOUT"
            click(615, 430)
            state = click(310, 200)
            assert (state["movie"], state["frame"]) == ("08.DXR", 1)
            state = click(610, 375)
            assert "Drucken" in state["notice"]
            command("step 1 610 375 0")
            state = command("step 1 610 375 2")
            assert not state["notice"]
            command("step 60 610 375 0")
            left, top, right, bottom = field(state, 80)["bounds"]
            state = click((left + right) // 2, (top + bottom) // 2)
            assert state["movie"] == "03.DXR"
            click(615, 430)
            state = click(490, 245)
            assert state["movie"] == "03.DXR"
            state = click(610, 120)
            assert state["movie"] == "06.DXR"  # Closing the toolbox re-enables buttons.
            state = click(575, 440)
            assert state["movie"] == "03.DXR"
            # Separate from player car assembly: the game's makeCar(#Good)
            # helper supplies a deterministic drivable car for these checks.
            command("fixture-drive")
            state = command("step 6000 320 200 0")
            assert state["movie"] == "05.DXR"
            # Spelled as the script that named the mode (#Driving); Lingo
            # compares text without case, and so does this.
            assert state["objects"]["gdir"]["mode"].lower() == "driving"
            headings = json.loads(state["driving"]["directionlist"])
            assert len({tuple(point) for point in headings}) == 16
            origin = state["driving"]["coordinate"]
            direction = state["driving"]["direction"]
            state = command("step 120 400 200 1")
            speed = float(state["driving"]["speed"])
            assert speed > 0 and state["driving"]["coordinate"] != origin
            assert state["driving"]["direction"] != direction
            state = command("step 120 400 200 2")
            assert float(state["driving"]["speed"]) < 0
            state = command("step 60 400 200 0")
            assert float(state["driving"]["acceleration"]) == 0
        finally:
            if process.poll() is None:
                process.stdin.write("quit\n")
                process.stdin.flush()
            process.wait(timeout=30)
            (output / "replay.json").write_text(
                json.dumps({"commands": commands, "states": states}, indent=2) + "\n"
            )
        if process.returncode:
            raise ValueError("native journey process failed")
    return {
        "status": "passing",
        "platform": "host-native",
        "source_sha256": game.source["sha256"],
        "scenarios": [
            "create-player",
            "enter-workshop",
            "reboot-login",
            "yard-return",
            "collect-part",
            "workshop-drag",
            "held-button-hover-guard",
            "save-car-and-source-name-width-check",
            "rename-saved-car",
            "load-car",
            "reboot-load-saved-car",
            "toolbox-certificate-and-return",
            "certificate-print-notice-and-dismissal",
            "toolbox-close",
            "source-fixture-driving-acceleration-steering-brake-reverse-release",
        ],
        "driving_fixture": "recovered 05.DXR makeCar(#Good); player assembly not covered",
        "commands": len(commands),
        "replay_sha256": hashlib.sha256((output / "replay.json").read_bytes()).hexdigest(),
        "hardware_qualified": False,
        "original_projector_compared": False,
    }

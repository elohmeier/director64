"""Drive login, name entry, class selection, a room visit and an exercise."""

import hashlib
import json
import os
import subprocess

from director64.project import selected_game

# The scripted journey in authored 800x600 coordinates: boot to the login
# screen, open the first profile line, type a name through the text-entry
# replacement (the on-screen keyboard's commit path), confirm with the green
# arrow, pick class 1, click once to skip the map demo, enter the castle's
# centre room, open its exercise book (the room intro releases the
# clickpoints after its speech, so the book takes two activations), let the
# spelling task write its prompt cards, and pick the first answer.
STEPS = [
    "step 2200 400 300 0",
    "step 10 367 185 0",
    "step 4 367 185 1",
    "step 30 367 185 0",
    "text 13 ANNA",
    "step 120 400 300 0",
    "step 10 473 301 0",
    "step 4 473 301 1",
    "step 900 473 301 0",
    "step 10 285 452 0",
    "step 4 285 452 1",
    "step 60 285 452 0",
    "step 1200 400 300 0",
    "step 4 400 300 1",
    "step 300 400 300 0",
    "step 30 400 300 0",
    "step 4 400 300 1",
    "step 900 400 300 0",
    "step 6 147 486 0",
    "step 4 147 486 1",
    "step 800 147 486 0",
    "step 6 147 486 0",
    "step 4 147 486 1",
    "step 800 147 486 0",
    "step 2500 400 300 0",
    "step 6 426 263 0",
    "step 4 426 263 1",
    "step 600 426 263 0",
]

# States sample at command boundaries; the boot chain inside the first step
# is covered by full_host and the boot capture.
REQUIRED_TRAIL = ["DEUTSCH.DXR", "LOGIN.DXR", "MAINSCR.DXR", "ROOM08.DXR", "TEST2A.DXR"]


def run(executable, output):
    output.mkdir(parents=True, exist_ok=True)
    game = selected_game()
    log_path = output / "journey.log"
    states = []
    with log_path.open("w") as log:
        process = subprocess.Popen(
            [str(executable), "DEUTSCH", "rpc"],
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
                raise ValueError(f"journey process ended early; see {log_path}")
            state = json.loads(line)
            states.append(state)
            if state["error"] or state["quit"]:
                raise ValueError(f"journey failed: {state['error'] or 'quit'}")
            return state

        receive()
        for command in STEPS:
            process.stdin.write(command + "\n")
            process.stdin.flush()
            receive()
        process.stdin.write("quit\n")
        process.stdin.flush()
        process.wait(timeout=60)
    if process.returncode:
        raise ValueError("journey process failed")
    trail = []
    for state in states:
        movie = state.get("movie")
        if movie and (not trail or trail[-1] != movie):
            trail.append(movie)
    for movie in REQUIRED_TRAIL:
        if movie not in trail:
            raise ValueError(f"journey missed {movie}: {trail}")
    final = states[-1]
    if final["script_errors"]:
        raise ValueError(f"journey raised script alerts: {final['last_script_error']}")
    if final["save_generation"] < 1:
        raise ValueError("profile creation persisted nothing to the archive")
    # The exercise's prompts must reach their Flash fields laid out: at least
    # the answer cards, with text and a computed height, and the picked card's
    # Selected state reading back.
    prompts = [f for f in final.get("flash", []) if f.get("field") and f.get("text")]
    if len(prompts) < 2:
        raise ValueError(f"exercise prompts missing from Flash fields: {final.get('flash')}")
    if not all(f.get("textheight", 0) > 0 for f in prompts):
        raise ValueError("exercise prompts have no laid-out height")
    if not any(
        f.get("name", "").lower() == "selected" and f.get("visible")
        for f in final.get("flash", [])
    ):
        raise ValueError("picking an answer card did not select it")
    (output / "states.json").write_text(json.dumps(states, indent=2) + "\n")
    evidence = {
        name: hashlib.sha256((output / name).read_bytes()).hexdigest()
        for name in ("journey.log", "states.json")
    }
    return {
        "status": "passing",
        "scenarios": [
            "login-name-class-demoskip-room",
            "login-name-class-demoskip-exercise",
        ],
        "movie_trail": trail,
        "final_movie": final["movie"],
        "save_generation": final["save_generation"],
        "source_sha256": game.source["sha256"],
        "evidence": evidence,
        "hardware_qualified": False,
    }

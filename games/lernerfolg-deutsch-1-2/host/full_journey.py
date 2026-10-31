"""Drive login, exercises and the authored controller's task selector."""

import hashlib
import json
import os
import subprocess
from contextlib import suppress

from director64.probe import state_line
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

# The recovered controller shortcut: Control+Shift+Q opens its own score.
PANEL_STEPS = [
    "key 59 0 1", "key 56 0 1", "key 12 81 1",
    "step 120 400 300 0",
    "key 12 81 0", "key 56 0 0", "key 59 0 0",
    "step 120 400 300 0",
]

# Sequence Info tab, task-number field, numbered room dropdown and Go button.
# The text token is the visible controller field reported by window_editable;
# it uses the same commit path as the host's text-entry interface.
TASK2CB1_STEPS = [
    "step 6 80 127 0", "step 4 80 127 1", "step 120 80 127 0",
    "step 6 75 493 0", "step 4 75 493 1", "step 40 75 493 0",
    "text 536871017 4", "step 60 75 493 0",
    "step 6 120 425 0", "step 4 120 425 1", "step 30 120 425 0",
    "step 10 120 441 0", "step 4 120 441 1", "step 30 120 441 0",
    "step 6 122 488 0", "step 4 122 488 1", "step 1800 122 488 0",
]

# States sample at command boundaries; the boot chain inside the first step
# is covered by full_host and the boot capture.
REQUIRED_TRAIL = [
    "DEUTSCH.DXR", "LOGIN.DXR", "MAINSCR.DXR", "ROOM08.DXR", "TEST2A.DXR", "TEST2CB1.DXR",
]


def task_layout(state):
    """Check completion of InitFields, beyond simply entering its movie."""
    if state["movie"] != "TEST2CB1.DXR" or state.get("script_errors"):
        raise ValueError("controller did not reach TEST2CB1 with no script alerts")
    task = state.get("objects", {}).get("gobjtask", {})
    try:
        positions = json.loads(task.get("ppositionslist", "null"))
        sentences = json.loads(task.get("p_sentencesprites", "null"))
        fields = json.loads(task.get("p_fieldsprites", "null"))
        backgrounds = json.loads(task.get("p_vfieldssprites", "null"))
    except (TypeError, json.JSONDecodeError) as error:
        raise ValueError("TEST2CB1 field layout is not recorded") from error
    if (not isinstance(positions, list) or not positions
            or not isinstance(sentences, list) or not sentences
            or not isinstance(fields, list) or not isinstance(backgrounds, list)
            or len(fields) != len(positions) or len(backgrounds) != len(positions)):
        raise ValueError("TEST2CB1 InitFields did not complete its field ranges")
    sprites = {sprite["id"]: sprite for sprite in state.get("sprites", [])}
    sentence = sprites.get(sentences[0], {})
    text_length = len(sentence.get("text", "").encode("utf-16-le")) // 2
    for bounds in positions:
        if (not isinstance(bounds, list) or len(bounds) != 2
                or not all(type(value) is int for value in bounds)
                or not 1 <= bounds[0] <= bounds[1] <= text_length):
            raise ValueError("TEST2CB1 has an invalid answer character range")
    for channel in [*sentences, *fields, *backgrounds]:
        sprite = sprites.get(channel, {})
        bounds = sprite.get("bounds", [])
        if (not sprite.get("visible") or len(bounds) != 4
                or bounds[2] <= bounds[0] or bounds[3] <= bounds[1]):
            raise ValueError("TEST2CB1 field geometry is absent or empty")
    if not all(sprites.get(channel, {}).get("editable") for channel in fields):
        raise ValueError("TEST2CB1 answer fields are not editable")
    if not any(field.get("field") and field.get("visible") and field.get("text")
               and field.get("textheight", 0) > 0 for field in state.get("flash", [])):
        raise ValueError("TEST2CB1 prompt has no visible, laid-out Flash text")
    return positions


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
                "DIRECTOR64_PROBE_OBSERVATIONS": "1",
            },
        )

        def receive():
            line = state_line(process.stdout)
            if not line:
                raise ValueError(f"journey process ended early; see {log_path}")
            state = json.loads(line)
            states.append(state)
            if state["error"] or state["quit"]:
                raise ValueError(f"journey failed: {state['error'] or 'quit'}")
            return state

        try:
            receive()
            for command in STEPS:
                process.stdin.write(command + "\n")
                process.stdin.flush()
                receive()
            exercise_index = len(states) - 1
            for command in PANEL_STEPS:
                process.stdin.write(command + "\n")
                process.stdin.flush()
                receive()
            panel_index = len(states) - 1
            for command in TASK2CB1_STEPS:
                process.stdin.write(command + "\n")
                process.stdin.flush()
                receive()
            process.stdin.write("quit\n")
            process.stdin.flush()
            process.wait(timeout=60)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
            for pipe in (process.stdin, process.stdout):
                with suppress(OSError):
                    pipe.close()
            (output / "states.json").write_text(json.dumps(states, indent=2) + "\n")
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
    final = states[exercise_index]
    if final["script_errors"]:
        raise ValueError(f"journey raised script alerts: {final['last_script_error']}")
    if final["save_generation"] < 1:
        raise ValueError("profile creation persisted nothing to the archive")
    # The exercise's prompts must reach their Flash fields laid out: at least
    # the answer cards, with text and a computed height, and the picked card's
    # Selected state reading back. Card_2A uses its authored `sel` frame 2;
    # its common behavior also probes optional Selected/Disabled clips that
    # this particular Flash member does not contain.
    prompts = [f for f in final.get("flash", []) if f.get("field") and f.get("text")]
    if len(prompts) < 2:
        raise ValueError(f"exercise prompts missing from Flash fields: {final.get('flash')}")
    if not all(f.get("textheight", 0) > 0 for f in prompts):
        raise ValueError("exercise prompts have no laid-out height")
    selected_clip = any(
        f.get("name", "").lower() == "selected" and f.get("visible")
        for f in final.get("flash", [])
    )
    before = next((s for s in states[exercise_index - 3]["sprites"] if s["id"] == 20), {})
    after = next((s for s in final["sprites"] if s["id"] == 20), {})
    selected_frame = before.get("flash_frame") == 1 and after.get("flash_frame") == 2
    if not (selected_clip or selected_frame):
        raise ValueError("picking an answer card did not select it")
    panel = states[panel_index]
    if (not panel.get("window_visible") or panel.get("window_frame", 0) <= 2
            or panel.get("window_sprites", 0) < 3):
        raise ValueError("controller shortcut did not reveal its scored statistics panel")
    if panel.get("window_tick") != panel["tick"]:
        raise ValueError("controller clock stopped while the stage was playing")
    if not any(field.get("text") for field in panel.get("window_fields", [])):
        raise ValueError("controller statistics fields were not populated")
    if (panel.get("script_errors") or not panel.get("frame_sha256")
            or panel["frame_sha256"] == final.get("frame_sha256")):
        raise ValueError("controller panel did not produce a clean, changed rendered frame")
    layout = states[-1]
    ranges = task_layout(layout)
    evidence = {
        name: hashlib.sha256((output / name).read_bytes()).hexdigest()
        for name in ("journey.log", "states.json")
    }
    return {
        "status": "passing",
        "scenarios": [
            "login-name-class-demoskip-room",
            "login-name-class-demoskip-exercise",
            "controller-statistics-shortcut",
            "controller-task2cb1-field-layout",
        ],
        "movie_trail": trail,
        "final_movie": layout["movie"],
        "answer_field_count": len(ranges),
        "save_generation": final["save_generation"],
        "source_sha256": game.source["sha256"],
        "evidence": evidence,
        "hardware_qualified": False,
    }

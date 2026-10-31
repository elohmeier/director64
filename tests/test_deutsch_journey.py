"""A failed rendered journey must retain its first state and release its probe."""
import json
import os

import pytest

from director64.project import GameSpec


def layout_state():
    return {
        "movie": "TEST2CB1.DXR", "script_errors": 0,
        "objects": {"gobjtask": {
            "ppositionslist": "[[1,2]]", "p_sentencesprites": "[10]",
            "p_fieldsprites": "[11]", "p_vfieldssprites": "[12]",
        }},
        "sprites": [
            {"id": 10, "text": "abc", "visible": True, "bounds": [0, 0, 30, 12]},
            {"id": 11, "visible": True, "bounds": [0, 0, 20, 12], "editable": True},
            {"id": 12, "visible": True, "bounds": [0, 0, 20, 12]},
        ],
        "flash": [{"field": True, "visible": True, "text": "prompt", "textheight": 12}],
    }


def test_controller_task_layout_requires_completed_ranges_and_geometry():
    journey = GameSpec.load("lernerfolg-deutsch-1-2").host("full_journey")
    state = layout_state()
    assert journey.task_layout(state) == [[1, 2]]
    state["objects"]["gobjtask"]["ppositionslist"] = "[]"
    with pytest.raises(ValueError, match="did not complete"):
        journey.task_layout(state)
    state["objects"]["gobjtask"]["ppositionslist"] = "[[0,2]]"
    with pytest.raises(ValueError, match="character range"):
        journey.task_layout(state)
    state["objects"]["gobjtask"]["ppositionslist"] = "[[1,2]]"
    state["sprites"][2]["bounds"] = [0, 0, 0, 12]
    with pytest.raises(ValueError, match="geometry"):
        journey.task_layout(state)


def test_controller_journey_failure_reaps_probe_and_keeps_state(tmp_path, monkeypatch):
    for key in ("DIRECTOR64_GAME", "DIRECTOR64_SOURCE", "DIRECTOR64_WORK_DIR"):
        monkeypatch.setenv(key, os.environ.get(key, ""))
    game = GameSpec.load("lernerfolg-deutsch-1-2")
    game.activate()
    pid_file = tmp_path / "probe.pid"
    monkeypatch.setenv("DIRECTOR64_FAKE_PID_FILE", str(pid_file))
    probe = tmp_path / "failing-probe"
    probe.write_text(
        "#!/usr/bin/env python3\n"
        "import json, os, pathlib, time\n"
        "pathlib.Path(os.environ['DIRECTOR64_FAKE_PID_FILE']).write_text(str(os.getpid()))\n"
        "print(json.dumps({'error':'synthetic failure','quit':False}),flush=True)\n"
        "while True: time.sleep(1)\n"
    )
    probe.chmod(0o755)
    output = tmp_path / "evidence"
    with pytest.raises(ValueError, match="synthetic failure"):
        game.host("full_journey").run(probe, output)
    assert json.loads((output / "states.json").read_text()) == [
        {"error": "synthetic failure", "quit": False}
    ]
    with pytest.raises(ProcessLookupError):
        os.kill(int(pid_file.read_text()), 0)

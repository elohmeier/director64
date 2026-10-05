"""Reject ambiguous input before the original projector or any capture starts."""

import copy
import importlib.util
import json
from pathlib import Path

import pytest

SPEC = importlib.util.spec_from_file_location(
    "reference_runner",
    Path(__file__).parents[1] / "games/findus-workshop/host" / "reference_runner.py",
)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)

REPLAY = {
    "schema_version": 1,
    "id": "source-boot-v1",
    "clock": "host_monotonic_ms",
    "duration_ms": 2000,
    "actions": [
        {"at_ms": 10, "type": "pointer", "x": 0, "y": 479},
        {"at_ms": 20, "type": "button_down", "button": "left"},
        {"at_ms": 30, "type": "button_up", "button": "left"},
        {"at_ms": 1000, "type": "checkpoint", "id": "boot"},
    ],
}


def read(tmp_path, replay):
    path = tmp_path / "replay.json"
    path.write_text(json.dumps(replay))
    return runner.read_replay(path)


def test_accepts_boundary_pointer_and_ordered_actions(tmp_path):
    assert read(tmp_path, REPLAY) == REPLAY


@pytest.mark.parametrize(
    "field,value",
    [
        ("schema_version", 2),
        ("id", "../boot"),
        ("clock", "guest_ticks"),
        ("duration_ms", True),
        ("duration_ms", 600001),
    ],
)
def test_rejects_unsupported_replay_identity_and_clock(tmp_path, field, value):
    replay = copy.deepcopy(REPLAY)
    replay[field] = value
    with pytest.raises(ValueError):
        read(tmp_path, replay)


@pytest.mark.parametrize(
    "action",
    [
        {"at_ms": 0, "type": "pointer", "x": 640, "y": 0},
        {"at_ms": 0, "type": "pointer", "x": 0, "y": -1},
        {"at_ms": 0, "type": "pointer", "x": True, "y": 0},
        {"at_ms": 0, "type": "shell", "command": "true"},
        {"at_ms": 0, "type": "key", "key": "ctrl+alt+Delete"},
        {"at_ms": 0, "type": "checkpoint", "id": "../escape"},
        {"at_ms": 2001, "type": "checkpoint", "id": "late"},
    ],
)
def test_rejects_unsafe_or_unrepresentable_actions(tmp_path, action):
    replay = copy.deepcopy(REPLAY)
    replay["actions"].insert(0, action)
    with pytest.raises(ValueError):
        read(tmp_path, replay)


def test_rejects_reordered_actions_and_duplicate_checkpoints(tmp_path):
    replay = copy.deepcopy(REPLAY)
    replay["actions"].append(replay["actions"][0])
    with pytest.raises(ValueError, match="timestamp order"):
        read(tmp_path, replay)
    replay["actions"][-1] = replay["actions"][-2]
    with pytest.raises(ValueError, match="unique"):
        read(tmp_path, replay)


def test_requires_at_least_one_observation(tmp_path):
    replay = copy.deepcopy(REPLAY)
    replay["actions"] = []
    with pytest.raises(ValueError, match="include a checkpoint"):
        read(tmp_path, replay)


def test_does_not_silently_ignore_seed_or_expected_semantic_fields(tmp_path):
    replay = copy.deepcopy(REPLAY)
    replay["random_seed"] = 17
    with pytest.raises(ValueError, match="unsupported fields"):
        read(tmp_path, replay)
    del replay["random_seed"]
    replay["actions"][-1]["expected_movie"] = "PINTRO"
    with pytest.raises(ValueError, match="unsupported fields"):
        read(tmp_path, replay)

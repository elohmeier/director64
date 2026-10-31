"""Equivalent input batching must preserve images, state and audio ordering."""

import json

import pytest

from director64 import metamorphic


def test_replay_rejects_injected_state_and_preserves_sessions(tmp_path):
    path = tmp_path / "replay.json"
    path.write_text(json.dumps([{"commands": ["step 4 780 590 0", "text 2 Rex"]},
                               {"commands": ["reboot", "key 36 13 1"]}]))
    assert len(metamorphic.sessions(path)) == 2
    for commands in (["movie SECRET"], ["step 100001 1 1 0"], ["step 1 1 1 0\nstate"], []):
        path.write_text(json.dumps({"commands": commands}))
        with pytest.raises(ValueError):
            metamorphic.sessions(path)


def test_split_preserves_held_input_and_does_not_repeat_key_edges():
    assert metamorphic.chunks("step 3 780 590 1", True) == ["step 1 780 590 1"] * 3
    assert metamorphic.chunks("pad 2 -128 127 1", True) == ["pad 1 -128 127 1"] * 2
    assert metamorphic.chunks("key 36 13 1", True) == ["key 36 13 1"]
    assert metamorphic.chunks("step 3 780 590 1", False) == ["step 3 780 590 1"]


class Probe:
    state = {}

    def __init__(self, fault=""):
        self.tick = 0
        self.fault = fault

    def send(self, command, ticks):
        start = self.tick
        self.tick += ticks
        audio = [["cue", i] for i in range(start, self.tick) if i % 3 == 0]
        if self.fault == "audio" and ticks > 1:
            audio = []
        return {"tick": self.tick, "frame_sha256": ("a" if self.fault != "pixels" else "b") * 64,
                "frame_trace": [str(i) if self.fault != "transient" else "broken"
                                for i in range(start, self.tick)],
                "audio_events": audio, "globals": {"name": "Rex"}}

    def renumbered(self, state):
        return dict(state)


def test_temporal_oracle_detects_pixels_and_dropped_audio_despite_equal_game_state():
    command = "step 6 780 590 1"
    expected = metamorphic.observe(Probe(), [command])
    assert expected == metamorphic.observe(Probe(), metamorphic.chunks(command, True))
    for fault in ("audio", "pixels", "transient"):
        actual = metamorphic.observe(Probe(fault), [command])
        assert actual["globals"] == expected["globals"]
        assert metamorphic.normalized(actual) != metamorphic.normalized(expected)

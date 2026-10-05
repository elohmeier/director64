import copy
import json

import pytest
from director64_findus_workshop.full_replay import generate, generate_controller


def report(tmp_path, commands=None, passed=True, target="MOSSEN"):
    path = tmp_path / "report.json"
    path.write_text(
        json.dumps(
            [
                {
                    "target": target,
                    "passed": passed,
                    "visited": [f"ENTER {target}.DXR frame=1 tick=0"],
                    "commands": commands or ["step 20 320 240 0"],
                    "state": {
                        "movie": "BYRAN.DXR",
                        "frame": 2,
                        "globals": {"gantalguldfeather": "1"},
                        "save_contents": ["1\r"] * 9,
                    },
                }
            ]
        )
    )
    return path


def test_feedback_is_explicit_and_does_not_replace_source_save_expectations(tmp_path):
    path = report(tmp_path)
    generate(path, "MOSSEN", tmp_path / "out", adaptive=True)
    result = json.loads((tmp_path / "out/replay.json").read_text())
    assert result["controller"] == "feedback-mouse"
    assert result["ticks"] == 12020
    assert len(result["expected_saves"]) == 9
    assert result["expected_saves"][0]["bytes"] == 2
    assert "FULL_REPLAY_ADAPTIVE_MOSSEN" in (tmp_path / "out/replay.inc").read_text()


@pytest.mark.parametrize(
    "command", ["reboot", "set reward 1", "step 0 0 0 0", "step 2 640 0 0", "step 2 0 0 2"]
)
def test_reject_non_input_and_invalid_replay(tmp_path, command):
    with pytest.raises(ValueError):
        generate(report(tmp_path, [command]), "MOSSEN", tmp_path / "out")


def test_reject_failed_host_evidence(tmp_path):
    with pytest.raises(ValueError, match="failing"):
        generate(report(tmp_path, passed=False), "MOSSEN", tmp_path / "out")


@pytest.mark.parametrize("sounds,expected", [(0, "silent"), (1, "audible"), (12, "audible")])
def test_audio_expectation_follows_native_journey(tmp_path, sounds, expected):
    path = report(tmp_path, target="HALLTYST")
    entries = json.loads(path.read_text())
    entries[0]["state"]["sounds"] = sounds
    path.write_text(json.dumps(entries))
    generate(path, "HALLTYST", tmp_path / "out")
    result = json.loads((tmp_path / "out/replay.json").read_text())
    assert result["expected_audio"] == expected


def test_controller_replay_preserves_buttons_and_signed_stick(tmp_path):
    path = tmp_path / "pad.json"
    path.write_text(
        json.dumps(
            {
                "status": "passing",
                "reports": [
                    {
                        "name": "pilot",
                        "movie": "VAKT",
                        "commands": ["pad 1 0 0 64", "pad 30 -70 70 1"],
                        "state": {
                            "movie": "PINTRO.DXR",
                            "frame": 33,
                            "tick": 20,
                            "save_contents": ["profile"] * 9,
                        },
                    }
                ],
            }
        )
    )
    result = generate_controller(path, "pilot", tmp_path / "out")
    assert result["controller"] == "recorded-gamepad" and result["ticks"] == 31
    assert result["expected_game_ticks"] == 20
    assert len(result["expected_saves"]) == 9
    code = (tmp_path / "out/replay.inc").read_text()
    assert "#define FULL_REPLAY_CONTROLLER 1" in code
    assert "{1,0,0,64}" in code and "{30,-70,70,1}" in code


@pytest.mark.parametrize(
    "command", ["step 1 0 0 0", "pad 0 0 0 0", "pad 1 128 0 0", "pad 1 0 0 128", "set focus 47"]
)
def test_controller_replay_rejects_non_controller_samples(tmp_path, command):
    path = tmp_path / "pad.json"
    path.write_text(
        json.dumps(
            {
                "status": "passing",
                "reports": [
                    {
                        "name": "pilot",
                        "movie": "VAKT",
                        "commands": [command],
                        "state": {"movie": "PINTRO.DXR", "frame": 33},
                    }
                ],
            }
        )
    )
    with pytest.raises(ValueError):
        generate_controller(path, "pilot", tmp_path / "out")


@pytest.mark.parametrize("mode", [1, 2, 3])
def test_feedback_feather_difficulty_is_explicit(tmp_path, mode):
    target = f"PLOCKSPL:{mode}"
    generate(report(tmp_path, target=target), target, tmp_path / "out", adaptive=True)
    assert f"FULL_REPLAY_ADAPTIVE_PLOCK {mode}" in (tmp_path / "out/replay.inc").read_text()


def test_feedback_rejects_undefined_target(tmp_path):
    with pytest.raises(ValueError, match="only defined"):
        generate(report(tmp_path, target="HYVEL"), "HYVEL", tmp_path / "out", adaptive=True)


def test_feedback_cans_retains_reward_and_save_oracle(tmp_path):
    path = report(tmp_path, target="FINNDUNK")
    generate(path, "FINNDUNK", tmp_path / "out", adaptive=True)
    result = json.loads((tmp_path / "out/replay.json").read_text())
    assert result["expected_feathers"] == 1
    assert len(result["expected_saves"]) == 9
    assert "FULL_REPLAY_ADAPTIVE_CANS" in (tmp_path / "out/replay.inc").read_text()


@pytest.mark.parametrize("mode", [1, 2, 3])
def test_feedback_memory_difficulty_is_explicit(tmp_path, mode):
    target = f"VEMORY:{mode}"
    generate(report(tmp_path, target=target), target, tmp_path / "out", adaptive=True)
    assert f"FULL_REPLAY_ADAPTIVE_VEMORY {mode}" in (tmp_path / "out/replay.inc").read_text()


def test_map_feedback_keeps_the_host_save_expectations(tmp_path):
    generate(report(tmp_path, target="SNIKBOD"), "SNIKBOD", tmp_path / "out", adaptive=True)
    assert "FULL_REPLAY_ADAPTIVE_MAP" in (tmp_path / "out/replay.inc").read_text()
    result = json.loads((tmp_path / "out/replay.json").read_text())
    assert len(result["expected_saves"]) == 9
    assert result["expected_saves"][0]["bytes"] == 2


def map_oracles(tmp_path):
    path = report(tmp_path, target="SNIKBOD")
    first = json.loads(path.read_text())[0]
    first["state"]["save_contents"] = ["0\r0\r[34,0]\r[1,1,1,1,1,1,1,1,1]\r"] + [""] * 8
    second = copy.deepcopy(first)
    second["state"]["save_contents"][0] = second["state"]["save_contents"][0].replace("34", "35")
    return path, [first, second]


def test_two_source_treasures_remain_byte_exact_oracles(tmp_path):
    path, entries = map_oracles(tmp_path)
    path.write_text(json.dumps(entries))
    generate(path, "SNIKBOD", tmp_path / "out", adaptive=True)
    result = json.loads((tmp_path / "out/replay.json").read_text())
    alternatives = result["expected_save_alternatives"]
    assert len(alternatives) == 1 and len(alternatives[0]) == 9
    assert alternatives[0][0] != result["expected_saves"][0]
    assert alternatives[0][1:] == result["expected_saves"][1:]


@pytest.mark.parametrize("change", ["missing_fragment", "other_file", "failed", "non_input"])
def test_map_oracles_cannot_relax_progress_or_persistence(tmp_path, change):
    path, entries = map_oracles(tmp_path)
    second = entries[1]
    if change == "missing_fragment":
        second["state"]["save_contents"][0] = second["state"]["save_contents"][0].replace(
            "1,1,1,1,1,1,1,1,1", "1,1,1,1,1,0,1,1,1"
        )
    elif change == "other_file":
        second["state"]["save_contents"][8] = "different music"
    elif change == "failed":
        second["passed"] = False
    else:
        second["commands"] = ["set reward 1"]
    path.write_text(json.dumps(entries))
    with pytest.raises(ValueError):
        generate(path, "SNIKBOD", tmp_path / "out", adaptive=True)

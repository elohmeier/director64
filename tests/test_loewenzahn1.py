import importlib.util
import sys
from pathlib import Path

import pytest

from director64.captures import CaptureError


def boot_module():
    # Load this adapter without changing the process-wide selected game.
    path = Path(__file__).parents[1] / "games/loewenzahn-1/host"
    package = "loewenzahn_test_host"
    if package not in sys.modules:
        spec = importlib.util.spec_from_file_location(
            package, path / "__init__.py", submodule_search_locations=[str(path)]
        )
        module = importlib.util.module_from_spec(spec)
        sys.modules[package] = module
        spec.loader.exec_module(module)
    return __import__(package + ".full_boot", fromlist=["full_boot"])


@pytest.mark.parametrize("first_stop", [None, 2550, 16660, 16661])
def test_intro_validation_requires_both_full_source_durations(first_stop):
    model = {
        "movies": [
            {
                "name": "INTRO.DXR",
                "id": 8,
                "members": [
                    {"name": "INTRO_1.MOV", "number": 4, "cast": 1, "frames": 16661},
                    {"name": "INTRO_2.MOV", "number": 7, "cast": 1, "frames": 14960},
                ],
            }
        ]
    }
    log = "VIDEO_OPEN sprite=2 member=8454148\nVIDEO_OPEN sprite=2 member=8454151\n"
    if first_stop is not None:
        log += f"VIDEO_STOP member=8454148 time={first_stop} duration=16661\n"
    log += "VIDEO_STOP member=8454151 time=14960 duration=14960\n"
    validate = boot_module().validate_intro_stops
    if first_stop == 16661:
        assert len(validate(log, model)) == 2
    else:
        with pytest.raises(CaptureError, match="INTRO_1.MOV"):
            validate(log, model)


@pytest.mark.parametrize("stop", [None, 0, 43199, 43200])
def test_hammer_validation_rejects_early_reward_stop(stop):
    regressions = __import__(
        boot_module().__package__ + ".full_regressions", fromlist=["full_regressions"]
    )
    checkpoint = {"video_member": 5308433, "video_duration": 43200}
    log = "" if stop is None else f"VIDEO_STOP member=5308433 time={stop} duration=43200\n"
    if stop == 43200:
        regressions.validate_reward_stop(log, checkpoint)
    else:
        with pytest.raises(ValueError, match="full duration"):
            regressions.validate_reward_stop(log, checkpoint)

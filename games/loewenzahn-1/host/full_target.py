"""Capture an isolated pointer probe and require its final checkpoint."""

import argparse
import json

from director64.project import selected_game

from .full_boot import capture
from .full_journey import ROUTES
from .full_regressions import SCENARIOS, validate_reward_stop


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "target", choices=[*ROUTES, *SCENARIOS, "PRINT", "CURSORS"], nargs="?", default="HOL"
    )
    args = parser.parse_args()
    game = selected_game()
    target = args.target
    if target == "CURSORS":
        path, report = capture(
            game,
            profile="probe",
            duration=90,
            marker="DIRECTOR64 SCENE_RENDERED name=PANO.DXR",
            completion="DIRECTOR64 REPLAY_COMPLETE id=CURSORS movie=PANO.DXR",
            required=("START.DXR", "INTRO.DXR", "PANO.DXR", "DIALOG.DXR"),
        )
        native = json.loads((game.work / "native/cursors/validation.json").read_text())
        log = (path / "emulator.log").read_text()
        for name, cursor in native["cursors"].items():
            x, y = cursor["hotspot"]
            marker = (
                f"NATIVE_CURSOR_LOAD image={cursor['image']} mask={cursor['mask']} "
                f"hotspot={x},{y} hash={cursor['hash']}"
            )
            if marker not in log:
                raise ValueError(f"missing matching N64 cursor bitmap: {name}")
        if log.count("DIRECTOR64 REPLAY_CURSOR ") != len(native["checks"]):
            raise ValueError("cursor replay did not assert every native checkpoint")
        report["cursors"] = native["cursors"]
        report["cursor_checks"] = len(native["checks"])
        (path / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"CURSORS probe passed: {path} ({report['rom']['rom_sha256']})")
        return
    if target in SCENARIOS:
        movie = {"HAMMER": "HAM.DXR", "FILMSPEED": "FIL.DXR", "RADIO": "PANO.DXR"}[target]
        marker = (
            f"DIRECTOR64 REGRESSION_CHECK id={target}"
            if target == "RADIO"
            else f"DIRECTOR64 SCENE_RENDERED name={movie}"
        )
        path, report = capture(
            game,
            profile="probe",
            marker=marker,
            duration={"HAMMER": 140, "FILMSPEED": 90, "RADIO": 25}[target],
            completion=f"DIRECTOR64 REPLAY_COMPLETE id={target} movie=PANO.DXR",
            required=tuple(dict.fromkeys(("START.DXR", "INTRO.DXR", "PANO.DXR", movie))),
        )
        log = (path / "emulator.log").read_text()
        if f"DIRECTOR64 REGRESSION_CHECK id={target}" not in log:
            raise ValueError("missing regression state assertions")
        native = json.loads((game.work / "native/regressions/validation.json").read_text())
        if target == "HAMMER":
            validate_reward_stop(log, native["checkpoints"][target])
        report["regression"] = {
            "id": target,
            "replay_sha256": native["replay_sha256"][target],
            "state_assertions": log.count(f"DIRECTOR64 REGRESSION_CHECK id={target}"),
        }
        (path / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"{target} regression probe passed: {path} ({report['rom']['rom_sha256']})")
        return
    if target == "PRINT":
        path, report = capture(
            game,
            profile="probe",
            duration=40,
            completion="DIRECTOR64 REPLAY_COMPLETE id=PRINT movie=PANO.DXR",
            marker="DIRECTOR64 PRINT_OPEN id=8",
            required=("START.DXR", "INTRO.DXR", "PANO.DXR", "UNK.DXR", "BAS.DXR"),
        )
        log = (path / "emulator.log").read_text()
        for action in ("OPEN", "CLOSE"):
            for document in (8, 7):
                if f"DIRECTOR64 PRINT_{action} id={document} " not in log:
                    raise ValueError(f"missing print {action} checkpoint {document}")
        print(f"PRINT QR/return probe passed: {path} ({report['rom']['rom_sha256']})")
        return
    marker = "DIALOG.DXR" if target == "HOL" else "BAS.DXR" if target == "UNK" else target + ".DXR"
    required = tuple(dict.fromkeys(("START.DXR", "INTRO.DXR", "PANO.DXR", target + ".DXR", marker)))
    path, report = capture(
        game,
        profile="probe",
        duration=120 if target == "HOL" else 30 if target == "UNK" else 90,
        completion=f"DIRECTOR64 REPLAY_COMPLETE id={target} movie=PANO.DXR",
        marker=f"DIRECTOR64 SCENE_RENDERED name={marker}",
        required=required,
    )
    print(f"{target} topic/return probe passed: {report['rom']['rom_sha256']}")

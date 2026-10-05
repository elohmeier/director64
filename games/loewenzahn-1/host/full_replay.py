"""Compile a passing native pointer journey into an isolated probe ROM."""

import argparse
import hashlib
import json

from director64.aot import cstring
from director64.project import selected_game

from .full_journey import ROUTES
from .full_regressions import SCENARIOS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "target", choices=[*ROUTES, *SCENARIOS, "PRINT", "CURSORS"], nargs="?", default="HOL"
    )
    args = parser.parse_args()
    game = selected_game()
    root = (
        game.work
        / "native"
        / (
            "cursors"
            if args.target == "CURSORS"
            else "printing"
            if args.target == "PRINT"
            else "regressions"
            if args.target in SCENARIOS
            else "journey"
        )
    )
    report = json.loads((root / "validation.json").read_text())
    if report.get("status") != "passing" or not report.get("sanitizers"):
        raise ValueError("probe requires the passing sanitized native journey")
    if report["source_sha256"] != game.source["sha256"]:
        raise ValueError("native journey source differs from selected source")
    replay_bytes = (root / args.target / "replay.json").read_bytes()
    if hashlib.sha256(replay_bytes).hexdigest() != report["replay_sha256"][args.target]:
        raise ValueError("native pointer replay changed after validation")
    replay = json.loads(replay_bytes)
    if len(replay["states"]) != len(replay["commands"]) + 1:
        raise ValueError("incomplete native checkpoint sequence")
    replay_hash = cstring(hashlib.sha256(replay_bytes).hexdigest())
    lines = [
        f"static const char replay_id[]={cstring(args.target)};",
        f"static const char replay_source_sha256[]={replay_hash};",
        "static const replay_step_t replay_steps[]={",
    ]
    for command, state in zip(replay["commands"], replay["states"][1:], strict=True):
        operation, ticks, x, y, down = command.split()
        ticks, x, y, down = map(int, (ticks, x, y, down))
        if (
            operation != "step"
            or not 0 < ticks <= 18000
            or not 0 <= x < 640
            or not 0 <= y < 480
            or down not in (0, 1, 2)
            or state["error"]
            or state["quit"]
            or len(state.get("channel_volumes", [])) != 4
            or any(not 0 <= v <= 255 for v in state["channel_volumes"])
        ):
            raise ValueError("invalid pointer-only native replay")
        volumes = ",".join(str(v) for v in state["channel_volumes"])
        checks = state.get("checks", {})
        tempo = checks.get("tempo", 0)
        sprite, sprite_member = checks.get("sprite", [0, 0])
        sound, sound_member = checks.get("sound", [0, 0])
        cursor = checks.get("cursor", [])
        if (
            set(checks) - {"tempo", "sprite", "sound", "cursor"}
            or not 0 <= tempo <= 120
            or not 0 <= sprite <= 48
            or not 0 <= sound <= 4
            or not 0 <= sprite_member < 2**32
            or not 0 <= sound_member < 2**32
            or (
                cursor
                and (
                    len(cursor) != 5
                    or cursor[0] not in (-1, 0, 1, 2, 3, 4, 200)
                    or any(not 0 <= n < 2**32 for n in cursor[1:3])
                    or any(not 0 <= n < 16 for n in cursor[3:])
                )
            )
        ):
            raise ValueError("invalid regression checkpoint")
        lines.append(
            f"{{.ticks={ticks},.x={x},.y={y},.down={down},.movie={cstring(state['movie'])},"
            f".volumes={{{volumes}}},.tempo={tempo},.sprite={sprite},.sprite_member={sprite_member},"
            f".sound_channel={sound},.sound_member={sound_member},"
            f".check_cursor={int(bool(cursor))},"
            f".cursor={{{','.join(map(str, cursor[:3] or [0, 0, 0]))}}},"
            f".hot_x={cursor[3] if cursor else 0},.hot_y={cursor[4] if cursor else 0}}},"
        )
    lines.append("};")
    (game.work / "director/c/replay.inc").write_text("\n".join(lines) + "\n")
    print(f"Prepared {args.target} pointer replay ({len(replay['commands'])} checkpoints)")

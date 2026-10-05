"""Compile a passing input-only host scenario into the separate N64 probe ROM."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from director64.aot import cstring
from director64.project import work_dir


def save_hashes(contents):
    return [
        {"bytes": len(s.encode("latin1")), "sha256": hashlib.sha256(s.encode("latin1")).hexdigest()}
        for s in contents
    ]


def map_save_shape(contents):
    if len(contents) != 9:
        raise ValueError("map oracle must contain all nine files")
    result = list(contents)
    marker = next((f"[{n}," for n in (34, 35) if f"[{n}," in result[0]), None)
    if marker is None:
        raise ValueError("map oracle lacks the source special treasure")
    result[0] = result[0].replace(marker, "[SPECIAL,", 1)
    return result


def generate(report: Path, target: str, output: Path, adaptive: bool = False):
    entries = json.loads(report.read_text())
    matches = [e for e in entries if e["target"] == target]
    entry = matches[0]
    if not entry["passed"]:
        raise ValueError("cannot replay a failing host scenario as validation")
    movie = entry["visited"][0].split()[1]
    steps = []
    for command in entry["commands"]:
        op, ticks, x, y, down = command.split()
        ticks, x, y, down = map(int, (ticks, x, y, down))
        if (
            op != "step"
            or not 1 <= ticks <= 100000
            or not 0 <= x < 640
            or not 0 <= y < 480
            or down not in (0, 1)
        ):
            raise ValueError("unsafe/non-input replay command")
        steps.append((ticks, x, y, down))
    total = sum(s[0] for s in steps)
    controller_define = ""
    if adaptive:
        if target == "MOSSEN":
            controller_define = "#define FULL_REPLAY_ADAPTIVE_MOSSEN 1\n"
        elif target == "FINNDUNK":
            controller_define = "#define FULL_REPLAY_ADAPTIVE_CANS 1\n"
        elif target == "SNIKBOD":
            controller_define = "#define FULL_REPLAY_ADAPTIVE_MAP 1\n"
        elif target in {"PLOCKSPL:1", "PLOCKSPL:2", "PLOCKSPL:3"}:
            controller_define = f"#define FULL_REPLAY_ADAPTIVE_PLOCK {target[-1]}\n"
        elif target in {"VEMORY:1", "VEMORY:2", "VEMORY:3"}:
            controller_define = f"#define FULL_REPLAY_ADAPTIVE_VEMORY {target[-1]}\n"
        else:
            raise ValueError(
                "feedback controller is only defined for MOSSEN, FINNDUNK, SNIKBOD, "
                "PLOCKSPL:1..3 and VEMORY:1..3"
            )
        total += 12000
        steps = [(total, 620, 20, 0)]
    if not 0 < total < 1000000:
        raise ValueError("replay duration out of range")
    sha = hashlib.sha256(report.read_bytes()).hexdigest()
    alternatives = []
    for oracle in matches[1:]:
        if target != "SNIKBOD" or not oracle["passed"]:
            raise ValueError("only passing source map outcomes can be alternate save oracles")
        if (oracle["state"]["movie"], oracle["state"]["frame"]) != (
            entry["state"]["movie"],
            entry["state"]["frame"],
        ) or map_save_shape(oracle["state"]["save_contents"]) != map_save_shape(
            entry["state"]["save_contents"]
        ):
            raise ValueError("map oracles may differ only in special treasure 34 versus 35")
        for command in oracle["commands"]:
            op, ticks, x, y, down = command.split()
            if (
                op != "step"
                or not 1 <= int(ticks) <= 100000
                or not 0 <= int(x) < 640
                or not 0 <= int(y) < 480
                or int(down) not in (0, 1)
            ):
                raise ValueError("non-input alternate oracle")
        alternatives.append(save_hashes(oracle["state"]["save_contents"]))
    output.mkdir(parents=True, exist_ok=True)
    (output / "replay.inc").write_text(
        controller_define + f"static const char replay_id[]={cstring(target)};\n"
        f"static const char replay_initial_movie[]={cstring(movie)};\n"
        f"static const char replay_source_sha256[]={cstring(sha)};\n"
        "static const replay_step_t replay_steps[]={\n"
        + ",\n".join("{" + ",".join(map(str, s)) + "}" for s in steps)
        + "\n};\n"
    )
    manifest = {
        "version": 1,
        "controller": "feedback-mouse" if adaptive else "recorded-mouse",
        "target": target,
        "report_sha256": sha,
        "initial_movie": movie,
        "ticks": total,
        "expected_movie": entry["state"]["movie"],
        "expected_frame": entry["state"]["frame"],
        "expected_feathers": int(entry["state"]["globals"].get("gAntalGuldFeather".lower(), "0")),
        # A journey with no native audio callbacks must remain silent on N64
        # (e.g. the credits route skips HALLTYST's earlier dialogue frames).
        "expected_audio": "silent" if entry["state"].get("sounds", 1) == 0 else "audible",
        "expected_saves": save_hashes(entry["state"].get("save_contents", [])),
        "expected_save_alternatives": alternatives,
    }
    (output / "replay.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest))


def generate_controller(report: Path, target: str, output: Path):
    """Preserve raw stick/button input exactly as the controller recorded it."""
    data = json.loads(report.read_text())
    if data.get("status") != "passing":
        raise ValueError("cannot replay failing controller evidence")
    entry = next(e for e in data["reports"] if e["name"] == target)
    steps = []
    for command in entry["commands"]:
        op, ticks, x, y, buttons = command.split()
        ticks, x, y, buttons = map(int, (ticks, x, y, buttons))
        if (
            op != "pad"
            or not 1 <= ticks <= 100000
            or not -128 <= x <= 127
            or not -128 <= y <= 127
            or not 0 <= buttons < 128
        ):
            raise ValueError("invalid controller sample")
        steps.append((ticks, x, y, buttons))
    total = sum(s[0] for s in steps)
    if not 0 < total < 1000000:
        raise ValueError("controller duration out of range")
    sha = hashlib.sha256(report.read_bytes()).hexdigest()
    movie = entry["movie"]
    if not movie.endswith(".DXR"):
        movie += ".DXR"
    output.mkdir(parents=True, exist_ok=True)
    (output / "replay.inc").write_text(
        "#define FULL_REPLAY_CONTROLLER 1\n"
        f"static const char replay_id[]={cstring(target)};\n"
        f"static const char replay_initial_movie[]={cstring(movie)};\n"
        f"static const char replay_source_sha256[]={cstring(sha)};\n"
        "static const replay_step_t replay_steps[]={\n"
        + ",\n".join("{" + ",".join(map(str, s)) + "}" for s in steps)
        + "\n};\n"
    )
    manifest = {
        "controller": "recorded-gamepad",
        "target": target,
        "ticks": total,
        "expected_movie": entry["state"]["movie"],
        "expected_frame": entry["state"]["frame"],
        "report_sha256": sha,
        "expected_game_ticks": entry["state"].get("tick", total),
        "expected_feathers": int(entry["state"].get("globals", {}).get("gantalguldfeather", "0")),
        "expected_saves": save_hashes(entry["state"].get("save_contents", [])),
    }
    (output / "replay.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("target")
    parser.add_argument("--output", type=Path, default=Path(f"{work_dir()}/director/c"))
    parser.add_argument("--adaptive", action="store_true")
    parser.add_argument("--controller", action="store_true", help="replay raw gamepad evidence")
    args = parser.parse_args()
    if args.controller:
        if args.adaptive:
            parser.error("--controller and --adaptive are separate input formats")
        generate_controller(args.report, args.target, args.output)
    else:
        generate(args.report, args.target, args.output, args.adaptive)


if __name__ == "__main__":
    main()

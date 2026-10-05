"""Drive source startup, saves, and the clock puzzle using pointer input."""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
from pathlib import Path
from time import time_ns

# Deterministic spine for the perf sweep: boot through the login screen to
# the house hub, then into the bedroom's train station — the scene with the
# reported long-session slowdown. ("click", sprite) entries resolve from the
# live state; the sprite ids match full_regressions.ROUTES ("bedroom-ts").
STEPS = [
    "step 18000 320 240 0",
    ("click", 17),
    "step 600 320 240 0",
    ("click", 40),
    "step 2400 320 240 0",
    ("click", 14),
    "step 600 320 240 0",
    ("click", 12),
    "step 6000 320 240 0",
]


def clock_minutes(setting: str) -> int:
    """Minutes past twelve for a clock property's authored ``[hour,minute]``."""
    hour, minute = (int(part) for part in setting.strip("[]").split(","))
    return (hour % 12) * 60 + minute


def run(executable: Path, output: Path, activity: bool = False):
    output.mkdir(parents=True, exist_ok=True)
    states = []
    commands = []
    checkpoints = {}
    failure = None
    flash_path = output / f"journey-flash-{time_ns()}.fla"
    flash_export = None
    with (output / "journey.log").open("w") as log:
        process = subprocess.Popen(
            [str(executable), "START", "rpc"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=log,
            text=True,
            env=os.environ
            | {
                "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
                "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=1",
                "DIRECTOR64_FLASH_EXPORT": str(flash_path.resolve()),
            },
        )

        def read():
            line = process.stdout.readline()
            if not line:
                raise ValueError(f"native probe exited: inspect {output / 'journey.log'}")
            state = json.loads(line)
            states.append(state)
            if state["error"]:
                raise ValueError(state["error"])
            return state

        def command(text):
            commands.append(text)
            process.stdin.write(text + "\n")
            process.stdin.flush()
            return read()

        def step(ticks, x=320, y=240, down=0):
            return command(f"step {ticks} {x} {y} {down}")

        def click(state, sprite):
            item = next(s for s in state["sprites"] if s["id"] == sprite)
            left, top, right, bottom = item["bounds"]
            x, y = (left + right) // 2, (top + bottom) // 2
            if not (0 <= x < 640 and 0 <= y < 480):
                raise ValueError(f"source control {sprite} outside stage")
            step(1, x, y, 1)
            return step(120, x, y, 0)

        try:
            read()
            state = step(18000)
            if (
                state["movie"] != "LO.DXR"
                or state["globals"].get("lo_state", "").lower() != "valjer"
            ):
                raise ValueError("original startup did not reach player selection")
            state = click(state, 17)
            # Lingo symbols compare without case; the state prints the corpus's
            # canonical spelling (#skriverNamn).
            if state["globals"].get("lo_state", "").lower() != "skrivernamn":
                raise ValueError("selecting first figure did not open name entry")
            state = click(state, 40)
            state = step(2400)
            if state["movie"] != "HUSET.DXR" or not state["save_generation"]:
                raise ValueError("source default-name player creation did not reach house/save")
            first = state["globals"].get("gen_minspelare")
            command("reboot")
            state = step(18000)
            state = click(state, 17)
            state = step(2400)
            if state["movie"] != "HUSET.DXR" or state["globals"].get("gen_minspelare") != first:
                raise ValueError("saved player did not survive a native reboot")
            if activity:
                state = click(state, 16)
                state = step(6000)
                if state["movie"] != "KP.DXR" or state["globals"].get("gspelstatus") != "3":
                    raise ValueError("house clock activity did not reach its start control")
                # KP:setmuckelklockorna starts the clocks at the meeting time and
                # deals each Muckla a route. Which meeting and which routes is a
                # seeded choice, and how many interpreter steps run before that
                # draw is not a property of this port, so the journey solves the
                # puzzle it is dealt rather than one recorded set: press the
                # five-minute back arrow route/5 times and require the clock to
                # have moved back by exactly the route. Every command below is a
                # physical pointer press/release.
                meeting = clock_minutes(state["objects"]["gklockamain"]["pinstalldtid"])
                for index, sprite in enumerate((71, 81, 91), 1):
                    duration = int(state["objects"][f"gmuckla{index}"]["phurlangtidtillvagskalet"])
                    if duration <= 0 or duration % 5:
                        raise ValueError(f"route {index} is not reachable by the five-minute arrow")
                    state = click(state, sprite)
                    for _ in range(duration // 5):
                        state = click(state, 101)
                    departure = clock_minutes(state["objects"][f"gklocka{index}"]["pinstalldtid"])
                    if (meeting - departure) % (12 * 60) != duration:
                        raise ValueError(f"source clock controls did not set departure {index}")
                checkpoints["departures"] = {
                    f"gklocka{index}": state["objects"][f"gklocka{index}"]["pinstalldtid"]
                    for index in range(1, 4)
                }
                step(1, 10, 400, 1)
                state = step(120, 10, 400, 0)
                inventory_before = state["globals"]["ginventorycontent"]
                generation_before = state["save_generation"]
                state = click(state, 53)
                # KP:hurmangakrockar records collisions at the meeting time;
                # mainloop requires all three before entering its victory path.
                for _ in range(120):
                    state = step(60)
                    if state["globals"].get("gantalharkrockatparatttid") == "3":
                        break
                else:
                    raise ValueError("clock puzzle did not record three on-time collisions")
                if (
                    state["movie"] != "KP.DXR"
                    or state["globals"].get("gantalharkrockatpafeltid") != "0"
                ):
                    raise ValueError("clock puzzle recorded a collision outside the meeting time")
                checkpoints["victory"] = {
                    "tick": state["tick"],
                    "on_time_collisions": 3,
                    "wrong_time_collisions": 0,
                }
                # KP:gevinst -> GENMODUL:genGeVinst adds food/a card, creates
                # the reward animation, then sparaVariabler saves preferences.
                for _ in range(120):
                    state = step(60)
                    if (
                        state["globals"].get("gspelstatus") == "6"
                        and state["globals"]["ginventorycontent"] != inventory_before
                        and state["save_generation"] > generation_before
                        and "genvinstobjekt" in state["objects"]
                    ):
                        break
                else:
                    raise ValueError("clock victory did not display and save its inventory reward")
                inventory_rewarded = state["globals"]["ginventorycontent"]
                checkpoints["reward"] = {
                    "tick": state["tick"],
                    "save_generation_before": generation_before,
                    "save_generation_after": state["save_generation"],
                    "inventory_before": inventory_before,
                    "inventory_after": inventory_rewarded,
                }
                # Let the original reward fader and speech finish before reset.
                for _ in range(120):
                    state = step(60)
                    if (
                        "genvinstobjekt" not in state["objects"]
                        and state["globals"].get("gspelstatus") == "3"
                    ):
                        break
                else:
                    raise ValueError("clock reward did not finish and return to the next puzzle")
                command("reboot")
                state = step(18000)
                state = click(state, 17)
                state = step(2400)
                if (
                    state["movie"] != "HUSET.DXR"
                    or state["globals"].get("gen_minspelare") != first
                    or state["globals"].get("ginventorycontent") != inventory_rewarded
                ):
                    raise ValueError(
                        "clock reward did not survive player reload after native reboot"
                    )
                checkpoints["reward_reload"] = {"tick": state["tick"], "inventory_preserved": True}

        except BaseException as exc:
            failure = str(exc)
            raise
        finally:
            if process.poll() is None:
                try:
                    process.stdin.write("quit-export\n" if failure is None else "quit\n")
                    process.stdin.flush()
                except BrokenPipeError:
                    pass
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            shutdown_failed = failure is None and process.returncode != 0
            if shutdown_failed:
                failure = f"native probe exited with status {process.returncode}"
            if failure is None:
                try:
                    flash = flash_path.read_bytes()
                    if len(flash) != 131072:
                        raise ValueError("native FlashRAM export must contain 131072 bytes")
                    flash_export = {
                        "path": flash_path.name,
                        "sha256": hashlib.sha256(flash).hexdigest(),
                        "bytes": len(flash),
                        "origin": "successful source input journey on host-native runtime",
                    }
                except (OSError, ValueError) as error:
                    failure = str(error)
                    shutdown_failed = True
            evidence = (
                json.dumps(
                    {
                        "commands": commands,
                        "states": states,
                        "checkpoints": checkpoints,
                        "error": failure,
                        "flash_export": flash_export,
                    },
                    indent=2,
                )
                + "\n"
            )
            (output / "journey.json").write_text(evidence)
            if failure is not None:
                # Keep each failed attempt when a later rerun replaces the
                # current report; the state includes its native stack/error.
                stamp = time_ns()
                (output / f"journey-failed-{stamp}.json").write_text(evidence)
                log.flush()
                (output / f"journey-failed-{stamp}.log").write_bytes(
                    (output / "journey.log").read_bytes()
                )
                (output / "validation.json").write_text(
                    json.dumps({"status": "failed", "error": failure}, indent=2) + "\n"
                )
            if shutdown_failed:
                raise ValueError(failure)
    return {
        "status": "passing",
        "platform": "host-native",
        "scenario": "startup-player-create-reload"
        + ("-clock-victory-reward-reload" if activity else ""),
        "commands": len(commands),
        "checkpoints": checkpoints,
        "flash_export": flash_export,
        "journey_sha256": hashlib.sha256((output / "journey.json").read_bytes()).hexdigest(),
        "hardware_qualified": False,
    }

"""Replay the audited activity entrances and formerly failing interactions."""

import hashlib
import json
import os
import subprocess
from pathlib import Path

ROUTES = {
    "bedroom-inventions": ([14, 17], "UFAP.DXR"),
    "bedroom-ts": ([14, 12], "TS.DXR"),
    "train-departure": ([14, 12], "TS.DXR"),
    "invention-release": ([14, 17], "UFAP.DXR"),
    "invention-drag": ([14, 17], "UFAP.DXR"),
    "living-br": ([11, 12], "BR.DXR"),
    "living-pl": ([11, 14, 11], "PL.DXR"),
    "living-fa": ([11, 14, 12], "FA.DXR"),
    "attic-sp": ([12, 11], "SP.DXR"),
    "attic-nb": ([12, 12], "NB.DXR"),
    "attic-vi": ([12, 13], "HUSET.DXR"),
    "kitchen-kk": ([15, 11], "KK.DXR"),
    "kitchen-ss": ([15, 13], "SS.DXR"),
    "kitchen-sk": ([15, 15, 11], "SK.DXR"),
    "kitchen-rs": ([15, 15, 13], "RS.DXR"),
    "world": ([13], "NY.DXR"),
}


class Probe:
    def __init__(self, executable: Path, output: Path, name: str):
        self.output, self.name = output, name
        self.commands, self.states = [], []
        self.log = (output / f"{name}.log").open("w")
        self.process = subprocess.Popen(
            [str(executable), "START", "rpc"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.log,
            text=True,
            env=os.environ
            | {
                "DIRECTOR64_WORK_DIR": str(executable.parent.parent),
                "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=1",
                "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
            },
        )

    def read(self):
        line = self.process.stdout.readline()
        if not line:
            raise ValueError(f"probe exited; inspect {self.name}.log")
        self.state = json.loads(line)
        self.states.append(self.state)
        if self.state["error"] or self.state["quit"]:
            raise ValueError(self.state["error"] or "unexpected source quit")
        return self.state

    def step(self, ticks=6000, x=320, y=240, down=0):
        command = f"step {ticks} {x} {y} {down}"
        self.commands.append(command)
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        return self.read()

    def center(self, sprite):
        item = next(s for s in self.state["sprites"] if s["id"] == sprite)
        left, top, right, bottom = item["bounds"]
        x, y = (left + right) // 2, (top + bottom) // 2
        if not (0 <= x < 640 and 0 <= y < 480):
            raise ValueError(f"source control {sprite} outside stage")
        return x, y

    def click(self, sprite):
        x, y = self.center(sprite)
        self.step(1, x, y, 1)
        return self.step(120, x, y, 0)

    def movie(self, name):
        if self.state["movie"] != name:
            raise ValueError(f"expected {name}, got {self.state['movie']}")

    def finish(self):
        try:
            if self.process.poll() is None:
                self.process.stdin.write("quit\n")
                self.process.stdin.flush()
            self.process.wait(timeout=10)
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.log.close()
            (self.output / f"{self.name}.json").write_text(
                json.dumps(
                    {
                        "commands": self.commands,
                        "states": self.states,
                        "exit_code": self.process.returncode,
                    },
                    indent=2,
                )
                + "\n"
            )


def name_entry(executable: Path, output: Path):
    probe = Probe(executable, output, "name-entry")
    failure = None

    def command(text):
        probe.commands.append(text)
        probe.process.stdin.write(text + "\n")
        probe.process.stdin.flush()
        return probe.read()

    try:
        probe.read()
        probe.step(18000)
        probe.movie("LO.DXR")
        probe.click(17)
        for character, code in (("A", 0), ("B", 11), ("C", 8)):
            command(f"key {code} {ord(character)} 1")
            probe.step(4)
            command(f"key {code} 0 0")
            probe.step(4)
        if probe.state["name_text"] != "ABC":
            raise ValueError("original name handler did not type ABC")
        command("key 51 8 1")
        probe.step(4)
        command("key 51 0 0")
        probe.step(4)
        if probe.state["name_text"] != "AB":
            raise ValueError("original name handler did not delete C")
        probe.click(40)
        probe.step(2400)
        probe.movie("HUSET.DXR")
        if '#namn:"AB"' not in probe.state["globals"].get("gen_minspelare", ""):
            raise ValueError("typed name did not reach the original player record")
        command("reboot")
        probe.step(18000)
        probe.click(17)
        probe.step(2400)
        probe.movie("HUSET.DXR")
        if '#namn:"AB"' not in probe.state["globals"].get("gen_minspelare", ""):
            raise ValueError("typed name did not survive reboot and player reload")
    except (ValueError, RuntimeError, StopIteration, BrokenPipeError) as error:
        failure = str(error) or type(error).__name__
    finally:
        probe.finish()
    if probe.process.returncode and not failure:
        failure = f"probe exit {probe.process.returncode}"
    return {"name": "name-entry", "status": "failed" if failure else "passing", "failure": failure}


def train_departure(probe: Probe):
    passengers = json.loads(
        probe.state["objects"]["gpassageraregenerellobj"]["pantalpassagerarepastationen"]
    )
    number = len(passengers)
    if not 1 <= number <= 30:
        raise ValueError("train has no station question to answer")
    board = next(s for s in probe.state["sprites"] if s["id"] == 14)["bounds"]
    x = board[0] + 13 + (number - 1) % 10 * 37 + 18
    y = board[1] + 15 + (number - 1) // 10 * 39 + 18
    probe.step(1, x, y, 1)
    probe.step(1, x, y, 0)
    moving_samples, poses = 0, set()
    for _ in range(400):
        probe.step(30)
        probe.movie("TS.DXR")
        moving = probe.state["objects"]["gtagetobj"]["ptagetflytta"] == "1"
        rods = [s["bounds"] for s in probe.state["sprites"] if s["id"] in (37, 38)]
        if len(rods) != 2 or any(
            not 0 <= value <= (480 if i % 2 else 640)
            for bounds in rods
            for i, value in enumerate(bounds)
        ):
            raise ValueError("train piston grew outside the stage during departure")
        if moving:
            moving_samples += 1
            poses.add(tuple(rods[0]))
        elif moving_samples >= 4 and len(poses) >= 4:
            return  # Departed, animated and arrived at the next station.
    raise ValueError("train did not animate through departure and the next arrival")


def run(executable: Path, output: Path):
    output.mkdir(parents=True, exist_ok=True)
    scenarios = [name_entry(executable, output)]
    for name, (route, destination) in ROUTES.items():
        probe = Probe(executable, output, name)
        failure = None
        try:
            probe.read()
            probe.step(18000)
            probe.movie("LO.DXR")
            probe.click(17)
            probe.click(40)
            probe.step(2400)
            probe.movie("HUSET.DXR")
            if probe.state["globals"].get("gvindspelaktivt") != "0":
                raise ValueError("missing VI activity was enabled")
            for control in route:
                probe.click(control)
                probe.step()
            probe.movie(destination)
            if name == "invention-release":
                probe.step(1, 400, 430, 1)
                probe.step(120, 400, 430, 0)
            elif name == "invention-drag":
                x, y = probe.center(16)
                probe.step(1, x, y, 1)
                probe.step(60, x + 80, y + 40, 1)
                probe.step(120, x + 80, y + 40, 0)
            # Run beyond each original failing tick, including several cycles
            # of NB/FA animation and BR's course drawing; this is entrance coverage,
            # not a claim that all activities have been completed.
            probe.step(6000)
            probe.movie(destination)
            if name == "train-departure":
                train_departure(probe)
            state = probe.state
            if name == "living-br":
                course = state["objects"]["bana"]
                if course["introstate"] != "vantapaklick" or course["ritkant"] != "527":
                    raise ValueError("BR did not draw the first course with its keyed spacing")
            elif name == "attic-nb":
                if state["globals"].get("gfas") != "spel" or "gflugan" not in state["objects"]:
                    raise ValueError("NB did not create its constrained fly and start play")
            elif name == "living-fa" and state["globals"].get("gfas") != "fanga":
                raise ValueError("FA did not remain in the active catching phase")
            elif name.startswith("invention-") and state["globals"].get("guppspelstatus") != "3":
                raise ValueError("invention input did not return to its active phase")
        except (ValueError, RuntimeError, StopIteration, BrokenPipeError) as error:
            failure = str(error) or type(error).__name__
        finally:
            probe.finish()
        if probe.process.returncode and not failure:
            failure = f"probe exit {probe.process.returncode}; inspect {name}.log"
        state = probe.states[-1] if probe.states else {}
        scenarios.append(
            {
                "name": name,
                "status": "failed" if failure else "passing",
                "failure": failure,
                "movie": state.get("movie"),
                "frame": state.get("frame"),
                "tick": state.get("tick"),
                "commands": len(probe.commands),
            }
        )
    return {
        "status": "passing" if all(s["status"] == "passing" for s in scenarios) else "failed",
        "platform": "host-native",
        "hardware_qualified": False,
        "original_projector_verified": False,
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "scenarios": scenarios,
    }

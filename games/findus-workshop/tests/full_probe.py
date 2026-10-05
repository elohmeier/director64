#!/usr/bin/env python3
"""Input-only integration checks against locally compiled native movie handlers.

The debug protocol exposes state for assertions; it has no handler invocation,
frame jump, global assignment, or reward-injection command.
"""

from __future__ import annotations

import argparse
import ast
import json
import random
import subprocess
from contextlib import suppress
from pathlib import Path

from director64.project import work_dir


class Probe:
    def __init__(self, movie="START", executable=None):
        executable = executable or f"{work_dir()}/native/director-probe"
        self.process = subprocess.Popen(
            [executable, movie, "rpc"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True
        )
        self.commands = []
        self.visited = []
        self.pointer = (320, 240)
        self.state = self.receive()

    def receive(self):
        while line := self.process.stdout.readline():
            if line.startswith("ENTER "):
                self.visited.append(line.strip())
            if line.startswith("{"):
                state = json.loads(line)
                if state["error"]:
                    self.state = state
                    raise RuntimeError(state["error"])
                return state
        raise RuntimeError(f"native probe stopped: {self.process.poll()}")

    def step(self, ticks=60, x=None, y=None, down=False):
        x = self.pointer[0] if x is None else x
        y = self.pointer[1] if y is None else y
        self.pointer = (x, y)
        command = f"step {ticks} {int(x)} {int(y)} {int(down)}"
        self.commands.append(command)
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        self.state = self.receive()
        return self.state

    def value(self, name, default=0):
        value = self.state["globals"].get(name.lower())
        if value is None:
            return default
        if value in ("#true", "true"):
            return True
        if value in ("#false", "false"):
            return False
        try:
            return ast.literal_eval(value)
        except ValueError, SyntaxError:
            return value

    def click(self, x, y, after=80):
        self.step(20, x, y, True)
        return self.step(after, x, y, False)

    def until(self, predicate, limit=12000, **input_):
        end = self.state["tick"] + limit
        while not predicate(self.state):
            if self.state["tick"] >= end:
                raise TimeoutError(
                    f"{self.state['movie']}:{self.state['frame']} "
                    f"{self.state['handler']}:{self.state['line']}"
                )
            self.step(20, **input_)

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=10)

    def sprite(self, index):
        return next(s for s in self.state["sprites"] if s["id"] == index)

    def sprite_point(self, index):
        self.process.stdin.write(f"point {index}\n")
        self.process.stdin.flush()
        point = self.receive()["point"]
        if point is None:
            raise LookupError(f"sprite {index} has no hittable point")
        return point

    def member_point(self, member, *, behavior=False):
        key = "script" if behavior else "member"
        for sprite in self.state["sprites"]:
            if sprite[key] & 65535 == member:
                try:
                    return self.sprite_point(sprite["id"])
                except LookupError:
                    pass
        raise LookupError(f"no hittable {'behavior' if behavior else 'member'} {member}")

    def drag(self, index, x, y):
        point = self.sprite_point(index)
        self.step(50, *point, True)
        self.step(30, x, y, True)
        self.step(80, x, y, False)

    def leave(self, x, y, target):
        source = self.state["movie"]
        for _ in range(3000):
            self.step(1, x, y, True)
            if self.state["movie"] != source:
                break
        self.step(1, x, y, False)
        assert self.state["movie"] == target + ".DXR", "return destination"

    def reboot(self):
        self.commands.append("reboot")
        self.process.stdin.write("reboot\n")
        self.process.stdin.flush()
        self.pointer = (320, 240)
        self.state = self.receive()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


ROUTES = {
    "GARDEN": (40, 35),
    "SNIKBOD": (105, 35),
    "SAGOR": (220, 35),
    "KISTA": (270, 40),
    "BYRAN": (320, 35),
    "MVEGGEN": (370, 35),
    "BREDHOGN": (420, 35),
    "BRODER": (480, 35),
    "VEMORY": (530, 35),
}


def workshop(probe, profile=1):
    for _ in range(100):
        scene = probe.state["movie"]
        if scene == "PINTRO.DXR" and probe.state["frame"] == 33:
            return
        point = {"GINTRO.DXR": (50, 100), "GARDEN.DXR": (195, 300)}.get(scene)
        if scene == "VAKT.DXR":
            with suppress(LookupError):
                point = probe.member_point(15 + profile, behavior=True)
        if point:
            probe.click(*point)
        else:
            probe.step(100)
    raise TimeoutError("boot/profile/workshop route did not settle")


def construction(p):
    p.until(lambda s: s["frame"] == 32)
    p.step(30)
    # Each construction's original object profile supplies both the removable
    # toolbox pieces and their snap coordinates. Repeated passes allow source
    # dependencies (e.g. a support must be fitted before its load).
    for _ in range(6):
        for i, (kind, profile) in enumerate(
            zip(p.value("verktygsobjekt"), p.value("objektaktivprofil"), strict=True)
        ):
            if kind != 1 or not profile:
                continue
            index = p.value("spritenum")[i]
            if p.value("objektaktiv")[i] == profile:
                continue
            try:
                p.drag(index, p.value("startxpos")[i], p.value("startypos")[i])
            except LookupError:
                continue
    switch = 9 if p.state["movie"] == "KONSTR01.DXR" else 27
    left, top, right, bottom = p.sprite(switch)["bounds"]
    p.click((left + right) // 2, (top + bottom) // 2, after=3000)
    if not p.value("konstruktionfardig"):
        raise AssertionError(f"construction did not succeed: {p.value('objektaktiv')}")
    # Stop the running mechanism; the original script then presents the reward.
    p.click(320, 450, after=100)
    p.until(lambda s: p.value("gAntalGuldFeather") > 0, limit=6000)
    p.click(40, 35)
    p.until(lambda s: s["movie"] == "PINTRO.DXR", limit=6000)


def vemory(p, mode):
    before = p.value("gAntalGuldFeather")
    p.until(lambda s: s["frame"] == 19)
    p.click(*p.sprite_point({1: 42, 2: 44, 3: 43}[mode]))
    p.until(
        lambda s: (
            p.value("gmode") == mode and len(p.value("lslumppos", [])) == 30 and s["frame"] == 3
        )
    )

    def point(index):
        boxes = list(
            zip(
                p.value("clminx"),
                p.value("clminy"),
                p.value("clmaxx"),
                p.value("clmaxy"),
                strict=True,
            )
        )
        left, top, right, bottom = boxes[index]
        board = p.value("lslumppos")
        for y in range(top + 1, bottom):
            for x in range(left + 1, right):
                if (
                    next(
                        i
                        for i, (left_, top_, right_, bottom_) in enumerate(boxes)
                        if board[i] and left_ <= x <= right_ and top_ <= y <= bottom_
                    )
                    == index
                ):
                    return x, y
        raise AssertionError(f"no accessible VEMORY position {index}")

    board = p.value("lslumppos")
    positive = [i for i, n in enumerate(board) if n > 0]
    # A wrong pair must not award a card or remove either object.
    for i in positive[:2]:
        p.click(*point(i), after=100)
    assert p.value("antalhittadeverktyg") == 0
    for found in range(10):
        board = p.value("lslumppos")
        first = next(i for i, n in enumerate(board) if n > 0)
        n = board[first]
        wanted = (
            n if mode == 1 else p.value(f"clmode{mode}obj2")[p.value(f"clmode{mode}obj1").index(n)]
        )
        second = board.index(-wanted)
        p.click(*point(first), after=100)
        p.click(*point(second), after=1800 if found == 0 else 100)
        p.until(lambda s, n=found: p.value("antalhittadeverktyg") == n + 1)
    p.until(lambda s: p.value("gAntalGuldFeather") == before + 1)
    p.step(3000)
    p.click(40, 35)
    p.until(lambda s: s["movie"] == "PINTRO.DXR" and s["frame"] == 33)


def finndunk(p):
    p.until(lambda s: s["frame"] == 3)
    p.step(100)

    def choose(value, count):
        x = p.value("clburkxpos")[value - 1] - p.value("clwidth")[value - 1] // 2
        y = p.value("clburkypos")[value - 1] - p.value("clheight")[value - 1] // 2
        for _ in range(count):
            p.click(x, y, after=120)
        assert p.value("gvald") == value and p.value("gantalburkar") == count, "can selection"

    choose(1, 1)
    p.click(*p.sprite_point(6), after=100)
    p.until(lambda s: p.value("bilfardklar"), limit=15000)
    assert not p.value("foregaenderattsvarat") and not p.value("gAntalGuldFeather")
    p.step(1000)
    p.click(*p.sprite_point(46), after=200)
    for _ in range(7):
        p.until(lambda s: not p.value("bilfardklar") and not p.value("gantalburkar"))
        p.step(300)
        total = p.value("gsumma")
        previous = p.value("gantalposter")
        factor = next(n for n in range(1, 11) if total % n == 0 and total // n <= 10)
        choose(factor, total // factor)
        p.click(*p.sprite_point(6), after=100)
        p.until(lambda s, n=previous: p.value("gantalposter") == n + 1, limit=20000)
        p.step(1800)
        if p.value("gAntalGuldFeather"):
            break
        # The retry animal bounces; release promptly while still inside it.
        point = p.sprite_point(46)
        p.step(1, *point, True)
        p.step(200, *point, False)
    assert p.value("gAntalGuldFeather") == 1
    p.step(1800)
    p.click(30, 20)
    p.until(lambda s: s["movie"] == "BYRAN.DXR")


def foting(p):
    p.until(lambda s: s["frame"] == 2)
    p.step(100)
    p.click(*p.sprite_point(23), after=120)
    p.click(*p.sprite_point(39), after=100)
    p.until(lambda s: p.value("spelstatus") == 3, limit=16000)
    p.step(1200)
    p.click(*p.sprite_point(p.value("igendjurspritenr")), after=200)
    for _ in range(5):
        p.click(*p.sprite_point(27), after=120)
    p.click(*p.sprite_point(39), after=100)
    p.until(lambda s: p.value("spelstatus") == 2, limit=16000)
    p.step(1200)
    p.click(*p.sprite_point(48), after=500)
    assert p.value("bmode") == 2
    a, b = p.value("lrandom1"), p.value("lrandom2")
    selection = 2 if a[0] * a[1] > b[0] * b[1] else 1
    p.click(*p.sprite_point(30 + selection), after=100)
    p.click(*p.sprite_point(39), after=100)
    p.until(lambda s: p.value("spelstatus") == 2, limit=16000)
    p.step(1200)
    p.click(40, 35)
    p.until(lambda s: s["movie"] == "BYRAN.DXR")


def broder(p):
    p.until(lambda s: p.value("mouseupset"))
    p.click(630, 470, after=120)
    p.until(lambda s: p.value("antalforsok") == 1)
    assert not p.value("antalbroderfunna") and not p.value("gAntalGuldFeather")
    for brother in range(1, 5):
        p.until(lambda s: p.value("mouseupset"))
        p.step(1200)
        position = p.value("lposlista")[brother - 1] - 1
        rectangles = list(
            zip(
                *(p.value(k) for k in ("clminxpos", "clminypos", "clmaxxpos", "clmaxypos")),
                strict=True,
            )
        )
        left, t, r, b = rectangles[position]
        criterion = p.value("lslumplista")[brother - 1]
        criteria = p.value("clkriterium")
        point = next(
            (x, y)
            for y in range(max(67, t + 1), b, 4)
            for x in range(left + 1, r, 4)
            if not any(
                j != position and criterion in criteria[j] and a <= x <= c and d <= y <= e
                for j, (a, d, c, e) in enumerate(rectangles)
            )
        )
        p.click(*point, after=100)
        p.until(lambda s, n=brother: p.value("antalbroderfunna") == n)
    p.until(lambda s: p.value("gAntalGuldFeather") == 1)
    p.step(3000)
    p.click(*p.member_point(25, behavior=True))
    p.until(lambda s: s["movie"] == "PINTRO.DXR")
    # Check a settled hub, not the one-frame transition boundary: native audio
    # completion can differ slightly from the host's sample-duration clock.
    p.step(1800)
    assert p.state["frame"] == 33


def hyvel(p):
    p.step(2400)
    p.click(320, 240, after=1800)
    p.click(*p.member_point(17, behavior=True), after=2500)
    assert not p.value("gAntalGuldFeather")
    target = p.value("ltjuv")
    for row, name in enumerate(("la", "lb", "lc")):
        for _ in range(15):
            stop = p.value("lcurrentstop")[row]
            if p.value(name)[stop - 1] == target[row]:
                break
            p.click(*p.member_point((23, 27, 31)[row]), after=100)
            p.until(lambda s, row=row, stop=stop: p.value("lcurrentstop")[row] != stop)
        assert p.value(name)[p.value("lcurrentstop")[row] - 1] == target[row]
    p.click(*p.member_point(17, behavior=True), after=100)
    p.until(lambda s: p.value("ltjuv") != target, limit=20000)
    p.step(2000)
    p.click(*p.member_point(36, behavior=True), after=100)
    p.until(lambda s: s["movie"] == "BYRAN.DXR")


def verkork(p):
    p.step(5000)
    for i in range(3):
        p.step(50, 110, 345, True)
        p.step(40, p.value("clhotspotx")[i], p.value("clhotspoty")[i], True)
        p.step(100, p.value("clhotspotx")[i], p.value("clhotspoty")[i], False)
    instruments, notes = p.value("linstrument"), p.value("lnot")
    assert instruments[:3] == [1, 1, 1] and notes[:3] == [1, 2, 3], "music placement"
    p.click(484, 42, after=500)
    assert p.value("antalposterifil") == 1 and p.state["save_lengths"][8] == 161
    p.click(150, 50, after=500)
    assert not any(p.value("linstrument")), "music clear"
    p.click(536, 45, after=500)
    # The original initializer/clear handlers have differently padded lists;
    # the file format stores exactly forty musical positions.
    assert p.value("linstrument")[:40] == instruments[:40], "music instruments reload"
    assert p.value("lnot")[:40] == notes[:40], "music notes reload"
    p.click(580, 390, after=3000)
    p.leave(40, 35, "MVEGGEN")


def malar(p):
    p.step(5000)
    for member in range(5, 25):
        p.click(*p.member_point(member, behavior=True), after=100)
    assert any(any(row) for row in p.value("lfigur")), "figure combinations"
    p.click(30, 35, after=100)
    p.until(lambda s: s["movie"] == "MVEGGEN.DXR")
    p.step(600)


def sagor(p):
    p.step(1200)
    menu = p.state["frame"]
    for point in ((200, 240), (480, 220)):
        p.click(*point, after=100)
        p.until(lambda s: s["frame"] != menu)
        p.until(lambda s: s["frame"] == menu, limit=60000)
    # This return is polled by the score handler, not a sprite mouse handler.
    p.click(25, 25, after=100)
    p.until(lambda s: s["movie"] == "PINTRO.DXR")


def bredhogn(p):
    p.step(3000)
    for i in range(3):
        p.step(50, 580, 200, True)
        p.step(40, 230 + i * 80, 260, True)
        p.step(100, 230 + i * 80, 260, False)
    assert p.value("gantalobj") == 3, "construction editor placement"
    old = p.value("lrot")[:3]
    p.click(*p.member_point(27, behavior=True), after=120)
    assert p.value("lrot")[:3] != old, "construction editor rotation"
    objects, rotations = p.value("lobjekt")[:3], p.value("lrot")[:3]
    p.click(315, 455, after=500)
    assert p.value("antalposterifil") == 1 and p.state["save_lengths"][7] > 30
    p.click(612, 440, after=500)
    assert not p.value("gantalobj"), "construction editor clear"
    p.click(360, 455, after=500)
    assert p.value("gantalobj") == 3 and p.value("lobjekt")[:3] == objects, "editor reload"
    assert p.value("lrot")[:3] == rotations, "editor rotation reload"
    # Make a new edit before leaving, exercising the explicit save prompt.
    p.drag(4, 400, 300)
    p.click(30, 30, after=100)
    p.until(lambda s: s["handler"] == "asksavequestion")
    left, t, r, b = p.sprite(46)["bounds"]
    p.click((left + r) // 2, (t + b) // 2, after=100)
    p.until(lambda s: s["movie"] == "PINTRO.DXR")
    assert p.state["save_lengths"][7] > 60, "save-on-return"


def mossen(p):
    p.until(lambda s: p.value("timecounter") > 0)
    p.step(8000, 620, 20)
    p.until(lambda s: p.value("speletslut") == 1)
    assert not p.value("gAntalGuldFeather"), "missed mice must not earn a reward"
    p.click(610, 430, after=100)
    p.until(lambda s: p.value("timecounter") < 10 and not p.value("speletslut"))
    end = p.state["tick"] + 14000
    while not p.value("gAntalGuldFeather") and p.state["tick"] < end:
        active = p.value("laktivobj")
        target = next(
            (i for i, v in enumerate(active) if v in (2, 3) and i + 1 != p.value("ghoppsprite")),
            None,
        )
        if target is None:
            p.step(4, 620, 20)
        else:
            p.step(4, p.value("clhotspotx")[target], p.value("clhotspoty")[target])
    assert p.value("gAntalGuldFeather") == 1, "mouse-catching reward"
    p.step(2000)
    p.click(30, 30, after=100)
    p.until(lambda s: s["movie"] == "BYRAN.DXR")


def plockspl_timing(p):
    """Assert recovered movement/timer relationships, not projector wall speed."""
    runs = []
    first = 20 if p.sprite(19)["loc"][0] > 320 else 620
    for target in (first, 640 - first, first):
        started = p.state["tick"]
        old_x = p.sprite(19)["loc"][0]
        moves = []
        for _ in range(150):
            p.step(1, target, 380)
            x = p.sprite(19)["loc"][0]
            if x != old_x:
                assert abs(x - old_x) == 10, "source Findus step"
                moves.append((p.state["tick"], x))
                old_x = x
            if abs(x - target) <= 40:
                break
        else:
            raise AssertionError("Findus traverse stalled behind stage updates")
        assert len(moves) >= 20, "measure a full running interval"
        interval = p.value("updatetime")[6] + 1
        gaps = [b[0] - a[0] for a, b in zip(moves, moves[1:], strict=False)]
        assert all(gap == interval for gap in gaps), ("Findus timer interval", gaps)
        turn_ticks = moves[0][0] - started
        assert turn_ticks <= 14, ("Findus turn stalled behind stage updates", turn_ticks)
        runs.append(
            {
                "target": target,
                "pixels_per_second": 600 / interval,
                "first_move_ticks": turn_ticks,
                "moves": moves,
            }
        )
    return {"findus_runs": runs}


def plockspl(p, mode):
    p.step(1200)
    for _ in range(mode - 1):
        left, t, r, b = p.sprite(6)["bounds"]
        p.click((left + r) // 2, (t + b) // 2, after=300)
    assert p.value("difficulty") == mode
    timing = plockspl_timing(p)
    end = p.state["tick"] + 120000
    while not p.value("gAntalGuldFeather") and p.state["tick"] < end:
        active, ys, xs = p.value("objaktiv"), p.value("ypos"), p.value("xpos")
        candidates = [i for i in range(6) if active[i] == 2 and ys[i] < 405]
        target = max(candidates, key=lambda i: ys[i]) if candidates else None
        p.step(9, xs[target] if target is not None else 320, 380)
    assert p.value("gAntalGuldFeather") == 1, "twenty caught feathers reward"
    p.step(2500)
    p.click(*p.member_point(147), after=100)
    p.until(lambda s: s["movie"] == "MVEGGEN.DXR")
    p.step(1800)
    assert p.state["frame"] == 2
    return timing


def luffspel(p, mode):
    p.step(2500)
    left, t, r, b = p.sprite(6 + mode)["bounds"]
    p.click((left + r) // 2, (t + b) // 2, after=2500)
    assert p.value("pmode") == mode
    p.click(*p.member_point(88, behavior=True), after=2500)
    end = p.state["tick"] + 30000
    moves = {1: 0, 2: 0}
    while not p.value("gamewon") and p.state["tick"] < end:
        player = p.value("player")
        if mode == 1 and player != 1:
            p.step(100)
            continue
        player = player or 1
        pieces = p.value(f"sprfig{player}")
        old = p.value(f"oldxfig{player}")
        index = next(i for i in reversed(range(10)) if old[i] == 0)
        if mode == 2:
            gx, gy = (4 + moves[1], 6) if player == 1 else (3 + 2 * moves[2], 8)
        else:
            # Deliberately do not block the computer: exercise its winning path.
            board = p.value("spelplan")
            gx, gy = next(
                (x, y) for y in (9, 7, 5, 3) for x in range(4, 9, 2) if board[y - 1][x - 1] == "0"
            )
        x = round(
            p.value("gridx") + (gx - 1) * p.value("spacex") + 6 - (gy - 1) * p.value("spacex2")
        )
        y = p.value("gridy") + (gy - 1) * p.value("spacey") - 10
        point = p.sprite_point(pieces[index])
        sprite = p.sprite(pieces[index])
        px, py = x + point[0] - sprite["loc"][0], y + point[1] - sprite["loc"][1]
        p.step(20, *point, True)
        p.step(30, px, py, True)
        p.step(500, px, py, False)
        assert p.value(f"oldxfig{player}")[index] == gx, "board piece placement"
        moves[player] += 1
    assert p.value("gamewon"), "board game completion"
    p.step(1800)
    p.click(*p.member_point(32, behavior=True), after=100)
    p.until(lambda s: s["movie"] == "BYRAN.DXR")


def profiles(p):
    for profile in range(1, 8):
        if profile > 1:
            p.reboot()
        workshop(p, profile)
        assert p.value("gvilkenvakt") == profile
        p.click(530, 35)
        p.until(lambda s: s["movie"] == "VEMORY.DXR")
        vemory(p, 1)
        saved = [int(s.split("\r")[0]) for s in p.state["save_contents"][:7]]
        assert saved == [1] * profile + [0] * (7 - profile), "profile isolation"
    p.reboot()
    workshop(p, 3)
    p.click(270, 40)
    p.until(lambda s: s["movie"] == "KISTA.DXR")
    p.step(1800)
    assert p.value("gAntalGuldFeather") == 1 and p.value("gvilkenvakt") == 3


def treasure(p):
    workshop(p)
    for _ in range(10):
        p.click(530, 35)
        p.until(lambda s: s["movie"] == "VEMORY.DXR")
        vemory(p, 1)
    p.click(270, 40)
    p.until(lambda s: s["movie"] == "KISTA.DXR")
    p.step(3000)
    assert p.value("gAntalGuldFeather") == 10, "currency loaded from selected profile"
    left, top, right, bottom = p.sprite(44)["bounds"]
    p.drag(2, (left + right) // 2, (top + bottom) // 2)
    p.until(lambda s: p.value("gantalguldpengar") == 1)
    assert p.value("gAntalGuldFeather") == 0
    p.step(500)
    p.drag(12, (left + right) // 2, (top + bottom) // 2)
    p.until(lambda s: p.value("gAntalGuldFeather") == 10)
    assert p.value("gantalguldpengar") == 0
    p.click(40, 80)
    p.until(lambda s: s["movie"] == "PINTRO.DXR")
    p.step(1500)
    p.click(105, 35)
    p.until(lambda s: s["movie"] == "SNIKBOD.DXR")
    collect_map(p)


def collect_map(p, magic_delay=0, magic_animation=False):
    p.step(1800)
    for i in range(9):
        for _ in range(100):
            try:
                point = p.sprite_point(31 + i)
                break
            except LookupError:
                p.step(60)
        else:
            raise AssertionError(f"map fragment {i + 1} inaccessible")
        # A timed fly interlude can own the score while pieces remain visible.
        # Retry real clicks as the controller journey does; visibility alone
        # does not mean the source's mouseUp handler will accept this click.
        for _ in range(8):
            p.click(*point, after=700)
            if p.value("lskattkartamatris")[i] > 0:
                break
        else:
            raise AssertionError(f"map fragment {i + 1} did not accept input")
        p.step(600)
    # A map-fragment speech or the fly interlude can still own the event loop.
    # Wait/try the real hotspot again; do not invoke its reward handler directly.
    if magic_animation:
        # These ordinary environmental hotspots choose random animations.
        # They provide another input-only RNG history when idle waits alone do
        # not vary the subsequent special treasure.
        p.until(lambda s: s["frame"] == 5)
        p.click(178, 318, after=1200)
        p.until(lambda s: s["frame"] == 5)
        p.click(215, 380, after=1800)
    if magic_delay:
        p.step(magic_delay)
    for _ in range(15):
        p.step(120, 448, 326, True)
        p.step(1200, 448, 326, False)
        if any(n in (34, 35) for n in p.value("lskattmatris")):
            break
    else:
        raise AssertionError("complete map did not reveal special treasure")
    p.step(1800)
    p.leave(40, 35, "PINTRO")
    p.step(1800)
    assert p.state["movie"] == "PINTRO.DXR" and p.state["frame"] == 33


HUB_ROUTES = {
    "HYVEL": ("BYRAN", (250, 140)),
    "MOSSEN": ("BYRAN", (250, 200)),
    "FINNDUNK": ("BYRAN", (230, 300)),
    "FOTING": ("BYRAN", (430, 320)),
    "LUFFSPEL": ("BYRAN", (450, 160)),
    "VERKORK": ("MVEGGEN", (40, 280)),
    "PLOCKIN": ("MVEGGEN", (40, 340)),
    "MALAR": ("MVEGGEN", (40, 400)),
}


def hubs(p, target):
    workshop(p)
    if target.startswith("KONSTR"):
        level = int(target[-2:])
        p.click(590, 35, after=300)
        p.until(lambda s: p.value("antaljanejdjurggr", -1) >= 0)
        for selected in range(2, level + 1):
            index = p.value("nejdjurspritenr")
            left, top, right, bottom = p.sprite(index)["bounds"]
            p.click((left + right) // 2, (top + bottom) // 2, after=100)
            p.until(lambda s, n=selected: p.value("aktritning") == n)
            p.step(100)
        index = p.value("jadjurspritenr")
        left, top, right, bottom = p.sprite(index)["bounds"]
        p.click((left + right) // 2, (top + bottom) // 2, after=100)
        p.until(lambda s: s["movie"] == target + ".DXR")
        p.until(lambda s: s["frame"] == 32)
        p.step(30)
        assert p.value("aktritning") == level
    else:
        hub, point = HUB_ROUTES[target]
        p.click(*ROUTES[hub])
        p.until(lambda s: s["movie"] == hub + ".DXR")
        p.step(1200)
        p.click(*point, after=100)
        p.until(lambda s: s["movie"] == target + ".DXR")
        if target == "PLOCKIN":
            p.until(lambda s: s["movie"] == "PLOCKSPL.DXR", limit=18000)
        p.step(600)


def exit_game(p):
    workshop(p)
    p.click(40, 35)
    p.until(lambda s: s["movie"] == "GARDEN.DXR")
    p.step(1200)
    for confirm in (False, True):
        p.click(*p.member_point(88, behavior=True), after=200)
        p.until(lambda s: s["handler"] == "askquitquestion")
        p.step(200)
        left, top, right, bottom = p.sprite(45 if confirm else 46)["bounds"]
        p.click((left + right) // 2, (top + bottom) // 2, after=400)
        if not confirm:
            assert p.state["movie"] == "GARDEN.DXR" and not p.state["quit"]
    p.until(lambda s: s["movie"] == "HALLTYST.DXR", limit=18000)
    p.until(lambda s: s["quit"], limit=24000)


ACTIVITIES = {
    "FINNDUNK": finndunk,
    "FOTING": foting,
    "BRODER": broder,
    "HYVEL": hyvel,
    "VERKORK": verkork,
    "MALAR": malar,
    "SAGOR": sagor,
    "BREDHOGN": bredhogn,
    "MOSSEN": mossen,
}


def garden(p):
    if p.state["movie"] == "START.DXR":
        workshop(p)
        p.click(40, 35)
    p.until(lambda s: s["movie"] == "GARDEN.DXR" and s["frame"] == 5)
    activations = []
    # Consecutive and mixed sequences in one visit: returning to frame 5 must
    # not hide channels whose first score record appears later in the movie.
    for x, y in [(460, 272)] * 3 + [(491, 296)] * 2 + [(500, 230)] * 2 + [(460, 272)]:
        p.step(30)
        start = p.state["tick"]
        for _ in range(30):
            p.step(1, x, y, True)
            if p.state["frame"] != 5:
                break
        assert p.state["frame"] != 5, "farmyard activation did not start"
        p.step(1, x, y)
        first = p.state["frame"]
        images = set()
        for _ in range(3000):
            for sprite in p.state["sprites"]:
                if sprite["id"] > 35 and sprite["blend"]:
                    left, top, right, bottom = sprite["bounds"]
                    if (
                        right > left
                        and bottom > top
                        and right > 0
                        and bottom > 0
                        and left < 640
                        and top < 480
                    ):
                        images.add((sprite["id"], sprite["member"], left, top, right, bottom))
            if p.state["frame"] == 5:
                break
            p.step(1)
        assert p.state["frame"] == 5, "farmyard animation did not return"
        assert len(images) >= 2, f"farmyard sequence {first} played without visible animation"
        activations.append(
            {"start_tick": start, "first_frame": first, "visible_states": len(images)}
        )
    p.step(60)
    return {"activations": activations}


def credits(p):
    """Reach the recovered logo through normal score playback, including flag-only deltas."""
    p.until(lambda s: s["frame"] >= 222, limit=4000)
    checkpoints = []
    for frame in range(222, 227):
        p.until(lambda s, f=frame: s["frame"] >= f, limit=2000)
        assert p.state["frame"] == frame, "credits checkpoint skipped"
        logo = p.sprite(8)
        assert logo["member"] & 65535 == 75
        assert logo["blend"] == 100, "disabled blending hid the credits logo"
        assert logo["bounds"] == [207, 194, 433, 230]
        checkpoints.append({"frame": frame, "tick": p.state["tick"], "logo": logo})
    return {"logo_checkpoints": checkpoints}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "mode",
        choices=[
            "snapshot",
            "routes",
            "fuzz",
            "construction",
            "activities",
            "profiles",
            "treasure",
            "hubs",
            "exit",
            "map",
            "garden",
            "credits",
        ],
    )
    parser.add_argument("movie", nargs="?", default="START")
    parser.add_argument("--executable", default=f"{work_dir()}/native/director-probe")
    parser.add_argument(
        "--output", type=Path, default=Path(f"{work_dir()}/director/input-probe.json")
    )
    parser.add_argument("--magic-delay", type=int, default=0)
    parser.add_argument("--magic-animation", action="store_true")
    args = parser.parse_args()
    reports = []
    names = (
        ROUTES
        if args.mode == "routes"
        else [f"KONSTR{i:02}" for i in range(1, 13)]
        if args.mode == "construction"
        else [args.movie]
    )
    if args.mode == "activities":
        names = (
            [args.movie]
            if args.movie != "START"
            else [
                "VEMORY:1",
                "VEMORY:2",
                "VEMORY:3",
                *ACTIVITIES,
                "PLOCKSPL:1",
                "PLOCKSPL:2",
                "PLOCKSPL:3",
                "LUFFSPEL:1",
                "LUFFSPEL:2",
            ]
        )
    if args.mode == "hubs":
        names = [*HUB_ROUTES, *(f"KONSTR{i:02}" for i in range(1, 13))]
    for name in names:
        with Probe(
            "START" if args.mode in {"routes", "hubs"} else name.split(":")[0], args.executable
        ) as p:
            try:
                measurements = {}
                if args.mode == "routes":
                    workshop(p)
                    p.click(*ROUTES[name])
                    p.until(lambda s, target=name: s["movie"] == target + ".DXR")
                    p.step(2400)
                elif args.mode == "snapshot":
                    p.step(3600)
                elif args.mode == "construction":
                    construction(p)
                elif args.mode == "profiles":
                    profiles(p)
                elif args.mode == "treasure":
                    treasure(p)
                elif args.mode == "hubs":
                    hubs(p, name)
                elif args.mode == "exit":
                    exit_game(p)
                elif args.mode == "map":
                    collect_map(p, args.magic_delay, args.magic_animation)
                elif args.mode == "garden":
                    measurements = garden(p)
                elif args.mode == "credits":
                    measurements = credits(p)
                elif args.mode == "activities":
                    if name.startswith("VEMORY"):
                        vemory(p, int(name[-1]))
                    elif name.startswith("PLOCKSPL"):
                        measurements = plockspl(p, int(name[-1]))
                    elif name.startswith("LUFFSPEL"):
                        luffspel(p, int(name[-1]))
                    else:
                        ACTIVITIES[name](p)
                else:
                    rng = random.Random(42)
                    for _ in range(500):
                        p.click(rng.randrange(8, 630), rng.randrange(8, 470), after=60)
                report = {
                    "target": name,
                    "passed": True,
                    "visited": p.visited,
                    "commands": p.commands,
                    "state": p.state,
                }
                if measurements:
                    report["measurements"] = measurements
            except (RuntimeError, TimeoutError, AssertionError, LookupError) as error:
                report = {
                    "target": name,
                    "passed": False,
                    "error": str(error),
                    "visited": p.visited,
                    "commands": p.commands,
                    "state": p.state,
                }
            reports.append(report)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(reports, indent=2) + "\n")
            print(f"{name}: {'PASS' if report['passed'] else report['error']}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(reports, indent=2) + "\n")
    if not all(r["passed"] for r in reports):
        raise SystemExit(1)


if __name__ == "__main__":
    main()

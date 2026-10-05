"""Synthetic contracts for the randomized input fuzzer; no recovered media."""

from __future__ import annotations

import json
import os
import stat
from pathlib import Path

import pytest

from director64.fuzz import (
    PROBE_FEATURES,
    Outcome,
    Probe,
    Runner,
    classify_exit,
    episode_commands,
)

FAKE_PROBE = r'''#!/usr/bin/env python3
"""Protocol double for director-probe: mode is selected by the movie name."""
import json
import sys

mode = sys.argv[1]
received = 0
soft_errors = 0
last_soft = ""
text_pending = False


def state(error=""):
    sprite = {"id": 1, "bounds": [100, 100, 200, 200], "member": 1, "script": 0}
    if mode == "TEXTSTRICT":
        sprite["editable"] = True
    print(
        json.dumps(
            {
                "movie": mode,
                "frame": 1,
                "tick": received,
                "depth": 0,
                "quit": False,
                "text_pending": text_pending,
                "error": error,
                "script_errors": soft_errors,
                "last_script_error": last_soft,
                "sprites": [sprite],
                "pointer": {"player": 1, "x": 320, "y": 240},
            }
        ),
        flush=True,
    )


state()
for line in sys.stdin:
    received += 1
    parts = line.split()
    if parts == ["reboot"]:
        text_pending = False
        state()
        continue
    if parts and parts[0] == "text":
        # Mimic dg_edit_text: only an editable sprite, the platform keyboard's
        # characters, at most 20 of them, and never while an entry drains.
        entered = line.split(None, 2)[2].rstrip("\n") if len(parts) >= 3 else ""
        ok = (
            mode == "TEXTSTRICT"
            and not text_pending
            and len(parts) >= 3
            and parts[1] == "1"
            and 1 <= len(entered) <= 20
            and all(c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_." for c in entered)
        )
        if not ok:
            sys.exit(2)
        text_pending = True
        state()
        continue
    text_pending = False
    if mode == "STRICT":
        # Mimic the strict workshop probe: malformed input kills the process.
        ok = len(parts) == 5 and parts[0] in {"step", "pad"}
        if ok:
            ticks, a, b, c = (int(p) for p in parts[1:])
            ok = 0 <= ticks <= 100000
            if parts[0] == "pad":
                ok = ok and -128 <= a <= 127 and -128 <= b <= 127 and 0 <= c < 128
        if not ok:
            sys.exit(2)
    if mode == "FAILSOON" and received >= 3:
        state("synthetic lingo failure in handler N")
        sys.exit(1)
    if mode == "TRIGGER" and "666" in parts:
        state("synthetic trigger failure")
        sys.exit(1)
    if mode == "SOFT" and received == 3:
        soft_errors += 1
        last_soft = "synthetic recovered alert in handler 7"
    state()
'''


@pytest.fixture
def fake_probe(tmp_path):
    path = tmp_path / "fake-probe"
    path.write_text(FAKE_PROBE)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


def make_runner(fake_probe, tmp_path, movies, minimize=False, features=None):
    output = tmp_path / "fuzz-out"
    output.mkdir(exist_ok=True)
    return Runner(
        executable=fake_probe,
        output=output,
        features=features or PROBE_FEATURES["findus-workshop"],
        env=dict(os.environ),
        movies=movies,
        actions=40,
        minimize=minimize,
    )


def test_clean_episode_leaves_no_artifacts(fake_probe, tmp_path):
    runner = make_runner(fake_probe, tmp_path, ["OK"])
    runner.run_episode(seed=5)
    assert runner.episodes == 1
    assert runner.signatures == {}
    assert list(runner.output.iterdir()) == []


def test_generated_input_satisfies_the_strict_probe_protocol(fake_probe, tmp_path):
    runner = make_runner(fake_probe, tmp_path, ["STRICT"])
    for seed in range(4):
        runner.run_episode(seed=seed)
    assert runner.signatures == {}, "generator sent input the workshop probe rejects"


def test_text_entry_stays_inside_the_keyboard_envelope(fake_probe, tmp_path):
    runner = make_runner(
        fake_probe, tmp_path, ["TEXTSTRICT"], features=PROBE_FEATURES["willy-werkel-cars"]
    )
    for seed in range(6):
        runner.run_episode(seed=seed)
    assert runner.commands, "no commands were exchanged"
    assert runner.signatures == {}, "generator sent text the platform keyboard cannot produce"


def test_failure_records_a_reproducible_case_once(fake_probe, tmp_path):
    runner = make_runner(fake_probe, tmp_path, ["FAILSOON"])
    runner.run_episode(seed=1)
    runner.run_episode(seed=2)
    assert len(runner.signatures) == 1
    info = next(iter(runner.signatures.values()))
    assert info["count"] == 2 and info["kind"] == "script-error"
    case = next(p for p in runner.output.iterdir() if p.name.startswith("case-"))
    report = json.loads((case / "report.json").read_text())
    assert report["kind"] == "script-error"
    assert "synthetic lingo failure" in report["error"]
    commands = (case / "commands.txt").read_text().splitlines()
    assert commands, "repro command stream missing"
    outcome, soft = runner.replay(report["movie"], commands)
    assert outcome.kind == "script-error"
    assert "synthetic lingo failure" in outcome.error
    assert soft == []


def test_recovered_script_alert_is_recorded_without_failing(fake_probe, tmp_path):
    runner = make_runner(fake_probe, tmp_path, ["SOFT"])
    runner.run_episode(seed=3)
    runner.run_episode(seed=4)
    assert len(runner.signatures) == 1
    info = next(iter(runner.signatures.values()))
    assert info["kind"] == "script-alert" and info["count"] == 2
    report = json.loads((Path(info["case"]) / "report.json").read_text())
    assert "synthetic recovered alert" in report["error"]
    assert report["commands"] == 3


def test_episode_commands_are_deterministic_per_seed(fake_probe, tmp_path):
    import random

    logs = []
    for _ in range(2):
        stderr = tmp_path / "stderr"
        probe = Probe(fake_probe, "OK", stderr, dict(os.environ))
        probe.receive(timeout=30)
        episode_commands(probe, random.Random(7), 25, PROBE_FEATURES["findus-mucklas"])
        probe.close()
        logs.append(probe.commands)
    assert logs[0] == logs[1]


def test_shrink_isolates_the_triggering_command(fake_probe, tmp_path):
    runner = make_runner(fake_probe, tmp_path, ["TRIGGER"])
    commands = [f"step 5 {100 + i} 100 0" for i in range(12)]
    commands.insert(7, "step 5 666 100 1")
    signature = "synthetic trigger failure"
    minimized = runner.shrink("TRIGGER", commands, signature)
    assert minimized == ["step 5 666 100 1"]


def test_classify_exit_recognizes_sanitizer_reports():
    stderr = (
        "==12==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x60200000eff8\n"
        "SUMMARY: AddressSanitizer: heap-buffer-overflow file.c:42 in handler\n"
    )
    outcome = classify_exit(1, stderr)
    assert outcome.kind == "sanitizer"
    expected = "SUMMARY: AddressSanitizer: heap-buffer-overflow file.c:N in handler"
    assert outcome.signature == expected


def test_classify_exit_recognizes_signals_and_script_errors():
    assert classify_exit(-11, "").kind == "crash"
    assert classify_exit(1, "NATIVE_FAIL boom 3\n") == Outcome(
        "script-error", "NATIVE_FAIL boom 3", "NATIVE_FAIL boom N", 1
    )
    assert classify_exit(2, "").kind == "protocol"

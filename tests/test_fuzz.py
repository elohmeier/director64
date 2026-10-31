"""Synthetic contracts for the randomized input fuzzer; no recovered media."""

from __future__ import annotations

import json
import os
import random
import stat
import time
from pathlib import Path

import pytest
from hypothesis import settings
from hypothesis.stateful import RuleBasedStateMachine, invariant, precondition, rule

from director64.fuzz import (
    FEATURES,
    InvariantModel,
    Outcome,
    Probe,
    ProbeFailure,
    Runner,
    classify_exit,
    episode_commands,
    game_features,
    pick_point,
    player_commands,
    pointer_bounds,
    read_prelude,
)
from director64.project import GameSpec

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
                "movie": "ACTIVITY.DXR" if mode.startswith("TRANSITION") and received else mode,
                "frame": 1,
                "tick": received,
                "depth": 0,
                "quit": False,
                "text_pending": text_pending,
                "error": error,
                "script_errors": soft_errors,
                "last_script_error": last_soft,
                "sprites": [sprite],
                "pointer": {"x": 320, "y": 240},
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
        soft_errors = 0
        last_soft = ""
        if mode == "REBOOTSOFT":
            received = 0
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
    if mode in {"FAILSOON", "TRANSITIONFAIL"} and received >= 3:
        state("synthetic lingo failure in handler N")
        sys.exit(1)
    if mode == "TRIGGER" and "666" in parts:
        state("synthetic trigger failure")
        sys.exit(1)
    if mode in {"SOFT", "REBOOTSOFT"} and received == 3:
        soft_errors += 1
        last_soft = "synthetic recovered alert in handler 7"
    state()
'''


# A probe without text entry whose pads take seven buttons (the STRICT
# double), and one that takes the platform keyboard's characters only.
PADS = {"pad": True, "buttons": 0x7F, "keys": {}}
TEXT = {"pad": False, "buttons": 0, "keys": {}, "text": "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_."}


@pytest.fixture
def fake_probe(tmp_path):
    path = tmp_path / "fake-probe"
    path.write_text(FAKE_PROBE)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


def make_runner(fake_probe, tmp_path, movies, minimize=False, features=None):
    output = tmp_path / "fuzz-out"
    output.mkdir(parents=True, exist_ok=True)
    return Runner(
        executable=fake_probe,
        output=output,
        features=features or PADS,
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
        fake_probe, tmp_path, ["TEXTSTRICT"], features=TEXT
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


def test_reboot_does_not_hide_new_script_alerts(fake_probe, tmp_path):
    probe = Probe(fake_probe, "REBOOTSOFT", tmp_path / "stderr", dict(os.environ))
    probe.receive(timeout=30)
    for _ in range(2):
        for _ in range(3):
            probe.send("step 1 100 100 0")
        probe.send("reboot")
    assert len(probe.soft_events) == 2
    assert probe.close().kind == "ok"


def test_episode_commands_are_deterministic_per_seed(fake_probe, tmp_path):
    import random

    logs = []
    for _ in range(2):
        stderr = tmp_path / "stderr"
        probe = Probe(fake_probe, "OK", stderr, dict(os.environ))
        probe.receive(timeout=30)
        episode_commands(probe, random.Random(7), 25, FEATURES)
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


def test_render_failures_keep_the_cause_instead_of_deduplicating_all_exit_one():
    missing = classify_exit(1, 'runtime failure after "step 664 520 13 0": '
                            'LZ8.CXT:47:exitframe:1976: '
                            'missing packaged streamed sound: pet/cue6\n')
    assert missing.kind == "script-error" and "missing packaged streamed sound" in missing.error
    broken = classify_exit(1, 'runtime failure after "text 3 Name: X": missing authored font 24\n')
    assert broken.kind == "script-error" and broken.signature != missing.signature
    decoder = classify_exit(1, 'Error: probe video clip: invalid decoded dimensions\n at update\n')
    assert decoder.signature != classify_exit(1, '').signature


class InputLog:
    def __init__(self, state):
        self.state = state
        self.commands = []

    def send(self, command, ticks=0):
        self.commands.append(command)
        return self.state


def test_default_mixed_input_exercises_pads_text_and_keyboard():
    probe = InputLog({"movie": "LOGIN.DXR", "stage": [800, 600],
                      "capabilities": {"keyboard": True},
                      "sprites": [{"id": 1, "bounds": [700, 500, 790, 590], "editable": True}]})
    episode_commands(probe, random.Random(7), 500, FEATURES)
    counts = {kind: sum(c.startswith(kind + " ") for c in probe.commands)
              for kind in ("step", "pad", "text", "key")}
    assert all(counts.values()), counts
    assert any(c.startswith("key 36 13 ") for c in probe.commands), "Enter was never sampled"
    assert any(c.startswith("key 123 28 ") for c in probe.commands), "Left was never sampled"


def test_text_input_is_not_admitted_during_an_existing_entry_or_handler():
    for busy in ({"text_pending": True}, {"depth": 1}):
        probe = InputLog({"movie": "LOGIN", "sprites": [
            {"id": 1, "bounds": [100, 100, 200, 200], "editable": True}], **busy})
        episode_commands(probe, random.Random(3), 100, FEATURES)
        assert not any(c.startswith("text ") for c in probe.commands)


def test_targets_include_controls_beyond_the_old_stage_and_use_runtime_dimensions():
    rng = random.Random(7)
    state = {"stage": [800, 600], "sprites": [{"bounds": [700, 500, 791, 591]}]}
    points = [pick_point(state, rng) for _ in range(100)]
    assert sum(700 <= x <= 790 and 500 <= y <= 590 for x, y in points) > 60
    assert all(8 <= x <= 790 and 8 <= y <= 590 for x, y in points)
    assert pointer_bounds(state, {"stage": [640, 480]}) == (790, 590)
    assert pointer_bounds({}, {"stage": [800, 600]}) == (790, 590)
    with pytest.raises(ValueError, match="stage"):
        pointer_bounds({"stage": [True, 600]})


def test_game_features_follow_selected_runtime_and_stage():
    assert game_features(GameSpec.load("findus-birthday"))["stage"] == [800, 600]
    assert game_features(GameSpec.load("findus-birthday"))["keyboard"]
    assert game_features(GameSpec.load("findus-workshop"))["stage"] == [640, 480]
    assert not game_features(GameSpec.load("findus-workshop"))["keyboard"]


def test_failure_after_movie_transition_keeps_boot_movie_for_replay(fake_probe, tmp_path):
    runner = make_runner(fake_probe, tmp_path, ["TRANSITIONFAIL"])
    runner.run_episode(seed=7)
    info = next(iter(runner.signatures.values()))
    report = json.loads((Path(info["case"]) / "report.json").read_text())
    assert report["movie"] == "TRANSITIONFAIL"
    assert report["state"]["movie"] == "ACTIVITY.DXR"
    outcome, _ = runner.replay(report["movie"],
                               (Path(info["case"]) / "commands.txt").read_text().splitlines())
    assert outcome.kind == "script-error"


def test_guided_corpus_retains_reachable_prefixes_and_resumes_them(fake_probe, tmp_path):
    runner = make_runner(fake_probe, tmp_path, ["TRANSITION"])
    runner.guided = True
    runner.corpus_dir = tmp_path / "corpus"
    runner.corpus_identity = {"game": "synthetic", "source_sha256": "a" * 64}
    runner.run_episode(seed=1)
    assert runner.observed_coverage["transition:TRANSITION->ACTIVITY.DXR"] == 1
    assert runner.input_counts["step"] > 0
    files = list(runner.corpus_dir.glob("*.json"))
    assert files
    for path in files:
        item = json.loads(path.read_text())
        assert item["movie"] == "TRANSITION"
        assert player_commands(item["commands"]) == item["commands"]
        assert len(item["commands"]) == 1
    reloaded = make_runner(fake_probe, tmp_path / "another", ["TRANSITION"])
    reloaded.guided = True
    reloaded.corpus_dir = runner.corpus_dir
    reloaded.corpus_identity = runner.corpus_identity
    reloaded.load_corpus()
    assert len(reloaded.corpus) == len(runner.corpus)
    for seed in range(10):
        reloaded.prefix("TRANSITION", random.Random(seed))
    assert reloaded.corpus_replays > 0
    reloaded.run_episode(seed=1)
    assert reloaded.signatures == {}
    reloaded.corpus_identity = {"game": "different"}
    with pytest.raises(ValueError, match="identity"):
        reloaded.load_corpus()


def test_journey_replays_supply_input_preludes_without_state_injection(tmp_path):
    path = tmp_path / "replay.json"
    commands = ["step 30 700 500 0", "key 36 13 1", "key 36 13 0", "text 7 Anna",
                "document cancel", "reboot"]
    path.write_text(json.dumps({"commands": commands, "error": None,
                                "states": [], "checkpoints": []}))
    assert read_prelude(tmp_path) == commands
    # A dirty process exit may leave replay.error unset; the suite's final
    # status still disqualifies that run as a successful journey prelude.
    report = tmp_path / "report.json"
    report.write_text(json.dumps({"status": "failed", "error": "probe exited 7"}))
    with pytest.raises(ValueError, match="did not pass"):
        read_prelude(path)
    report.unlink()
    path.write_text(json.dumps({"commands": commands, "error": "failed"}))
    with pytest.raises(ValueError, match="did not pass"):
        read_prelude(path)
    for command in ("movie SECRET.DXR", "step 1 2 3 0\nmovie SECRET.DXR", "inspect gstate",
                    "step -1 2 3 0", "pad 1 999 0 0", "key 999 0 1"):
        with pytest.raises(ValueError):
            player_commands([command])


SAVE_MODEL = {
    "schema_version": 1, "id": "synthetic-profile-reload-v1", "rules": [
        {"id": "profile-survives", "after": {"movie": "ROOM", "/saves/player.txt": "Anna"},
         "when": {"movie": "LOGIN"}, "expect": {"/saves/player.txt": "Anna"}}
    ],
}


def test_stateful_oracle_accepts_valid_reloads_and_detects_silent_save_loss():
    from hypothesis.stateful import run_state_machine_as_test

    class ProfileSession(RuleBasedStateMachine):
        def __init__(self, lose_save=False):
            super().__init__()
            self.lose_save = lose_save
            self.state = {"movie": "LOGIN", "saves": {}}
            self.model = InvariantModel(SAVE_MODEL)

        @rule()
        def register_and_commit(self):
            self.state = {"movie": "ROOM", "saves": {"player.txt": "Anna"}}

        @rule()
        def idle(self):
            pass

        @precondition(lambda self: bool(self.state["saves"]))
        @rule()
        def reboot_and_reload(self):
            self.state = {"movie": "LOGIN",
                          "saves": {} if self.lose_save else self.state["saves"].copy()}

        @invariant()
        def saved_profile_is_retained(self):
            self.model.observe(self.state)

    options = settings(max_examples=30, stateful_step_count=15, derandomize=True,
                       database=None, deadline=None)
    run_state_machine_as_test(ProfileSession, settings=options)
    with pytest.raises(ProbeFailure, match="profile-survives"):
        run_state_machine_as_test(lambda: ProfileSession(lose_save=True), settings=options)


def test_invariant_failures_retain_the_model_and_exact_replay(tmp_path):
    executable = tmp_path / "lossy-probe"
    executable.write_text(f'''#!{os.sys.executable}
import json, sys
print(json.dumps({{"movie":"LOGIN", "saves":{{}}}}), flush=True)
for line in sys.stdin:
    state = {{"movie":"LOGIN", "saves":{{}}}} if line.strip()=="reboot" else {{
        "movie":"ROOM", "saves":{{"player.txt":"Anna"}}}}
    print(json.dumps(state), flush=True)
''')
    executable.chmod(0o755)
    runner = make_runner(executable, tmp_path, ["LOGIN"])
    runner.invariant_spec = SAVE_MODEL
    runner.preludes = [["step 1 100 100 0", "reboot"]]
    runner.run_episode(seed=7)
    info = next(iter(runner.signatures.values()))
    report = json.loads((Path(info["case"]) / "report.json").read_text())
    assert report["kind"] == "invariant"
    assert report["invariant_model"] == SAVE_MODEL
    assert runner.invariant_checks["profile-survives"] == 1
    outcome, _ = runner.replay("LOGIN", ["step 1 100 100 0", "reboot"])
    assert outcome.kind == "invariant"
    assert outcome.signature == report["signature"]


def test_model_requires_explicit_supported_expectations():
    with pytest.raises(ValueError, match="unique ids"):
        InvariantModel({"schema_version": 1, "id": "bad", "rules": [{"id": "empty"}]})
    invalid = {"id": "bad", "when": {"movie": {"typo": "LOGIN"}}, "expect": {"frame": 1}}
    with pytest.raises(ValueError, match="unknown expectation"):
        InvariantModel({"schema_version": 1, "id": "bad", "rules": [invalid]})


@pytest.mark.parametrize("option", ["--duration", "--episode-timeout"])
@pytest.mark.parametrize("value", ["nan", "inf", "-inf"])
def test_cli_rejects_nonfinite_time_limits(option, value):
    from director64.fuzz import main

    with pytest.raises(SystemExit) as error:
        main(GameSpec.load("findus-workshop"), [option + "=" + value])
    assert error.value.code == 2


def test_prelude_bounds_cover_command_count_and_aggregate_service_ticks():
    with pytest.raises(ValueError, match="4096 commands"):
        player_commands(["step 1 10 10 0"] * 4097)
    with pytest.raises(ValueError, match="1000000 service ticks"):
        player_commands(["step 100000 10 10 0"] * 11)
    assert len(player_commands(["step 100000 10 10 0"] * 10)) == 10


def assert_process_stopped(pid):
    status = Path(f"/proc/{pid}/status")
    deadline = time.monotonic() + 1
    while True:
        try:
            contents = status.read_text()
        except (FileNotFoundError, ProcessLookupError):
            return
        if "State:\tZ" in contents:
            return
        assert time.monotonic() < deadline, "decoder child survived probe cleanup"
        time.sleep(0.01)


def test_episode_deadline_kills_probe_and_decoder_descendants(tmp_path):
    executable = tmp_path / "blocked-probe"
    child_pid = tmp_path / "child.pid"
    executable.write_text(f'''#!{os.sys.executable}
import json, pathlib, subprocess, sys, time
child=subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
pathlib.Path({str(child_pid)!r}).write_text(str(child.pid))
print(json.dumps({{"movie":"START", "frame":1}}), flush=True)
for line in sys.stdin: time.sleep(30)
''')
    executable.chmod(0o755)
    probe = Probe(executable, "START", tmp_path / "stderr", dict(os.environ), timeout=0.5)
    started = time.monotonic()
    try:
        probe.receive(timeout=30)
        with pytest.raises(ProbeFailure, match="deadline exceeded"):
            probe.send("step 1 10 10 0")
    finally:
        probe.kill()
    assert time.monotonic() - started < 5
    assert probe.process.poll() is not None
    assert_process_stopped(child_pid.read_text())


def test_replay_exception_still_terminates_the_probe(fake_probe, tmp_path, monkeypatch):
    original_send = Probe.send
    processes = []

    def interrupted(probe, command, ticks=0):
        processes.append(probe.process)
        original_send(probe, command, ticks)
        raise KeyboardInterrupt()

    runner = make_runner(fake_probe, tmp_path, ["OK"])
    monkeypatch.setattr(Probe, "send", interrupted)
    with pytest.raises(KeyboardInterrupt):
        runner.replay("OK", ["step 1 10 10 0"])
    assert processes and all(p.poll() is not None for p in processes)
    assert not list(runner.output.glob("replay-*.stderr"))


def test_clean_probe_shutdown_does_not_leave_decoder_children(tmp_path):
    executable = tmp_path / "clean-probe"
    child_pid = tmp_path / "child.pid"
    executable.write_text(f'''#!{os.sys.executable}
import json, pathlib, subprocess, sys
child=subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
pathlib.Path({str(child_pid)!r}).write_text(str(child.pid))
print(json.dumps({{"movie":"START", "frame":1}}), flush=True)
for line in sys.stdin: print(json.dumps({{"movie":"START"}}), flush=True)
''')
    executable.chmod(0o755)
    probe = Probe(executable, "START", tmp_path / "stderr", dict(os.environ), timeout=5)
    try:
        probe.receive(timeout=5)
        assert probe.close().kind == "ok"
        assert_process_stopped(child_pid.read_text())
    finally:
        probe.kill()


def test_replay_rejects_a_failure_from_a_different_source_edition(fake_probe, tmp_path):
    from director64.fuzz import main

    case = tmp_path / "case"
    case.mkdir()
    (case / "report.json").write_text(json.dumps({"movie": "START", "provenance": {
        "identity": {"game": "findus-workshop", "source_sha256": "0" * 64}}}))
    (case / "commands.txt").write_text("step 1 10 10 0\n")
    with pytest.raises(ValueError, match="different game or source edition"):
        main(GameSpec.load("findus-workshop"), ["--replay", str(case),
             "--probe", str(fake_probe), "--output", str(tmp_path / "out")])


def test_replaying_a_known_script_alert_returns_failure_when_it_recurs(fake_probe, tmp_path):
    from director64.fuzz import main

    case = tmp_path / "case"
    case.mkdir()
    (case / "report.json").write_text(json.dumps({"movie": "SOFT", "kind": "script-alert",
        "signature": "synthetic recovered alert in handler N"}))
    (case / "commands.txt").write_text("step 1 10 10 0\n" * 3)
    assert main(GameSpec.load("findus-workshop"), [
        "--replay", str(case), "--probe", str(fake_probe), "--output", str(tmp_path / "out")]) == 1


def test_plain_input_replay_uses_the_selected_games_entry_movie(tmp_path):
    from director64.fuzz import main

    executable = tmp_path / "entry-probe"
    executable.write_text(f'''#!{os.sys.executable}
import json, sys
if sys.argv[1] != "PF3": sys.exit(2)
print(json.dumps({{"movie":"PF3", "frame":1}}), flush=True)
for line in sys.stdin: print(json.dumps({{"movie":"PF3", "frame":1}}), flush=True)
''')
    executable.chmod(0o755)
    commands = tmp_path / "commands.txt"
    commands.write_text("step 1 10 10 0\n")
    assert main(GameSpec.load("findus-birthday"), ["--replay", str(commands),
                "--probe", str(executable), "--output", str(tmp_path / "out")]) == 0


def test_controller_prefixes_keep_single_player_shape():
    assert player_commands(["pad 60 80 0 1"]) == ["pad 60 80 0 1"]
    for player in range(4):
        with pytest.raises(ValueError, match="player indices are unsupported"):
            player_commands([f"pad {player} 60 80 0 1"])

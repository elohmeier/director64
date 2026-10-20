"""Journey coverage and failure/performance gates, with no retail media."""

import copy
import json
import os
import subprocess
import sys
from dataclasses import replace

import pytest

from director64 import journeys
from director64.project import GameSpec, repository


def test_every_port_has_registered_journeys():
    runnable, unavailable = [], []
    for path in repository().glob("games/*/game.toml"):
        game = GameSpec.load(path.parent.name)
        if game.data["status"] == "catalogued":
            unavailable.append(game.slug)
            continue
        runnable.append(game.slug)
        for plan in journeys.plans(game):
            if plan.get("legacy"):
                assert (game.directory / "host" / (plan["legacy"] + ".py")).is_file() or (
                    game.slug == "findus-workshop"
                    and plan["legacy"] in {"hubs", "profiles", "garden"})
            else:
                assert sum("label" in step for step in plan["steps"]) >= 2
                assert any("movie" in step.get("expect", {}) or "movie" in step.get("wait", {})
                           for step in plan["steps"])
    assert len(runnable) == 20
    assert len(unavailable) == 0


def test_expectations_fail_on_absent_values_and_wrong_destinations():
    state = {"movie": "MENU.DXR", "save_generation": 1, "globals": {"name": "ANNA"}}
    journeys.expect(state, {"save_generation": {"gte": 1}, "globals.name": "ANNA"})
    journeys.expect({"saves": {"player/anna/game.txt": "ANNA"}},
                    {"/saves/player~1anna~1game.txt": {"contains": "ANNA"}})
    with pytest.raises(ValueError, match="movie"):
        journeys.expect(state, {"movie": "ACTIVITY.DXR"})
    with pytest.raises(ValueError, match="missing"):
        journeys.expect(state, {"globals.missing": {"gte": 1}})
    with pytest.raises(ValueError, match="unknown expectation"):
        journeys.expect(state, {"movie": {"typo": True}})


def synthetic_probe(tmp_path, monkeypatch, failure=""):
    executable = tmp_path / "probe"
    executable.write_text(f"""#!{sys.executable}
import json, sys
state = {{"movie": "START.DXR", "error": "", "script_errors": 0, "quit": False}}
print(json.dumps(state), flush=True)
for command in sys.stdin:
    state["movie"] = "ACTIVITY.DXR"
    if {failure!r} == "alert":
        state.update(script_errors=1, last_script_error="bad recovered script")
    print(json.dumps(state), flush=True)
if {failure!r} == "exit":
    sys.exit(7)
""")
    executable.chmod(0o755)
    monkeypatch.setattr("director64.web.probe_path", lambda game: executable)
    return replace(GameSpec.load("findus-workshop"), root=tmp_path)


def test_script_requires_its_destination_and_saves_replay(tmp_path, monkeypatch):
    game = synthetic_probe(tmp_path, monkeypatch)
    output = tmp_path / "out"
    plan = {"steps": [{"idle": 1, "expect": {"movie": "ACTIVITY.DXR"}, "label": "activity"}]}
    assert journeys.run_script(game, plan, output)["status"] == "passing"
    replay = json.loads((output / "replay.json").read_text())
    assert replay["commands"] == ["step 1 400 300 0"]
    assert replay["checkpoints"] == [{"label": "activity", "state": 1}]
    plan["steps"][0]["expect"]["movie"] = "WRONG.DXR"
    with pytest.raises(ValueError, match="WRONG.DXR"):
        journeys.run_script(game, plan, output)
    assert "WRONG.DXR" in json.loads((output / "replay.json").read_text())["error"]


@pytest.mark.parametrize("failure", ["alert", "exit"])
def test_script_alert_and_dirty_shutdown_cannot_pass(tmp_path, monkeypatch, failure):
    game = synthetic_probe(tmp_path, monkeypatch, failure)
    with pytest.raises(ValueError, match="bad recovered script|probe protocol"):
        journeys.run_script(game, {"steps": [{"idle": 1}]}, tmp_path / "out")


@pytest.mark.parametrize("command", ["movie SECRET.DXR", "", "step 1 0 0 0\nmovie SECRET.DXR"])
def test_journey_rejects_state_injection(tmp_path, monkeypatch, command):
    game = synthetic_probe(tmp_path, monkeypatch)
    with pytest.raises(ValueError, match="cannot inject"):
        journeys.run_script(game, {"steps": [{"command": command}]}, tmp_path / "out")


def test_unreached_wait_has_a_finite_tick_budget(tmp_path, monkeypatch):
    game = synthetic_probe(tmp_path, monkeypatch)
    output = tmp_path / "out"
    with pytest.raises(ValueError, match="MISSING.DXR"):
        journeys.run_script(game, {"steps": [{"wait": {"movie": "MISSING.DXR"}, "ticks": 120}]},
                            output)
    replay = json.loads((output / "replay.json").read_text())
    assert len(replay["commands"]) == 2


def baseline_report():
    return {"source_sha256": "a" * 64, "plan_sha256": "b" * 64,
            "environment": {"host": "same", "rendered": True}, "status": "passing",
            "performance": {"command_stream_sha256": ["c" * 64], "elapsed_ms": 100,
                            "boot_ms": 10, "p95_command_ms": 10, "max_command_ms": 10,
                            "peak_rss_bytes": 10000,
                            "wasm_bytes": 1000, "scenes": {
                                "MENU.DXR": {"commands": 10, "elapsed_ms": 90},
                                "ACTIVITY.DXR": {"commands": 1, "elapsed_ms": 10}}}}


def test_performance_gates_time_memory_and_a_slow_scene():
    baseline = baseline_report()
    current = copy.deepcopy(baseline)
    assert journeys.compare(current, baseline, 1.25, 1.1) == []
    current["performance"].update(elapsed_ms=130, peak_rss_bytes=11001)
    current["performance"]["scenes"]["ACTIVITY.DXR"]["elapsed_ms"] = 20
    failures = journeys.compare(current, baseline, 1.25, 1.1)
    assert any("elapsed_ms" in failure for failure in failures)
    assert any("peak_rss_bytes" in failure for failure in failures)
    assert any("ACTIVITY.DXR" in failure for failure in failures)


@pytest.mark.parametrize("change", ["source_sha256", "source_image", "plan_sha256",
                                    "environment", "runner_sha256", "harness_sha256",
                                    "commands", "status"])
def test_incomparable_baselines_are_rejected(change):
    baseline = baseline_report()
    current = copy.deepcopy(baseline)
    if change == "commands":
        current["performance"]["command_stream_sha256"] = ["different"]
    elif change == "status":
        baseline["status"] = "failed"
    else:
        current[change] = "different"
    assert journeys.compare(current, baseline, 1.25, 1.1)


def test_performance_retains_multiple_sessions_and_excludes_boot_from_p95(tmp_path):
    for index in range(2):
        samples = [{"command": command, "movie": "MENU.DXR", "elapsed_ms": duration,
                    "cpu_ms": 1, "peak_rss_bytes": 1000, "wasm_bytes": 500, "script_errors": 0}
                   for command, duration in (("boot", 100), ("step 1 0 0 0", 2))]
        (tmp_path / f"{index}.jsonl").write_text("\n".join(map(json.dumps, samples)))
    report = journeys.performance(tmp_path)
    assert report["sessions"] == 2
    assert report["elapsed_ms"] == 204
    assert report["p95_command_ms"] == 2
    assert report["boot_ms"] == 200
    assert len(report["command_stream_sha256"]) == 2


def test_timeout_fails_and_terminates_worker_group(tmp_path, monkeypatch):
    popen = subprocess.Popen
    children = []

    def start(command, **kwargs):
        child = popen([sys.executable, "-c", "import time; time.sleep(30)"], **kwargs)
        children.append(child)
        return child

    monkeypatch.setattr(journeys.subprocess, "Popen", start)
    monkeypatch.setattr(journeys, "environment", lambda: {})
    game = replace(GameSpec.load("findus-workshop"), root=tmp_path)
    report = journeys.run_case(game, {"name": "test", "covers": ["timeout"]}, tmp_path / "out",
                               .1, None, 1.25, 1.1)
    assert report["status"] == "failed"
    assert "exceeded" in report["error"]
    assert children[0].returncode == -9
    assert json.loads((tmp_path / "out/report.json").read_text())["status"] == "failed"


def test_suite_continues_after_failure_and_records_catalogued_games(tmp_path, monkeypatch):
    root = repository()
    monkeypatch.setattr(journeys, "repository", lambda: root)
    visited = []

    def run_case(game, plan, *args):
        visited.append(game.slug)
        return {"game": game.slug, "status": "failed" if len(visited) == 1 else "passing"}

    monkeypatch.setattr(journeys, "run_case", run_case)
    assert journeys.main(None, ["--output", str(tmp_path)]) == 1
    summary = json.loads((tmp_path / "summary.json").read_text())
    assert len(set(visited)) == 20
    assert len(summary["unavailable"]) == 0
    assert summary["status"] == "failed"


def test_all_journeys_can_be_listed_without_game_media():
    result = subprocess.run([sys.executable, "-m", "director64", "journey", "--game", "all",
                             "--list"], capture_output=True, text=True,
                            env=os.environ | {"DIRECTOR64_WORK_DIR": "unused"})
    assert result.returncode == 0, result.stderr
    assert "findus-birthday/player-registration" in result.stdout
    assert "fritz-fertig-1/opening-player-screen" in result.stdout


def test_worker_rejects_a_plan_edited_during_a_suite(tmp_path):
    with pytest.raises(ValueError, match="plan changed"):
        journeys.main(GameSpec.load("findus-workshop"), ["profiles", "--worker", "--output",
                                                       str(tmp_path), "--plan-sha256", "bad"])


def test_original_mucklas_activity_shortcut_is_preserved(tmp_path, monkeypatch):
    visited = []

    def run_case(game, plan, *args):
        visited.append(plan)
        return {"status": "passing"}

    monkeypatch.setattr(journeys, "run_case", run_case)
    assert journeys.main(GameSpec.load("findus-mucklas"),
                         ["--activity", "--output", str(tmp_path)]) == 0
    assert len(visited) == 1
    assert visited[0]["name"] == "full_journey" and visited[0]["activity"]


def test_median_timings_keep_peak_memory_and_every_correctness_failure():
    reports = [baseline_report() for _ in range(3)]
    for report, elapsed, memory in zip(
            reports, [100, 110, 300], [10000, 11000, 10500], strict=True):
        report['performance'].update(cpu_ms=elapsed, elapsed_ms=elapsed, peak_rss_bytes=memory)
        for scene in report['performance']['scenes'].values():
            scene['max_command_ms'] = 10
    result = journeys.aggregate(reports)
    assert result['performance']['elapsed_ms'] == 110
    assert result['performance']['peak_rss_bytes'] == 11000
    assert result['timing_ranges']['elapsed_ms'] == {'min': 100, 'max': 300}
    reports[1]['status'] = 'failed'
    assert journeys.aggregate(reports)['status'] == 'failed'
    reports[1]['status'] = 'passing'
    reports[1]['performance']['command_stream_sha256'] = ['different']
    with pytest.raises(ValueError, match='command streams differ'):
        journeys.aggregate(reports)

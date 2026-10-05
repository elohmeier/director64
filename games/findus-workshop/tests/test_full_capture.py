import json
import os

import pytest
from director64_findus_workshop import full_capture

from director64.project import work_dir


def test_prepare_freezes_every_shared_input_before_next_build(tmp_path, monkeypatch):
    paths = [
        f"{work_dir()}/n64/findus-workshop-probe.z64",
        f"{work_dir()}/n64/probe/findus-workshop-probe.elf",
        f"{work_dir()}/director/c/replay.json",
        f"{work_dir()}/director/c/replay.inc",
        f"{work_dir()}/director/packed.json",
    ]
    for name in paths:
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(name)
    checked, verified = [], []
    monkeypatch.setattr(full_capture, "snapshot", lambda root, run: checked.append((root, run)))
    monkeypatch.setattr(
        full_capture,
        "verify_rom_inputs",
        lambda game, profile, rom: verified.append((profile, rom)),
    )
    run = full_capture.prepare(tmp_path)
    assert checked == [(tmp_path, run)]
    # The probe ROM must match the checkout before it is frozen into the run.
    assert verified == [("probe", tmp_path / paths[0])]
    for name in paths:
        path = tmp_path / name
        path.write_text("next build")
        assert (run / "inputs" / path.name).read_text() == name
    assert all((run / d).is_dir() for d in ("config", "data", "cache"))


@pytest.mark.parametrize("target", ["MOSSEN", "BREDHOGN"])
def test_capture_reads_frozen_duration_and_isolates_saves(tmp_path, monkeypatch, target):
    (tmp_path / "inputs").mkdir()
    (tmp_path / "inputs/replay.json").write_text(json.dumps({"ticks": 61, "target": target}))
    commands, events = [], []
    monkeypatch.setattr(
        full_capture.subprocess, "run", lambda cmd, **kw: commands.append((cmd, kw))
    )

    def validate(run):
        assert run == tmp_path
        events.append("validate")
        return {"status": "passing"}

    def reboot(run):
        assert run == tmp_path
        events.append("reboot")
        return {"status": "passing", "reload": {"status": "passing"}}

    monkeypatch.setattr(full_capture, "validate_capture", validate)
    monkeypatch.setattr(full_capture, "reboot", reboot)
    before = dict(os.environ)
    result = full_capture.capture(tmp_path)
    assert os.environ == before
    emulator, options = commands[0]
    assert emulator[emulator.index("--capture-duration") + 1] == "17"
    assert options["env"]["XDG_DATA_HOME"] == str(tmp_path / "data")
    assert emulator[-1] == str(tmp_path / "inputs/findus-workshop-probe.z64")
    assert events == (["validate", "reboot"] if target == "BREDHOGN" else ["validate"])
    assert json.loads((tmp_path / "result.json").read_text()) == result

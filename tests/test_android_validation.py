"""Android replay must exercise platform input and preserve the user's installed game."""

import json
from pathlib import Path
from types import SimpleNamespace

import pytest

from director64 import android
from director64 import android_validation as validation
from director64.project import GameSpec
from director64.visual import assess_capture, write_png


def test_validation_apk_identity_and_receipt_are_separate(tmp_path, monkeypatch):
    game = SimpleNamespace(dist=tmp_path, slug="test-game", source={"sha256": "disc"},
                           image_identity={})
    assert android.package_name(game) == "org.director64.tv.test_game"
    assert android.package_name(game, validation=True) == "org.director64.tv.test_game.validation"
    production, isolated = android.apk_path(game), android.apk_path(game, validation=True)
    assert production != isolated
    isolated.write_bytes(b"test APK")
    monkeypatch.setattr(android, "apk_input_digest", lambda *args, **kwargs: "inputs")
    receipt = {"source_sha256": "disc", "abis": ["x86_64"], "inputs": "inputs",
               "apk_sha256": android.web.sha256(isolated)}
    stamp = android.apk_receipt_path(game, validation=True)
    stamp.write_text(json.dumps(receipt))
    with pytest.raises(ValueError, match="stale"):
        android.verify_apk(game, validation=True)
    stamp.write_text(json.dumps(receipt | {"validation": True}))
    assert android.verify_apk(game, validation=True) == isolated
    assert not production.exists()


def test_full_native_stage_maps_to_letterboxed_android_surface():
    assert validation.screen_point((799, 599), [160, 0, 960, 720], (800, 600)) == (1118, 718)
    assert validation.screen_point((0, 0), [160, 0, 960, 720], (800, 600)) == (160, 0)
    with pytest.raises(ValueError, match="outside"):
        validation.screen_point((800, 0), [0, 0, 960, 720], (800, 600))


@pytest.mark.parametrize("command", ["movie SECRET.DXR", "step 1 0 0 1\nreboot",
                                    "pad 10 80 0 1", "key 0 97 1", "shell rm -rf /data"])
def test_replay_rejects_injection_and_unrepresentable_inputs(command):
    with pytest.raises(ValueError, match="replay|command"):
        validation.validate_plan({"steps": [{"command": command}]})


def test_replay_checkpoint_indexes_include_boot_state():
    plan = validation.replay_plan({"commands": ["step 1 0 0 0", "reboot"],
                                   "states": [{"movie": "BOOT"}, {"movie": "MENU"},
                                              {"movie": "BOOT"}],
                                   "checkpoints": [{"label": "menu", "state": 1}]})
    assert plan["steps"][0]["label"] == "menu"
    assert plan["steps"][0]["expect"] == {"movie": "MENU"}
    assert "label" not in plan["steps"][1]


def test_document_cancel_is_android_back_only_for_an_observed_modal(tmp_path, monkeypatch):
    validation.validate_plan({"steps": [{"command": "document cancel"}]})
    adb = FakeAdb()
    session = validation.Session(GameSpec.load("findus-birthday"), adb, tmp_path)
    observations = iter([{"document_id": 7, "document_kind": 4},
                         {"document_id": 0, "document_kind": 0}])
    session.observe = lambda: next(observations)
    session.idle = lambda ticks: {"resumed_ticks": ticks}
    monkeypatch.setattr(validation.time, "sleep", lambda seconds: None)
    assert session.command("document cancel") == {"resumed_ticks": 6}
    assert adb.calls == [("input", "keyevent", "KEYCODE_BACK")]
    session.observe = lambda: {"document_id": 0, "document_kind": 0}
    with pytest.raises(ValueError, match="requires an active"):
        session.command("document cancel")
    assert len(adb.calls) == 1


class FakeAdb:
    def __init__(self):
        self.calls = []
        self.deadline = float("inf")

    def shell(self, *args):
        self.calls.append(args)
        return SimpleNamespace(stdout="", returncode=0)


def test_replay_uses_android_touch_dispatch_instead_of_runtime_rpc(tmp_path):
    game = GameSpec.load("findus-birthday")
    adb = FakeAdb()
    session = validation.Session(game, adb, tmp_path)
    session.surface = [160, 0, 960, 720]
    session.idle = lambda ticks: {"ticks": ticks}
    session.pointer(10, 799, 599, True)
    session.pointer(5, 700, 500, True)
    session.pointer(20, 700, 500, False)
    assert [call[:4] for call in adb.calls] == [
        ("input", "touchscreen", "motionevent", "DOWN"),
        ("input", "touchscreen", "motionevent", "MOVE"),
        ("input", "touchscreen", "motionevent", "UP"),
    ]
    assert adb.calls[0][4:] == ("1118", "718")


def test_text_replay_opens_and_uses_android_edit_dialog(tmp_path):
    game = GameSpec.load("findus-birthday")
    adb = FakeAdb()
    session = validation.Session(game, adb, tmp_path)
    session.idle = lambda ticks: {"ticks": ticks}
    session.observe = lambda: {"text_entry": True, "state": {}}
    session.text(127, "ANNA MARIA")
    assert adb.calls == [("input", "keycombination", "113", "29"),
                         ("input", "text", "ANNA%sMARIA"),
                         ("input", "keyevent", "66")]
    with pytest.raises(ValueError, match="ASCII"):
        session.text(127, "ANNA;reboot")


def test_text_replay_waits_for_sliding_field_before_clicking(tmp_path):
    session = validation.Session(GameSpec.load("findus-birthday"), FakeAdb(), tmp_path)
    hidden = {"text_entry": False, "state": {"sprites": [
        {"id": 127, "bounds": [900, 100, 1100, 140]}]}}
    ready = {"text_entry": False, "state": {"sprites": [
        {"id": 127, "bounds": [200, 100, 400, 140]}]}}
    observed = iter([hidden, {"text_entry": True, "state": {}}])
    session.observe = lambda: next(observed)
    waits, points = [], []

    def idle(ticks):
        waits.append(ticks)
        return ready

    session.idle = idle
    session.pointer = lambda ticks, x, y, down: points.append((x, y, down))
    session.text(127, "ANNA")
    assert points == [(300, 120, True), (300, 120, False)]
    assert waits == [6, 60]


def test_plan_validation_precedes_adb_or_save_mutations(tmp_path, monkeypatch):
    game = GameSpec.load("findus-birthday")
    monkeypatch.setattr(android, "verify_apk", lambda *args, **kwargs: pytest.fail("verified APK"))
    report = validation.run(game, {"name": "bad", "steps": [{"command": "movie SECRET"}]},
                            FakeAdb(), tmp_path / "run")
    assert report["status"] == "failed" and "cannot faithfully" in report["error"]
    assert json.loads((tmp_path / "run/report.json").read_text())["status"] == "failed"


def test_capture_rejects_corrupted_screenshot_before_using_it(tmp_path):
    with pytest.raises(ValueError, match="PNG"):
        validation.png_size(b"error: no Android device")
    path = tmp_path / "screen.png"
    write_png(path, 2, 1, bytes((20, 30, 40, 255)) * 2)
    assert validation.png_size(path.read_bytes()) == (2, 1)


def test_callback_telemetry_is_not_a_request_for_a_pcm_recording(tmp_path, monkeypatch):
    screen = tmp_path / "device.png"
    write_png(screen, 2, 1, bytes((20, 30, 40, 255)) * 2)
    adb = FakeAdb()
    adb.run = lambda *args, **kwargs: SimpleNamespace(stdout=screen.read_bytes())
    session = validation.Session(GameSpec.load("findus-birthday"), adb, tmp_path)
    session.observe = lambda: {"state": {"movie": "SYNTHETIC"}, "surface": [0, 0, 2, 1],
                               "audio": {"samples": 48000, "nonzero": 100},
                               "paused": False, "has_surface": True}

    def resize(command, **kwargs):
        write_png(Path(command[-1]), 800, 600, bytes((20, 30, 40, 255)) * (800 * 600))

    monkeypatch.setattr(validation.subprocess, "run", resize)
    item = session.capture("checkpoint")
    assert item["android"]["audio"]["nonzero"] == 100
    (tmp_path / "capture.json").write_text(json.dumps({
        "status": "captured", "stage": [800, 600], "checkpoints": [item]}))
    assert assess_capture(tmp_path)["status"] == "unbaselined"


def test_target_selection_is_explicit():
    with pytest.raises(ValueError, match="select one"):
        validation.Adb(Path("/work"))
    with pytest.raises(ValueError, match="choose"):
        validation.Adb(Path("/work"), emulator=True, serial="physical")


def test_audio_checks_catch_missing_cues_and_excess_clipping():
    before = {"rate": 48000, "channels": 2, "samples": 100, "nonzero": 10,
              "clipped": 0, "energy": 1}
    after = before | {"samples": 96100, "nonzero": 96010, "energy": 24001}
    metrics = validation.audio_interval(before, after, {"min_rms": 0.1, "min_seconds": 1})
    assert metrics["rms"] == 0.5 and metrics["seconds"] == 1
    with pytest.raises(ValueError, match="min_rms"):
        validation.audio_interval(before, before | {"samples": 96100}, {"min_rms": 0.1})
    with pytest.raises(ValueError, match="max_clipped_ratio"):
        validation.audio_interval(before, after | {"clipped": 96000}, {"max_clipped_ratio": 0.01})


@pytest.mark.parametrize("timeout", [0, -1, float("nan"), float("inf"), float("-inf")])
def test_journey_boundary_rejects_unbounded_timeouts(tmp_path, timeout):
    with pytest.raises(ValueError, match="finite and positive"):
        validation.run(GameSpec.load("findus-birthday"), {"steps": [{"idle": 1}]},
                       FakeAdb(), tmp_path / "invalid", timeout=timeout)
    assert not (tmp_path / "invalid").exists()


@pytest.mark.parametrize("timeout", ["nan", "inf", "-inf", "0"])
def test_cli_rejects_unbounded_timeouts_before_accessing_device(timeout):
    with pytest.raises(SystemExit) as error:
        validation.main(GameSpec.load("findus-birthday"), ["--emulator", "--timeout=" + timeout])
    assert error.value.code == 2

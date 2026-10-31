#!/usr/bin/env python3
"""Bounded real-Android pointer-role and modal proof, using an authored fixture."""
from __future__ import annotations

import argparse
import json
import math
import shutil
import subprocess
import time
from pathlib import Path

from pointer_fixture import fixture_game

from director64 import android
from director64.android_validation import Adb, Session
from director64.project import repository


def run(output: Path, timeout: float) -> dict:
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("timeout must be finite and positive")
    game = fixture_game(repository())
    output = output.resolve()
    if not output.is_relative_to(game.root / "build"):
        raise ValueError("evidence must remain under ignored build/")
    apk = android.verify_apk(game, validation=True)
    receipt_path = android.apk_receipt_path(game, validation=True)
    receipt = json.loads(receipt_path.read_text())
    output.mkdir(parents=True, exist_ok=False)
    adb = Adb(game.root, emulator=True)
    adb.deadline = time.monotonic() + timeout
    session = Session(game, adb, output, build_id=receipt["inputs"])
    report = {"status": "running", "scope": "authored Android pointer-role fixture",
              "physical_device_evidence": False, "original_projector_evidence": False,
              "checks": [], "receipt": receipt}

    def check(value, name):
        if not value:
            raise AssertionError(name)
        report["checks"].append(name)

    def mouse(x, y, *, tap=False):
        left, top, width, height = session.surface
        sx, sy = left + round(x * width / 800), top + round(y * height / 600)
        session.record({"android_mouse": "tap" if tap else "move", "stage": [x, y]})
        if tap:
            adb.shell("input", "mouse", "tap", str(sx), str(sy))
        else:
            adb.shell("env", "CLASSPATH=/data/local/tmp/director64-pointer-input.jar",
                      "app_process", "/", "DirectorPointerInput", str(sx), str(sy))

    def wait(predicate):
        limit = min(adb.deadline, time.monotonic() + 10)
        latest = None
        while time.monotonic() < limit:
            latest = session.observe()
            if predicate(latest):
                return latest
            time.sleep(.05)
        raise AssertionError("Android pointer state did not settle: " + json.dumps(latest))

    def role(label, x, y, kind, icon, software):
        mouse(x, y)
        value = wait(lambda o: o["native_pointer_kind"] == kind
                     and o["android_pointer_icon"] == icon
                     and o["state"]["pointer_presentation"]["requested"] == kind
                     and o["state"]["pointer_presentation"]["acknowledged"] == kind
                     and o["state"]["pointer_presentation"]["software"] == software)
        check(value["state"]["pointer_presentation"]["physical"],
              label + " uses Android mouse input")
        session.capture(label)
        check(True, label + " has one requested cursor representation")

    try:
        report["device"] = adb.identity()
        check(report["device"]["kind"] == "android-emulator", "isolated emulator provenance")
        subprocess.run(["bash", "tools/android/build-pointer-input.sh"],
                       cwd=game.root, check=True, timeout=60)
        adb.run("push", "/workdir/build/tools/android-pointer-input/pointer-input.jar",
                "/data/local/tmp/director64-pointer-input.jar")
        shutil.copyfile(apk, output / "tested-fixture.apk")
        shutil.copyfile(receipt_path, output / "tested-fixture.apk.json")
        adb.run("install", "-r", "-t", "/workdir/" + apk.relative_to(game.root).as_posix())
        session.launch(reboot=True)
        session.capture("boot")
        role("native-arrow", 400, 450, 0, 1000, False)
        role("native-text", 150, 90, 1, 1008, False)
        role("authored-invert-fallback", 200, 200, -1, 0, True)
        role("native-wait", 150, 290, 4, 1004, False)
        role("hidden", 150, 390, 200, 0, False)
        role("native-return", 400, 450, 0, 1000, False)
        session.record({"android_key": "DPAD_RIGHT"})
        adb.shell("input", "keyevent", "KEYCODE_DPAD_RIGHT")
        wait(lambda o: o["native_pointer_kind"] == -1
             and o["state"]["pointer_presentation"]["software"])
        session.capture("controller-single-pointer")
        check(True, "controller takes the same pointer and hides native icon")
        adb.shell("input", "keyevent", "KEYCODE_BACK")
        wait(lambda o: o["dialog"] and not o["state"]["pointer_presentation"]["software"])
        session.capture("native-menu")
        check(True, "native menu holds game and erases canvas cursor")
        adb.shell("input", "keyevent", "KEYCODE_BACK")
        wait(lambda o: not o["dialog"])
        role("mouse-reclaims-pointer", 410, 450, 0, 1000, False)
        mouse(150, 90, tap=True)
        wait(lambda o: o["document_kind"] == 1
             and not o["state"]["pointer_presentation"]["software"])
        session.capture("native-document-picker")
        check(True, "real document picker holds game without canvas cursor")
        session.command("document cancel")
        role("after-modal", 420, 450, 0, 1000, False)
        android.verify_apk(game, validation=True)
        report["status"] = "passed"
    except Exception as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        report["observations"] = session.observations
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    report = run(args.output, args.timeout)
    print(json.dumps({"status": report["status"], "checks": len(report["checks"])}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

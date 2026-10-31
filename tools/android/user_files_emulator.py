#!/usr/bin/env python3
"""Exercise authored FileIO/PrintOMatic buttons through real Android system UI.

Build user_files_fixture.py first and start the isolated development emulator.
This tool installs only its dedicated .validation app and writes only its own
Download folder. It never connects physical devices or injects engine state.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import shutil
import subprocess
import time
import xml.etree.ElementTree as ET
from pathlib import Path

from user_files_fixture import CAR_NAME, CAR_TEXT, fixture_game

from director64 import android
from director64.android_validation import Adb, Session
from director64.project import repository
from director64.visual import read_png


def run(output: Path, timeout: float, snapshot: Path | None = None) -> dict:
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("timeout must be finite and positive")
    game = fixture_game(repository())
    output = output.resolve()
    if not output.is_relative_to(game.root / "build"):
        raise ValueError("evidence must remain under ignored build/")
    if not shutil.which("pdfinfo") or not shutil.which("pdftoppm"):
        raise ValueError("PDF verification requires pdfinfo and pdftoppm")
    apk = android.apk_path(game, validation=True) if snapshot is None else snapshot.resolve()
    receipt_path = android.apk_receipt_path(game, validation=True) if snapshot is None else (
        apk.with_suffix(apk.suffix + ".json"))
    if snapshot is None:
        android.verify_apk(game, validation=True)
    elif not apk.is_relative_to(game.root / "build"):
        raise ValueError("retained snapshot APK must be under ignored build/")
    receipt = json.loads(receipt_path.read_text())
    if (receipt.get("source_sha256") != game.source["sha256"] or
        not receipt.get("validation") or
        receipt.get("apk_sha256") != hashlib.sha256(apk.read_bytes()).hexdigest()):
        raise ValueError("retained fixture APK identity differs from its validation receipt")
    output.mkdir(parents=True, exist_ok=False)
    adb = Adb(game.root, emulator=True)
    adb.deadline = time.monotonic() + timeout
    session = Session(game, adb, output, build_id=receipt["inputs"])
    report = {"kind": "source-free-android-document-qualification", "status": "running",
              "original_projector_evidence": False, "physical_device_evidence": False,
              "build_verification": ("current-source" if snapshot is None
                                     else "retained-apk-snapshot"),
              "source_sha256": game.source["sha256"], "apk": receipt, "checks": []}

    def check(condition, name):
        if not condition:
            raise AssertionError(name)
        report["checks"].append(name)

    def capture(label):
        value = session.capture(label)
        (output / (label + ".json")).write_text(json.dumps(value, indent=2) + "\n")
        return value

    def nodes():
        adb.shell("uiautomator", "dump", "/sdcard/director64-documents-ui.xml")
        raw = adb.run("exec-out", "cat", "/sdcard/director64-documents-ui.xml").stdout
        (output / f"ui-{time.monotonic_ns()}.xml").write_text(raw)
        return ET.fromstring(raw)

    def tap(label, *, description=False, scroll=False):
        for attempt in range(8 if scroll else 3):
            tree = nodes()
            key = "content-desc" if description else "text"
            matches = [node for node in tree.iter("node")
                       if node.get(key) == label and node.get("enabled") == "true"]
            if not matches and not description:
                matches = [node for node in tree.iter("node")
                           if node.get("content-desc", "").startswith(label + ",")]
            if not matches and not description:
                parents = {child: parent for parent in tree.iter() for child in parent}
                previews = [node for node in tree.iter("node")
                            if node.get("content-desc") == "Preview the file " + label]
                for node in previews:
                    while node.get("clickable") != "true" or node in previews:
                        node = parents[node]
                    matches.append(node)
            if matches:
                box = list(map(int, re.findall(r"\d+", matches[-1].get("bounds", ""))))
                if len(box) != 4:
                    raise ValueError(f"Android UI node has no bounds: {label}")
                x, y = (box[0] + box[2]) // 2, (box[1] + box[3]) // 2
                session.record({"android_ui_label": label, "tap": [x, y]})
                adb.shell("input", "tap", str(x), str(y))
                time.sleep(0.3)
                return
            if scroll and attempt:
                session.record({"android_ui_scroll": [800, 650, 800, 330]})
                adb.shell("input", "swipe", "800", "650", "800", "330", "350")
            time.sleep(0.2)
        raise AssertionError(f"Android system UI did not expose {label!r}")

    def button(y, label, kind):
        session.pointer(4, 150, y, True)
        session.pointer(12, 150, y, False)
        observation = session.observe()
        check(observation["document_kind"] == kind, label + " opens real system modal")
        capture(label + "-dialog")

    def finished(action):
        deadline = min(adb.deadline, time.monotonic() + 20)
        while time.monotonic() < deadline:
            state = session.observe()
            if state["document_kind"] == 0:
                check(state["state"]["globals"]["lastaction"] == action, action)
                return state
            time.sleep(0.2)
        raise AssertionError("Android modal did not finish")

    def pull(remote, local):
        adb.run("pull", remote, "/workdir/" + local.relative_to(game.root).as_posix())

    try:
        report["device"] = adb.identity()
        check(report["device"]["kind"] == "android-emulator", "isolated emulator provenance")
        shutil.copyfile(apk, output / "tested-fixture.apk")
        shutil.copyfile(receipt_path, output / "tested-fixture.apk.json")
        adb.run("install", "-r", "-t", "/workdir/" + apk.relative_to(game.root).as_posix())
        folder_name = "director64-documents-" + hashlib.sha256(str(output).encode()).hexdigest()[:8]
        folder = "/sdcard/Download/" + folder_name
        adb.shell("mkdir", "-p", folder)
        for name in (CAR_NAME, "fixture-image.png"):
            source = game.work / "inputs" / name
            adb.run("push", "/workdir/" + source.relative_to(game.root).as_posix(),
                    folder + "/" + name)
            adb.shell("am", "broadcast", "-a", "android.intent.action.MEDIA_SCANNER_SCAN_FILE",
                      "-d", "file://" + folder + "/" + name)
        session.launch(reboot=True)
        capture("boot")
        width, _, rgba = read_png(output / "boot.stage.png")
        check(rgba[(200 * width + 400) * 4:][:4] == bytes((120, 120, 120, 255)),
              "source fixture starts with gray image")
        button(90, "open-car", 1)
        tap("Show roots", description=True)
        tap("Downloads")
        tap(folder_name, scroll=True)
        tap(CAR_NAME)
        finished("Opened: " + CAR_TEXT)
        capture("car-opened")
        button(190, "save-car", 2)
        tap(CAR_NAME)
        session.record({"android_ui_text": "exported.car"})
        adb.shell("input", "keycombination", "113", "29")
        adb.shell("input", "text", "exported.car")
        tap("SAVE")
        finished("Saved fixture car")
        pull(folder + "/exported.car", output / "exported.car")
        expected_car = CAR_TEXT.encode("cp1252")
        check((output / "exported.car").read_bytes() == expected_car,
              "selected URI contains exact authored CAR bytes")
        capture("car-saved")
        button(390, "open-image", 1)
        tap("fixture-image.png")
        finished("Image imported")
        capture("image-imported")
        width, _, rgba = read_png(output / "image-imported.stage.png")
        for (x, y), expected in zip(((230, 30), (430, 30), (230, 180), (430, 180)),
                                    ((255, 0, 0, 255), (0, 255, 0, 255),
                                     (0, 0, 255, 255), (255, 255, 255, 255)), strict=True):
            check(rgba[(y * width + x) * 4:][:4] == bytes(expected),
                  f"imported image pixel {x},{y}")
        button(290, "print", 4)
        tree = nodes()
        if any(node.get("text") == "Select a printer" for node in tree.iter("node")):
            tap("Select a printer")
            tap("Save as PDF")
        tap("Save to PDF", description=True)
        tap("SAVE")
        finished("Print dialog finished")
        pull(folder + "/game-picture.pdf", output / "game-picture.pdf")
        pdf = subprocess.run(["pdfinfo", str(output / "game-picture.pdf")],
                             text=True, capture_output=True, check=True).stdout
        (output / "pdfinfo.txt").write_text(pdf)
        check(bool(re.search(r"Pages:\s+1\b", pdf)) and "A4" in pdf,
              "real PrintManager output is one A4 page")
        subprocess.run(["pdftoppm", "-scale-to", "1000", "-png", "-singlefile",
                        str(output / "game-picture.pdf"), str(output / "game-picture")], check=True)
        _, _, pdf_rgba = read_png(output / "game-picture.png")
        colors = [tuple(pdf_rgba[i:i + 3]) for i in range(0, len(pdf_rgba), 4)]
        check(sum(color == (0, 0, 0) for color in colors) > 100,
              "PDF contains authored text pixels")
        check(sum(color == (255, 0, 0) for color in colors) > 100,
              "PDF contains authored stage image pixels")
        capture("printed")
        button(90, "cancel-open", 1)
        first = session.observe()["state"]["tick"]
        time.sleep(0.7)
        check(session.observe()["state"]["tick"] == first,
              "engine time pauses while picker is open")
        session.command("document cancel")
        finished("Open cancelled")
        capture("cancelled")
        button(90, "destroy-open", 1)
        adb.shell("am", "force-stop", session.package)
        session.launch(reboot=True)
        state = session.observe()
        check(state["document_kind"] == 0 and state["state"]["globals"]["lastaction"] == "Ready",
              "process restart abandons stale system modal capability")
        capture("restarted")
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["failure"] = str(error)
        raise
    finally:
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        try:
            logs = adb.run("logcat", "-d", "-v", "threadtime", "director64:I",
                           "AndroidRuntime:E", "*:S", check=False, timeout=3).stdout
            (output / "logcat.txt").write_text(logs)
        except Exception as error:
            (output / "logcat-error.txt").write_text(str(error) + "\n")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=240)
    parser.add_argument("--snapshot", type=Path,
                        help="Replay a retained APK+receipt with explicit snapshot provenance")
    args = parser.parse_args()
    run(args.output, args.timeout, args.snapshot)
    print(args.output / "report.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

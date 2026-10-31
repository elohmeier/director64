#!/usr/bin/env python3
"""Build a source-free .validation APK whose authored buttons exercise Android I/O.

The fixture uses the production compiler, loader, interpreter and Android host.
No gameplay mutation RPC or retail media is used. Generated packages, APKs and
observations stay in ignored build/ and dist/ directories.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path
from unittest.mock import patch

from director64 import android, convert
from director64.lingo import parse_movie
from director64.project import GameSpec, converter, repository
from director64.visual import write_png

CAR_NAME = "Äuto Straße €.car"
CAR_TEXT = '[#name:"Müller Straße €",#parts:[1,2,3]]'

LINGO = '''-- cast: Internal; member: 10; type: MovieScript; name: fixture
global lastAction
on startMovie
  lastAction = "Ready"
end
on exitFrame
  go(the frame)
end

-- cast: Internal; member: 20; type: BehaviorScript; name: OpenText
on mouseUp
  global lastAction
  f = new(xtra("FileIO"))
  f.setFilterMask("Cars (*.car),*.car")
  p = f.displayOpen()
  lastAction = "Open cancelled"
  if stringp(p) then
    f.openFile(p, 1)
    lastAction = "Opened: " & f.readFile()
    f.closeFile()
  end if
  f.dispose()
  member("Status").text = lastAction
end

-- cast: Internal; member: 21; type: BehaviorScript; name: SaveText
on mouseUp
  global lastAction
  f = new(xtra("FileIO"))
  p = f.displaySave("Save fixture car", "fixture.car")
  lastAction = "Save cancelled"
  if stringp(p) then
    f.createFile(p)
    f.openFile(p, 2)
    f.writeString("[#name:" & QUOTE & "TEST CAR" & QUOTE & ",#parts:[1,2,3]]")
    f.closeFile()
    lastAction = "Saved fixture car"
  end if
  f.dispose()
  member("Status").text = lastAction
end

-- cast: Internal; member: 22; type: BehaviorScript; name: PrintPicture
on mouseUp
  global lastAction
  doc = new(xtra("PrintOMatic"))
  doc.setMargins(rect(20, 20, 20, 20))
  doc.newFrame(rect(0, 0, 550, 80))
  doc.append(member("PrintText"))
  doc.drawStagePicture(rect(300, 100, 700, 400), rect(0, 100, 400, 400))
  doc.print()
  lastAction = "Print dialog finished"
  member("Status").text = lastAction
end

-- cast: Internal; member: 23; type: BehaviorScript; name: OpenImage
on mouseUp
  global lastAction
  f = new(xtra("FileIO"))
  f.setFilterMask("Images (*.png),*.png")
  p = f.displayOpen()
  lastAction = "Image cancelled"
  if stringp(p) then
    importFileInto(member("Picture"), p)
    lastAction = "Image imported"
  end if
  f.dispose()
  member("Status").text = lastAction
end
'''
LINGO = LINGO.replace("fixture.car", CAR_NAME).replace("TEST CAR", "Müller Straße €")


def scene() -> dict:
    members = [
        {"cast": 1, "number": n, "name": title, "type": 3,
         "width": 220, "height": 60, "text": title}
        for n, title in enumerate(("OPEN CAR", "SAVE CAR", "PRINT DOCUMENT", "OPEN IMAGE"), 1)
    ]
    members += [
        {"cast": 1, "number": 5, "name": "Picture", "type": 1,
         "width": 64, "height": 64, "depth": 32,
         "asset": "fixture.fdi", "regX": 0, "regY": 0},
        {"cast": 1, "number": 6, "name": "Status", "type": 3,
         "width": 720, "height": 70, "text": "Ready"},
        {"cast": 1, "number": 7, "name": "PrintText", "type": 3,
         "width": 550, "height": 80,
         "text": "DIRECTOR64 AUTHORED PRINT FIXTURE\rText and stage image"},
        *({"cast": 1, "number": n, "name": "fixture-script-" + str(n), "type": 11}
          for n in (10, 20, 21, 22, 23)),
    ]
    channels = []
    for sprite, member, x, y, width, height in (
        (1, 1, 40, 60, 220, 60), (2, 2, 40, 160, 220, 60),
        (3, 3, 40, 260, 220, 60), (4, 4, 40, 360, 220, 60),
        (5, 5, 350, 120, 320, 240), (6, 6, 40, 500, 720, 70),
    ):
        raw = bytearray(48)
        raw[0], raw[1], raw[2], raw[3] = 16, 128, 0, 255
        struct.pack_into(">HH", raw, 4, 1, member)
        struct.pack_into(">hhhh", raw, 12, y, x, height, width)
        channels.append({"channel": sprite + 5, "bytes": list(raw), "changed": [1] * 48,
                         "behaviors": [{"cast": 1, "member": 19 + sprite}] if sprite <= 4 else []})
    return {"problems": [], "extendedD6": True, "fullEngine": True, "fonts": [], "movies": [{
        "id": 1, "name": "MAIN.DXR", "directorVersion": 850, "tempo": 30,
        "stageColor": 0xE0E0E0, "stageColorRGB": 0xE0E0E0,
        "palette": [0x000000, 0xFFFFFF] + [0] * 253 + [0xFFFFFF],
        "casts": [{"number": 1, "name": "Internal", "file": "MAIN.DXR"}],
        "members": members,
        "score": {"version": 13, "recordSize": 48,
                  "frames": [{"number": 1, "channels": channels}], "labels": []},
    }]}


class FixtureGame(GameSpec):
    @property
    def directory(self) -> Path:
        return self.work / "fixture"


def fixture_game(root: Path) -> FixtureGame:
    model = scene()
    source = json.dumps({"lingo": LINGO, "model": model}, sort_keys=True).encode()
    digest = hashlib.sha256(source).hexdigest()
    identity = {"id": "sha256-" + digest[:12], "file": "source-free.json", "sha256": digest,
                "bytes": len(source), "kind": "synthetic"}
    return FixtureGame(root, "android-documents-fixture", {
        "schema_version": 1, "slug": "android-documents-fixture", "title": "Document test",
        "status": "experimental", "sources": [identity], "default_source": identity["id"],
        "port": {"director_version": 850, "extended_d6": True, "full_engine": True,
                 "media_root": ".", "entry_movie": "MAIN"}, "player": {},
    }, identity)


def prepare(root: Path) -> FixtureGame:
    game = fixture_game(root)
    model = scene()
    source = json.dumps({"lingo": LINGO, "model": model}, sort_keys=True).encode()
    digest, identity = game.source["sha256"], game.source
    game.directory.mkdir(parents=True, exist_ok=True)
    (game.directory / "host").mkdir(exist_ok=True)
    (game.directory / "host/source-policy.json").write_text(
        '{"native_stage":true,"directories":["."]}\n')
    (game.directory / identity["file"]).write_bytes(source)
    (game.directory / "MAIN.DXR.lingo").write_text(LINGO)
    program = {"files": [{"name": "MAIN.DXR.lingo"}], "handlers": parse_movie(LINGO, "MAIN.DXR")}
    for name, value in (("model.json", model), ("program.json", program),
                        ("meta.json", {"director_version": 850, "extended_d6": True,
                                       "source_sha256": digest, "renderer_target": "android"})):
        (game.directory / name).write_text(json.dumps(value))
    packed = game.work / "converted"
    packed.mkdir(exist_ok=True)
    subprocess.run([*converter(root), "package", game.directory / "program.json",
                    game.directory / "model.json", packed / "game.d64p",
                    "--bytecode", root / "runtime/lingo/lingo_bytecode.h",
                    "--meta", game.directory / "meta.json"], check=True)
    header = bytearray(32)
    header[:4] = b"FDI2"
    pixels = bytes((120, 120, 120, 255)) * (64 * 64)
    struct.pack_into(">IHH", header, 4, 32 + len(pixels), 64, 64)
    for kind in convert.PACKS:
        entries = ({"fixture.fdi": convert.deflated(bytes(header) + pixels)}
                   if kind == "images" else {})
        convert.write_pack(packed, kind, entries, {})
    (packed / "manifest.json").write_text(json.dumps({
        "schema_version": 1, "kind": "source-free-authored-fixture", "source_free": True,
        "source_sha256": digest, "director_version": 850,
        "original_projector_evidence": False,
        "outputs": {name: hashlib.sha256((packed / name).read_bytes()).hexdigest()
                    for name in convert.GAME_FILES},
    }, indent=2) + "\n")
    input_dir = game.work / "inputs"
    input_dir.mkdir(exist_ok=True)
    (input_dir / CAR_NAME).write_bytes(CAR_TEXT.encode("cp1252"))
    image = bytes(c for y in range(16) for x in range(16)
                  for c in ((255, 0, 0, 255) if x < 8 and y < 8 else
                            (0, 255, 0, 255) if y < 8 else
                            (0, 0, 255, 255) if x < 8 else (255, 255, 255, 255)))
    write_png(input_dir / "fixture-image.png", 16, 16, image)
    (game.work / "fixture.json").write_text(json.dumps({
        "kind": "source-free-android-document-fixture", "director_version": 850,
        "source_sha256": digest, "input": "normal Android pointer events on authored buttons",
        "expected": {"open": "Opened: " + CAR_TEXT,
                     "save": "exact Windows-1252 CAR bytes at the selected URI",
                     "image": "four-color image replaces the gray rectangle",
                     "print": "Android print preview contains authored fixture text and image"},
        "environment": "isolated Android14 emulator; no native Windows or physical TV claim",
    }, indent=2) + "\n")
    return game


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    game = prepare(repository())
    # The authored synthetic bundle replaces retail-media conversion only in
    # this fixture process. APK staging, compilation and its receipt are normal.
    with patch.object(android, "convert", return_value=game.work / "converted"):
        apk = android.build(game, args.jobs, ["x86_64"], False, validation=True)
    android.verify_apk(game, validation=True)
    print(apk)
    print(game.work)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

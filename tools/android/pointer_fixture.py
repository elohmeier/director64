#!/usr/bin/env python3
"""Source-free Android pointer roles, authored mask fallback and real modals.

Reuses the authored document fixture's compiler/package builder, with distinct
source identity and validation application. This never edits a retail game.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from dataclasses import replace
from unittest.mock import patch

import user_files_fixture as base

from director64 import android, convert
from director64.project import repository


def cursor_bytes() -> bytes:
    pixels = bytes(0 if x in (0, 11) or y in (0, 11) else 3 if x == y else 2
                   for y in range(12) for x in range(12))
    return b"D64C" + struct.pack(">HHHHHH", 12, 12, 0, 0, 1, 0) + pixels


_document_game = base.fixture_game
_source = base.LINGO
parts = _source.split("-- cast:")
for i, kind in enumerate((1, 2, 4, 200), 2):
    parts[i] += f"\non mouseEnter\n  cursor({kind})\nend\non mouseLeave\n  cursor(0)\nend\n"
base.LINGO = ("-- cast:".join(parts) + "\n-- authored-mask-sha256 "
              + hashlib.sha256(cursor_bytes()).hexdigest() + "\n")


def fixture_game(root):
    game = _document_game(root)
    return replace(game, slug="android-pointer-fixture",
                   data={**game.data, "slug": "android-pointer-fixture", "title": "Pointer test"})


def prepare(root):
    with patch.object(base, "fixture_game", fixture_game):
        game = base.prepare(root)
    policy = {"native_stage": True, "directories": ["."], "executable_cursors": {
        "kind": "source-free-authored-mask", "entries": [{"id": 2}]}}
    (game.directory / "host/source-policy.json").write_text(json.dumps(policy) + "\n")
    packed = game.work / "converted"
    with (packed / "images.json").open() as handle:
        index = json.load(handle)
    # The ordinary image remains byte-identical; the additional D64C is authored
    # by this tool and exercises inversion against actual game pixels.
    image_blob = (packed / "images.bin").read_bytes()
    entries = {name: image_blob[value[0]:value[0] + value[1]]
               for name, value in index.items()}
    entries["d64-cursor-2.dcur"] = convert.deflated(cursor_bytes())
    convert.write_pack(packed, "images", entries, {})
    manifest = json.loads((packed / "manifest.json").read_text())
    manifest["outputs"] = {name: hashlib.sha256((packed / name).read_bytes()).hexdigest()
                           for name in convert.GAME_FILES}
    (packed / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return game


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    game = prepare(repository())
    with patch.object(android, "convert", return_value=game.work / "converted"):
        apk = android.build(game, args.jobs, ["x86_64"], False, validation=True)
    android.verify_apk(game, validation=True)
    print(apk)
    print(game.work)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

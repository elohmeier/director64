"""Preserve source text metrics with an explicit, pinned system-font substitute."""

import hashlib
import json
import shutil

from director64.project import selected_game


def main():
    game = selected_game()
    output = game.work / "director"
    path = output / "model.json"
    model = json.loads(path.read_text())
    source = game.root / "third_party/libdragon/examples/fontgallery/assets/droid-sans.ttf"
    asset = "fonts/christmas-system.ttf"
    shutil.copyfile(source, output / asset)
    sizes, source_fonts = set(), set()
    for movie in model["movies"]:
        for member in movie["members"]:
            styles = member.get("sourceTextStyles")
            if not styles:
                continue
            style = styles[0]
            source_fonts.add(style["sourceFontId"])
            sizes.add(style["size"])
            member["textStyle"] = {
                "fontName": f"Director system font {style['sourceFontId']}",
                "fontId": 1,
                "size": style["size"],
                "align": {-1: 2, 0: 0, 1: 1}.get(member.get("textAlign", 0), 0),
                "ascent": style["ascent"],
                "descent": max(0, style["lineHeight"] - style["ascent"]),
                "leading": 0,
                "lineHeight": max(1, style["lineHeight"]),
                "color": style["color"],
            }
            member["textPresentation"] = "explicit-system-font-substitute"
    model["fonts"] = [
        {
            "number": 1,
            "asset": asset,
            "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "kind": "system-font-substitute",
            "sourceFontIds": sorted(source_fonts),
            "targetFont": "Droid Sans",
            "license": "Apache-2.0",
            "provenance": source.relative_to(game.root).as_posix(),
            "variants": [{"size": s, "asset": f"fonts/f1-{s}.font64"} for s in sorted(sizes)],
        }
    ]
    model["approximations"].append(
        {
            "kind": "system-font-substitute",
            "description": "Source STXT metrics and Mac Roman text use regular Droid Sans; "
            "mixed fonts and faces use the first source run.",
            "implemented": True,
        }
    )
    path.write_text(json.dumps(model) + "\n")

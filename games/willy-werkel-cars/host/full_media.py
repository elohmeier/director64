"""Preserve source text metrics with an explicit, pinned system-font substitute."""

import hashlib
import json
import shutil

from director64.project import selected_game

from .text_metrics import measure


def main():
    game = selected_game()
    output = game.work / "director"
    path = output / "model.json"
    model = json.loads(path.read_text())
    source = game.root / "third_party/libdragon/examples/fontgallery/assets/droid-sans.ttf"
    asset = "fonts/willy-system.ttf"
    shutil.copyfile(source, output / asset)
    sizes, source_fonts = set(), set()
    # These fields participate in controller text entry and source width checks.
    controller_metrics = measure(game, source, 24)
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
            if member["name"] in {"SavedCarName", "EnterField"} and style["size"] == 24:
                member["textStyle"]["controllerMetrics"] = controller_metrics
            member["textPresentation"] = "explicit-system-font-substitute"
    template = next(
        m
        for f in model["movies"]
        if f["name"] == "CDDATA.CXT"
        for m in f["members"]
        if m["number"] == 1
    )
    expected = "ac7f41cbf12bfe707ef1d0f284ebfd363e1a1272abe9a5ebe2ac69788287bb36"
    if hashlib.sha256(template["text"].encode()).hexdigest() != expected:
        raise ValueError("new-player template source changed")
    template["text"] = template["text"][:-1]
    template["compatibility"] = {
        "id": "new-player-template-unmatched-bracket",
        "source_text_sha256": expected,
        "change": "remove the final unmatched closing bracket",
        "original_projector_verified": False,
    }
    for number, expected in (
        (170, "4926eca66bc61a7a708afbf2bcc2780c553da3005e6af0dcb8e485fb69daeaf1"),
        (173, "2b3f97edade776b90c18901c4a1fdd4bf583bc5723519a00b371eed46f1deb1c"),
    ):
        member = next(
            m
            for f in model["movies"]
            if f["name"] == "00.CXT"
            for m in f["members"]
            if m["number"] == number
        )
        if hashlib.sha256(member["text"].encode()).hexdigest() != expected:
            raise ValueError("animation chart source changed")
        member["text"] = member["text"][:-3]
        member["compatibility"] = {
            "id": "animation-chart-trailing-digits",
            "source_text_sha256": expected,
            "change": "remove 333 after the closed property list",
            "original_projector_verified": False,
        }
    model["approximations"].extend(
        {
            "kind": "source-data-correction",
            "movie": f["name"],
            "member": m["number"],
            **m["compatibility"],
        }
        for f in model["movies"]
        for m in f["members"]
        if "compatibility" in m
    )
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

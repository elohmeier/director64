"""Source-driven cursor navigation and dragging, with independent mask checks."""

import hashlib
import json
import struct

from director64.project import selected_game

from .full_journey import Replay
from .full_regressions import member


def bitmap_expectation(root, image, mask):
    def pixels(m):
        data = (root / "director/images" / m["asset"]).read_bytes()
        if data[:4] != b"FDI1" or len(data) != 32 + 2 * m["width"] * m["height"]:
            raise ValueError("cursor fixture is not the recovered monochrome FDI1")
        return struct.unpack(f">{m['width'] * m['height']}H", data[32:])

    ink, coverage = pixels(image), pixels(mask)
    rows = bytearray()
    opaque_count, black_count = 0, 0
    for y in range(16):
        black, opaque = 0, 0
        for x in range(16):
            if x >= min(image["width"], mask["width"]) or y >= min(image["height"], mask["height"]):
                continue
            color = ink[y * image["width"] + x] & 0xFFFE
            alpha = coverage[y * mask["width"] + x] & 0xFFFE
            if color not in (0, 0xFFFE) or alpha not in (0, 0xFFFE):
                raise ValueError("source cursor is no longer monochrome")
            if alpha == 0:
                opaque |= 0x8000 >> x
                opaque_count += 1
                if color == 0:
                    black |= 0x8000 >> x
                    black_count += 1
        rows += struct.pack(">HH", black, opaque)
    hotspot = [image["regX"], image["regY"]]
    if any(not 0 <= n < 16 for n in hotspot):
        hotspot = [8, 8]
    value = 2166136261
    for byte in rows + bytes(hotspot):
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return {"hotspot": hotspot, "hash": value, "opaque": opaque_count, "black": black_count}


def run(executable, output):
    game = selected_game()
    model = json.loads((game.work / "director/model.json").read_text())
    shared = next(m for m in model["movies"] if m["name"] == "CURSOR.CXT")
    members = {m["name"].casefold(): m for m in shared["members"]}
    expected = {}
    for name in ("links", "rechts", "Vor", "Rueck", "Hotspot", "Hand", "Greif"):
        image, mask = members[name.casefold()], members[name.casefold() + "m"]
        expected[name] = {
            "resource": 0,
            "image": member(shared, name),
            "mask": member(shared, name + "M"),
            **bitmap_expectation(game.work, image, mask),
        }
    # Recovered mask extents independently distinguish the three hand/hotspot
    # silhouettes from the generic matte alpha stripped by sprite conversion.
    assert [
        (expected[n]["opaque"], expected[n]["black"]) for n in ("Hotspot", "Hand", "Greif")
    ] == [(146, 53), (179, 56), (126, 33)]
    replay = Replay(executable, output / "CURSORS")
    checked = []

    def check(name):
        cursor = replay.state["cursor"]
        wanted = expected[name]
        if any(cursor[k] != wanted[k] for k in cursor):
            raise ValueError(f"{name} cursor mismatch: {cursor}, expected {wanted}")
        replay.state["checks"] = {
            "cursor": [cursor["resource"], cursor["image"], cursor["mask"], *cursor["hotspot"]]
        }
        checked.append(name)

    try:
        replay.step(4000)
        if replay.state["movie"] != "PANO.DXR":
            raise ValueError("cursor journey did not reach panorama")
        for name, point in (
            ("links", (30, 240)),
            ("rechts", (610, 240)),
            ("Vor", (309, 270)),
            ("Hotspot", (205, 400)),
        ):
            replay.step(60, *point)
            check(name)
        replay.click(400, 350)  # Table enters the panorama's 1Z view.
        replay.step(60, 320, 430)
        check("Rueck")
        replay.click(320, 430)
        replay.click(615, 460)
        if replay.state["movie"] != "DIALOG.DXR":
            raise ValueError("cursor journey did not open the volume dialog")
        # Use the mower body: its source registration point keeps it under the
        # mouse while dragging, unlike the handle's offset artwork.
        replay.step(60, 375, 300)
        check("Hand")
        replay.step(120, 375, 300, 1)
        check("Greif")
        replay.step(120, 297, 300, 1)
        check("Greif")
        replay.step(240, 297, 300)
        check("Hand")
        replay.step(60, 40, 40)
        check("links")  # Cursor belongs to the stage outside the dialog.
        replay.click(40, 40)
        replay.step(60, 610, 240)
        check("rechts")
        if replay.state["movie"] != "PANO.DXR":
            raise ValueError("cursor journey did not restore panorama")
    finally:
        replay.close()
    report = {
        "status": "passing",
        "platform": "host-native",
        "scenario": "source-cursors",
        "source_sha256": game.source["sha256"],
        "cursors": expected,
        "checks": checked,
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "replay_sha256": {
            "CURSORS": hashlib.sha256((output / "CURSORS/replay.json").read_bytes()).hexdigest()
        },
        "hardware_qualified": False,
        "original_projector_compared": False,
    }
    return report

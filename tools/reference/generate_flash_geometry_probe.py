#!/usr/bin/env python3
"""Generate source-free Flash8 display-geometry probes for Adobe's debugger.

Outputs belong in ignored build/. The player trace records transformed bounds,
scale signs, rotation, and size-assignment order; no game media is required.
"""
import argparse
from pathlib import Path
import struct
from generate_filter_probe import rect, tag


def push(value):
    data = b"\0" + value.encode("ascii") + b"\0" if isinstance(value, str) else b"\7" + struct.pack("<i", value)
    return b"\x96" + struct.pack("<H", len(data)) + data


def variable(name):
    return push(name) + b"\x1c"


def call(receiver, method, arguments=()):
    return b"".join(push(value) for value in reversed(arguments)) + push(len(arguments)) + variable(receiver) + push(method) + b"\x52\x17"


def assign(name, value):
    return variable("clip") + push(name) + push(value) + b"\x4f"


def trace(label):
    return b"".join(push(label + " " + key + "=") + variable("clip") + push(key) + b"\x4e\x47\x26"
                    for key in ("_width", "_height", "_xscale", "_yscale", "_rotation", "_x", "_y"))


def movie(mirrored):
    code = call("_root", "createEmptyMovieClip", ("clip", 1)) + call("clip", "beginFill", (0x00ff00,))
    for method, point in (("moveTo", (0, 0)), ("lineTo", (16, 0)), ("lineTo", (16, 100)),
                          ("lineTo", (0, 100)), ("lineTo", (0, 0)), ("endFill", ())):
        code += call("clip", method, point)
    if mirrored:
        code += assign("_rotation", 90) + assign("_xscale", 80) + assign("_yscale", -80) + trace("initial")
        code += assign("_width", 40) + trace("width40") + assign("_height", 20) + trace("height20")
        code += assign("_xscale", -100) + trace("xnegative") + assign("_xscale", -100) + trace("xnegativeagain")
    else:
        code += trace("plain") + assign("_width", 40) + trace("plainwidth40") + assign("_height", 20) + trace("plainheight20")
        code += assign("_rotation", 90) + assign("_xscale", 80) + assign("_yscale", 80) + trace("positive90")
        code += assign("_width", 40) + trace("p90width40") + assign("_width", 20) + trace("p90width20")
        code += assign("_height", 20) + trace("p90height20")
        code += assign("_rotation", 45) + assign("_xscale", 80) + assign("_yscale", 80) + trace("positive45")
        code += assign("_width", 40) + trace("p45width40") + assign("_height", 20) + trace("p45height20")
    code += push("GEOMETRY_DONE") + b"\x26\x07\0"
    body = rect(0, 0, 640, 640) + bytes((0, 12)) + struct.pack("<H", 1) + tag(12, code) + tag(1) + tag(0)
    return b"FWS\x08" + struct.pack("<I", len(body) + 8) + body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name, mirrored in (("mirrored", True), ("size-order", False)):
        (args.output / f"{name}.swf").write_bytes(movie(mirrored))


if __name__ == "__main__":
    main()

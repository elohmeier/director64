#!/usr/bin/env python3
"""Write synthetic Flash8 bevel fixtures and an original-Director pixel probe.

No retail media or player code is embedded. Outputs belong in ignored build/.
The probe requires a Flash8-capable original Xtra; log its binary version/hash
and loaded Director runtime before treating output as original-player evidence.
"""
from __future__ import annotations
import argparse
import json
import math
from pathlib import Path
import struct

class Bits:
    def __init__(self): self.bits = []
    def put(self, value, width):
        self.bits.extend((value >> bit) & 1 for bit in reversed(range(width)))
        return self
    def bytes(self):
        out = bytearray((len(self.bits) + 7) // 8)
        for i, value in enumerate(self.bits): out[i // 8] |= value << (7 - i % 8)
        return bytes(out)

def rect(left, top, right, bottom):
    width = max(abs(v).bit_length() + 1 for v in (left, top, right, bottom))
    bits = Bits().put(width, 5)
    for v in (left, right, top, bottom): bits.put(v, width)
    return bits.bytes()

def tag(kind, data=b""):
    return struct.pack("<H", (kind << 6) | min(len(data), 63)) + (
        struct.pack("<I", len(data)) if len(data) >= 63 else b"") + data

def capture_actions():
    def push(value):
        if isinstance(value, str): data = b"\0" + value.encode("ascii") + b"\0"
        elif isinstance(value, bool): data = b"\x05" + bytes([value])
        else: data = b"\x07" + struct.pack("<i", value)
        return b"\x96" + struct.pack("<H", len(data)) + data
    def variable(name): return push(name) + b"\x1c"
    def member(name): return push(name) + b"\x4e"
    def call(obj, name, args):
        return b"".join(args) + push(len(args)) + variable(obj) + push(name) + b"\x52"
    # BitmapData.draw(root) includes the filtered child; drawing the child
    # itself would omit the filter attached to that drawing root.
    out = push("bmp") + push(0) + push(True) + push(32) + push(32) + push(4)
    out += variable("flash") + member("display") + push("BitmapData") + b"\x53\x1d"
    out += call("bmp", "draw", [variable("_root")]) + b"\x17"
    for y in range(32):
        for x in range(32):
            out += push(f"PIX {x} {y} ") + call("bmp", "getPixel32", [push(y), push(x)]) + b"\x47\x26"
    out += push("VERSION ") + variable("$version") + b"\x47\x26"
    return out + push("FILTER_PROBE_DONE") + b"\x26\x07\0"

def movie(case, trace=False):
    bounds = rect(0, 0, 640, 640)
    records = Bits().put(1, 4).put(0, 4)
    records.put(0, 1).put(5, 5).put(10, 5).put(160, 10).put(160, 10).put(1, 1)
    for dx, dy in ((320, 0), (0, 320), (-320, 0), (0, -320)):
        records.put(1, 1).put(1, 1).put(8, 4).put(1, 1).put(dx, 10).put(dy, 10)
    records.put(0, 6)
    shape = struct.pack("<H", 1) + bounds + bytes([1, 0, 0, 255, 0, 255, 0]) + records.bytes()
    matrix = Bits().put(0, 1).put(0, 1).put(1, 5).put(0, 1).put(0, 1).bytes()
    effect = bytes([1, 7, 3, 255, 0, 0, 255, 0, 0, 0, 0, 0, 0, 255, 255, 0, 128, 255])
    effect += b"".join(struct.pack("<i", round(v * 65536)) for v in (
        case["blur"], case["blur"], math.radians(case["angle"]), case["distance"]))
    effect += struct.pack("<HB", round(case["strength"] * 256), case["flags"])
    placement = bytes([6, 1]) + struct.pack("<HH", 1, 1) + matrix + effect
    body = bounds + bytes([0, 12]) + struct.pack("<H", 1) + tag(32, shape) + tag(70, placement)
    if trace: body += tag(12, capture_actions())
    body += tag(1) + tag(0)
    return b"FWS\x08" + struct.pack("<I", len(body) + 8) + body

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--standalone-trace", action="store_true",
                        help="Use Adobe's original standalone debugger trace log to capture ARGB pixels")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    cases = [dict(blur=blur, angle=angle, distance=distance, strength=strength, flags=flags)
        for blur, angle, distance, strength, flags in (
            (0, 0, 1, 1, 0xa1), (0, 90, 1, 1, 0xa1), (3, 0, 1, 1, 0xa1),
            (5, 220, 5, 1, 0xa1), (5, 220, 5, 0, 0xa1), (5, 220, 5, 2, 0xa1),
            (5, 220, 5, 1, 0xe1), (5, 220, 5, 1, 0x21), (5, 220, 5, 1, 0x31),
        )]
    for i, case in enumerate(cases):
        case["file"] = f"bevel-{i}.swf"
        (args.output / case["file"]).write_bytes(movie(case, args.standalone_trace))
    (args.output / "inputs.json").write_text(json.dumps(dict(
        width=32, height=32, source_rect=[8,8,24,24], source_color=[0,255,0,255],
        gradient_colors=[[255,0,0,255],[0,0,0,0],[0,0,255,255]], ratios=[0,128,255], cases=cases,
        exact_expectations={"case0_row16": {"8":[0,0,255], "15":[0,255,0], "23":[255,0,0]},
                            "case4":"bit-identical unfiltered green rectangle"},
        blur_precision="Original Flash32 debugger matrix: compiler/tests/convert_swf_filters.rs sparse samples; selected Director Xtra precision remains distinct"), indent=2) + "\n")
    # Copy generated SWFs to C:\\filter-probe under the existing isolated Wine prefix.
    script = '''global out, pm, i, x, y, p
out=new(xtra("fileio"))
out.createFile("C:\\filter-probe\\observed.txt")
out.openFile("C:\\filter-probe\\observed.txt",2)
pm=new(#flash)
puppetSprite(149,1)
sprite(149).visible=1
repeat with i=0 to 8
 importFileInto(pm,"C:\\filter-probe\\bevel-"&string(i)&".swf")
 sprite(149).member=pm
 sprite(149).rect=rect(0,0,32,32)
 sprite(149).ink=0
 updateStage
 repeat with y=0 to 31
  repeat with x=0 to 31
   p=(the stage).image.getPixel(point(x,y))
   out.writeString(string([i,x,y,p])&RETURN)
  end repeat
 end repeat
end repeat
out.closeFile()
'''
    (args.output / "probe.lingo").write_bytes(script.replace("\n", "\r").encode("ascii"))
if __name__ == "__main__": main()

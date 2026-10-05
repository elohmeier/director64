#!/usr/bin/env python3
"""Verify the converter's PFR1 decoder against the original Bitstream player.

Director MX 2004 ships the TrueDoc Character Shape Player statically linked
into "Font Xtra.x32". This script emulates that player's simple-glyph outline
loop (identity output transform, recording the moveTo/lineTo/curveTo/close
callbacks) and compares it with compiler/src/convert/pfr.rs:

  default   every simple glyph of every recovered PFR font in the build tree
  --fuzz N  N mutations of those glyph programs; a program the decoder
            rejects (it reads past its end, or leaves a value undefined) is
            counted, never guessed

Requires the unicorn package (`uv run --with unicorn tools/fonts/pfr1-verify.py`),
the built converter (`mise run compiler`) and an extracted source providing
the pinned Font Xtra binary.
"""

import argparse
import hashlib
import json
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Font Xtra.x32 from the Lernerfolg Deutsch 1-2 disc; the code addresses
# below belong to exactly this binary.
XTRA_SHA256 = "36cf1efec70e9fcd88f44c964ea95c37353249cbbe331999a8afb11f7e10813a"
XTRA_NAME = "Font Xtra.x32"

BASE = 0x6A100000
OUTLINE_FN = 0x6A11CCF9      # simple-glyph outline loop
HINT_CALL_OFFSET = 0x1CD5A   # push edx; push esi; call <extra-items hints>

SCRATCH = 0x10000000
STACK = 0x20000000
GLYPH = 0x30000000

STATE = SCRATCH
XCTRL, YCTRL = SCRATCH + 0x1000, SCRATCH + 0x1100
XCTRL2, YCTRL2 = SCRATCH + 0x1200, SCRATCH + 0x1300
SCALE1, OFFS1 = SCRATCH + 0x1400, SCRATCH + 0x1500
STUB_MOVE, STUB_CURVE = SCRATCH + 0x2000, SCRATCH + 0x2010
STUB_LINE, STUB_CLOSE = SCRATCH + 0x2020, SCRATCH + 0x2030
RET_TRAP = SCRATCH + 0x2040


class OriginalPlayer:
    """The outline loop of the original CSP, run in-place under Unicorn."""

    def __init__(self, xtra_path):
        from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
        from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP
        self._esp = UC_X86_REG_ESP
        self._eip = UC_X86_REG_EIP
        image = bytearray(xtra_path.read_bytes())
        # Neutralize the extra-items hint call: grid-fitting only, and it
        # reaches state the harness does not model.
        assert image[HINT_CALL_OFFSET:HINT_CALL_OFFSET + 3] == b"\x52\x56\xe8"
        image[HINT_CALL_OFFSET:HINT_CALL_OFFSET + 7] = b"\x90" * 7
        self.uc = uc = Uc(UC_ARCH_X86, UC_MODE_32)
        uc.mem_map(BASE, ((len(image) + 0xFFF) & ~0xFFF) + 0x10000)
        uc.mem_write(BASE, bytes(image))
        uc.mem_map(SCRATCH, 0x100000)
        uc.mem_map(STACK, 0x100000)
        uc.mem_map(GLYPH, 0x10000)
        uc.mem_write(STUB_MOVE, b"\xc2\x0c\x00")   # ret 0xc
        uc.mem_write(STUB_CURVE, b"\xc2\x14\x00")  # ret 0x14
        uc.mem_write(STUB_LINE, b"\xc2\x08\x00")   # ret 0x8
        uc.mem_write(STUB_CLOSE, b"\xc2\x04\x00")  # ret 0x4
        uc.mem_write(RET_TRAP, b"\xf4")            # hlt
        self.events = []
        uc.hook_add(UC_HOOK_CODE, self._on_stub, None, STUB_MOVE, STUB_CLOSE + 1)

    def _arg(self, idx):
        esp = self.uc.reg_read(self._esp)
        return struct.unpack("<I", self.uc.mem_read(esp + 4 * idx, 4))[0]

    @staticmethod
    def _point(packed):
        x, y = packed & 0xFFFF, (packed >> 16) & 0xFFFF
        return (x - 0x10000 if x >= 0x8000 else x,
                y - 0x10000 if y >= 0x8000 else y)

    def _on_stub(self, uc, address, size, _):
        if address == STUB_MOVE:
            self.events.append(("move", self._point(self._arg(2))))
        elif address == STUB_LINE:
            self.events.append(("line", self._point(self._arg(2))))
        elif address == STUB_CURVE:
            self.events.append(("curve", self._point(self._arg(2)),
                                self._point(self._arg(3)),
                                self._point(self._arg(4))))
        elif address == STUB_CLOSE:
            self.events.append(("close",))

    def decode(self, glyph_bytes):
        uc = self.uc
        self.events.clear()
        uc.mem_write(STATE, b"\x00" * 0x900)
        uc.mem_write(STATE + 0x258, struct.pack("<I", XCTRL))
        uc.mem_write(STATE + 0x25C, struct.pack("<I", YCTRL))
        uc.mem_write(STATE + 0x22C, struct.pack("<I", XCTRL2))
        uc.mem_write(STATE + 0x230, struct.pack("<I", YCTRL2))
        # identity device transform: no thresholds, scale 1, offset 0, shift 0
        for slot in (0x240, 0x244):
            uc.mem_write(STATE + slot, struct.pack("<I", SCALE1 + 0x80))
        for slot in (0x248, 0x24C):
            uc.mem_write(STATE + slot, struct.pack("<I", SCALE1))
        for slot in (0x250, 0x254):
            uc.mem_write(STATE + slot, struct.pack("<I", OFFS1))
        uc.mem_write(SCALE1, struct.pack("<h", 1) * 0x40)
        uc.mem_write(OFFS1, b"\x00" * 0x100)
        uc.mem_write(STATE + 0x178, struct.pack("<IIII", STUB_MOVE, STUB_CURVE,
                                                STUB_LINE, STUB_CLOSE))
        uc.mem_write(GLYPH, bytes(glyph_bytes))
        esp = STACK + 0x80000
        uc.mem_write(esp, struct.pack("<IIIII", RET_TRAP, STATE, GLYPH + 1,
                                      GLYPH + len(glyph_bytes) - 1,
                                      glyph_bytes[0]))
        uc.reg_write(self._esp, esp)
        uc.emu_start(OUTLINE_FN, RET_TRAP + 1, timeout=5_000_000,
                     count=5_000_000)
        assert uc.reg_read(self._eip) in (RET_TRAP, RET_TRAP + 1)
        return self._contours()

    def _contours(self):
        contours, cur = [], None
        for e in self.events:
            if e[0] == "move":
                cur = [(0,) + e[1]]
                contours.append(cur)
            elif e[0] == "line":
                cur.append((1,) + e[1])
            elif e[0] == "curve":
                cur.append((2,) + e[3] + e[1] + e[2])
        return [canonical(c) for c in contours]


def canonical(contour):
    """Implicit-close form: a trailing line back to the start is implied."""
    if len(contour) > 2 and contour[-1][0] == 1 and \
            contour[-1][1:] == contour[0][1:]:
        return contour[:-1]
    return contour


def find_xtra():
    for path in sorted(ROOT.glob("build/*/sha256-*/extracted/xtras/" + XTRA_NAME)):
        if hashlib.sha256(path.read_bytes()).hexdigest() == XTRA_SHA256:
            return path
    return None


AOT = ROOT / "compiler/target/release/director64-aot"


def rust(*args):
    return subprocess.run([str(AOT), "director", *args], capture_output=True, text=True)


def simple_programs(fonts):
    for pfr in fonts:
        data = pfr.read_bytes()
        with tempfile.TemporaryDirectory() as out:
            run = rust("font", str(pfr), out)
            if run.returncode:
                raise SystemExit(f"pfr1-verify: {pfr.name}: {run.stderr.strip()}")
            outlines = json.loads(next(Path(out).glob("*.outlines.json")).read_text())
        for g in outlines["glyphs"]:
            off, length = g["sourceOffset"], g["sourceLength"]
            if length > 1 and data[off] < 0x80:  # compounds recurse into simple glyphs
                yield outlines["name"], g, data[off:off + length]


def decoded(contours):
    return [canonical([(int(v[0]),) + tuple(int(round(c)) for c in v[1:])
                       for v in contour]) for contour in contours]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fuzz", type=int, default=0)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    xtra = find_xtra()
    if xtra is None:
        print("pfr1-verify: pinned Font Xtra binary not present in any "
              "extracted source; run extract for lernerfolg-deutsch-1-2 first")
        return 2
    if not AOT.is_file():
        print("pfr1-verify: the converter is not built; run mise run compiler")
        return 2
    player = OriginalPlayer(xtra)
    fonts = sorted(ROOT.glob("build/*/sha256-*/director/fonts/*.pfr"))
    programs = list(simple_programs(fonts))
    if args.fuzz:
        rng = random.Random(args.seed)
        same = rejected = 0
        bad = []
        for _ in range(args.fuzz):
            p = bytearray(rng.choice(programs)[2])
            for _ in range(rng.randint(1, 3)):
                at = rng.randrange(1, len(p))
                p[at] = rng.randrange(256) if rng.random() < 0.5 else p[at] ^ (1 << rng.randrange(8))
            if rng.random() < 0.2:
                p[0] = (p[0] & 0x7F) ^ (1 << rng.randrange(7))
            run = rust("glyph", p.hex())
            if run.returncode:
                rejected += 1
                continue
            if player.decode(bytes(p)) == decoded(json.loads(run.stdout)):
                same += 1
            else:
                bad.append(p.hex())
        print(f"{args.fuzz} mutations: {same} identical, {len(bad)} different, "
              f"{rejected} rejected by the decoder")
        for program in bad[:5]:
            print("  differs:", program)
        return 1 if bad else 0
    mismatches = 0
    names = {}
    for name, g, program in programs:
        if player.decode(program) != decoded(g["contours"]):
            mismatches += 1
            names.setdefault(name, []).append(g["codepoint"])
        else:
            names.setdefault(name, [])
    for name, bad in names.items():
        print(f"{name:40} {'ok' if not bad else f'MISMATCH {bad}'}")
    print(f"{len(fonts)} fonts, {len(programs)} simple glyphs, {mismatches} mismatches")
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Render authored single-style XMED fields from recovered original fonts.

Validation/static raster artifacts use the pinned SDK FreeType source. Empty
fields stay empty. Unsupported or missing source fonts are reported, never
substituted. The N64 runtime renders dynamic text from the same OpenType font.
"""
import argparse
import hashlib
import json
import struct
import subprocess
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def raster_tool():
    out = ROOT / "build/host/font-raster"
    out.mkdir(parents=True, exist_ok=True)
    source = ROOT / "third_party/libdragon/tools/mkfont/freetype"
    adapter = ROOT / "tools/fonts/font-glyphs.cpp"
    inputs = {str(p.relative_to(ROOT)): digest(p) for p in [adapter, source/"FreeTypeAmalgam.c", source/"FreeTypeAmalgam.h"]}
    manifest = out/"build.json"
    old = json.loads(manifest.read_text()) if manifest.exists() else {}
    exe = out/"font-glyphs"
    if old.get("inputs") != inputs or not exe.exists():
        subprocess.run(["cc", "-O2", "-c", str(source/"FreeTypeAmalgam.c"), "-o", str(out/"freetype.o")], check=True)
        subprocess.run(["c++", "-std=c++20", "-O2", "-I"+str(source), str(adapter), str(out/"freetype.o"), "-lm", "-o", str(exe)], check=True)
    report = {"inputs": inputs, "executableSha256": digest(exe)}
    manifest.write_text(json.dumps(report, indent=2)+"\n")
    return exe, report


def png(width, height, rgba):
    def chunk(tag, data):
        return struct.pack(">I", len(data))+tag+data+struct.pack(">I", zlib.crc32(tag+data))
    scanlines = b"".join(b"\0"+rgba[y*width*4:(y+1)*width*4] for y in range(height))
    return b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))+chunk(b"IDAT", zlib.compress(scanlines, 9))+chunk(b"IEND", b"")


def render(text, width, height, style, glyphs):
    rgba = bytearray(width*height*4)
    color = style["color"]
    rgb = bytes([(color>>16)&255, (color>>8)&255, color&255])
    baseline = style["ascent"]
    for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        chars = []
        for ch in line:
            if ord(ch) not in glyphs: raise ValueError(f"missing original glyph U+{ord(ch):04X}")
            chars.append(glyphs[ord(ch)])
        advance = sum(g["advance26_6"] for g in chars)/64
        position = (width-advance)/2 if style["align"] == 1 else width-advance if style["align"] == 2 else 0
        for g in chars:
            alpha = bytes.fromhex(g["coverageHex"])
            left = round(position)+g["left"]
            top = baseline-g["top"]
            for y in range(g["height"]):
                for x in range(g["width"]):
                    tx,ty = left+x,top+y
                    if not (0 <= tx < width and 0 <= ty < height): continue
                    a = alpha[y*g["width"]+x]
                    if not a: continue
                    p = (ty*width+tx)*4
                    # Source-over masks sharing a constant authored color.
                    rgba[p:p+3] = rgb
                    rgba[p+3] = a+(rgba[p+3]*(255-a)+127)//255
            position += g["advance26_6"]/64
        baseline += style["lineHeight"]
    return bytes(rgba)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    model = json.loads(args.model.read_text())
    exe, provenance = raster_tool()
    fonts = {f["number"]: f for f in model.get("fonts", [])}
    cache = {}
    def glyphs(number, size):
        key = (number, size)
        if key not in cache:
            font = fonts[number]
            source = args.model.parent/font["asset"]
            if digest(source) != font["sha256"]: raise ValueError("modified original OpenType font")
            raw = subprocess.check_output([str(exe), str(source), str(size)])
            parsed = json.loads(raw)
            if {g["codepoint"] for g in parsed["glyphs"]} != set(font["codepoints"]):
                raise ValueError("FreeType did not recover every original Unicode mapping")
            cache[key] = {g["codepoint"]: g for g in parsed["glyphs"]}
        return cache[key]
    args.output_dir.mkdir(parents=True, exist_ok=True)
    files, skipped = [], []
    for movie in model["movies"]:
        for member in movie["members"]:
            if "textStyle" not in member: continue
            style = member["textStyle"]
            if not style["fontId"]:
                skipped.append({"movie": movie["name"], "member": member["number"], "reason": "original font unavailable", "font": style["fontName"]})
                continue
            active = [r["index"] for r in member["typography"]["styleRuns"] if r["offset"] < len(member["text"])]
            if len(set(active)) > 1: raise ValueError("static raster requires a single authored character style")
            width,height = member["width"],member["height"]
            rgba = render(member["text"], width, height, style, glyphs(style["fontId"],style["size"]))
            name = f"{movie['name']}-{member['cast']}-{member['number']}.png"
            (args.output_dir/name).write_bytes(png(width,height,rgba))
            files.append({"file": name, "sha256": digest(args.output_dir/name), "width": width, "height": height,
                          "text": member["text"], "style": style, "nonzeroCoveragePixels": sum(bool(a) for a in rgba[3::4])})
    # A separate diagnostic specimen validates accented/dynamic input glyphs;
    # it is not substituted into any recovered member.
    specimen = "Pettson & Findus 0123456789\nÄÖÜ äöü ß Åå Éé"
    if fonts:
        number = min(fonts)
        style = {"size": 20, "color": 0, "align": 0, "ascent": 20, "lineHeight": 32}
        rgba=bytearray(render(specimen,390,70,style,glyphs(number,20)))
        for p in range(0,len(rgba),4):
            a=rgba[p+3]
            for c in range(3): rgba[p+c]=(rgba[p+c]*a+255*(255-a)+127)//255
            rgba[p+3]=255
        (args.output_dir/"original-font-specimen.png").write_bytes(png(390,70,rgba))
    report = {"modelSha256": digest(args.model), "rasterizer": provenance, "rendered": files, "unavailableSourceFonts": skipped,
              "originalProjectorCompared": False, "sourceHintingExported": False}
    (args.output_dir/"result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2)+"\n")
    print(f"Rendered {len(files)} authored fields; {len(skipped)} fields have unavailable original fonts")


if __name__ == "__main__":
    main()

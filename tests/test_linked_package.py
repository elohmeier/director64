"""A source-free compiler/loader ABI check for embedded Director movies."""

import json
import struct
import subprocess

from director64.project import converter, repository


def film_track_data(keys):
    """The reference encoding of package.rs film tracks (film_track.h)."""
    fields = (
        ("I", ("member",)), ("h", ("x",)), ("h", ("y",)),
        ("h", ("width",)), ("h", ("height",)), ("BB", ("ink", "blend")),
        ("BB", ("type", "flags")), ("BB", ("fore", "back")),
        ("BBB", ("thickness", "stretch", "trails")), ("h", ("loc_z",)),
        ("I", ("fore_rgb",)), ("I", ("back_rgb",)),
        ("i", ("rotation",)), ("i", ("skew",)),
    )

    def groups(spec):
        return [struct.pack("<" + fmt, *(spec.get(name, 0) for name in names))
                for fmt, names in fields]

    previous, last, result = groups({}), -1, bytearray()
    if not keys:
        raise ValueError("empty native film track")
    for frame, spec in keys:
        if not last < frame <= 65535:
            raise ValueError("native film keys are not in frame order")
        if any(spec.get(name) for name in ("script", "tempo", "delay", "behaviors")):
            raise ValueError("native film key contains script/main-channel state")
        current = groups(spec)
        mask = sum(1 << i for i, (before, after) in enumerate(zip(previous, current, strict=True))
                   if before != after)
        if not mask:
            raise ValueError("redundant native film key")
        result.extend(struct.pack("<HH", frame, mask))
        for i, value in enumerate(current):
            if mask & (1 << i):
                result.extend(value)
        previous, last = current, frame
    return bytes(result)


def test_linked_movie_metadata_survives_packaging(tmp_path):
    root = repository()
    def layer(channel, *, enabled=True, x=11):
        data = [0] * 48
        data[0], data[1], data[2], data[3] = int(enabled), 136, 2, 3
        data[5], data[7], data[13], data[15] = 2, 1, 7, x
        data[17], data[19], data[21], data[22] = 20, 40, 51, 18
        data[28:32] = (-375).to_bytes(4, "big", signed=True)
        data[32:36] = (125).to_bytes(4, "big", signed=True)
        return {"channel": channel, "bytes": data}

    model = {"problems": [], "extendedD6": True,
             "fonts": [{"number": 3, "aliases": ["Synthetic Original"],
                                        "variants": [{"size": 12, "asset": "fonts/f3-12.font64"}]}],
             "fontMetrics": {"3:12": {"ascent": 9, "descent": 3, "advances": [6] * 95,
                                        "kerning": []}},
             "movies": [
        {"id": 1, "name": "MAIN.DXR", "directorVersion": 850, "tempo": 30,
         "palette": [0xFFFFFF, 0, 0x123456, 0xABCDEF] + [0] * 252,
         "casts": [{"number": 1, "name": "Internal", "file": "MAIN.DXR",
                    "filmCastMap": [1, 1, 2], "patternAssets": ["brush.fdi"] + [None] * 7},
                   {"number": 2, "name": "Sounds", "file": "CHILD.DXR"}],
         "members": [{"cast": 1, "number": 1, "type": 9, "width": 200, "height": 100,
                      "linkedMovie": "CHILD.DXR", "linkedFlags": 0x19,
                      "linkedLeft": 192, "linkedTop": -12},
                     {"cast": 1, "number": 2, "type": 2, "filmFlags": 14,
                      "filmRuntime": True, "left": -9, "top": 3, "width": 60, "height": 40,
                      "flashTimeline": {"stopFrames": [3]},
                      "filmScore": {"version": 13, "frames": [
                          {"channels": [layer(8), layer(6)]},
                          {"channels": [layer(8), layer(6)]},
                          {"channels": [layer(6, enabled=False), layer(8, x=17)]}]},
                      "filmAssets": ["frame-a.fdi", "frame-a.fdi", "frame-b.fdi"],
                      "filmSounds": [[{"cast": 2, "member": 2}, None],
                                     [None, None], [None, {"cast": 2, "member": 2}]]},
                     {"cast": 1, "number": 3, "type": 2, "filmFlags": 38,
                      "filmAssets": ["silent.fdi"],
                      "filmSounds": [[{"cast": 2, "member": 2}, None]]},
                     {"cast": 1, "number": 4, "type": 2, "width": 80, "height": 60,
                      "flashFontUses": [{"fontName": "Synthetic Original", "fontHeight": 12}],
                      "flashFields": [{"name": "my_txt", "text": "AB", "fontHeight": 10,
                                       "embedFonts": True,
                                       "bounds": {"left": 10, "top": 11, "width": 20, "height": 8},
                                       "localBounds": {"left": -0.25, "top": 0.5,
                                                       "width": 20.25, "height": 8.5}}],
                      "flashHelpers": [{"name": "advance", "target": "sun", "property": "_y",
                                        "operation": 1, "argument": 0},
                                       {"name": "decodeCaption", "target": "my_txt",
                                        "property": "text", "operation": 4,
                                        "argument": 0, "method": True}],
                      "filmAssets": ["flash.fdi"], "filmLayers": [[
                          {"channel": 2, "asset": "sun.fda", "name": "sun", "x": 9, "y": 12,
                           "width": 5, "height": 7, "originX": 163840, "originY": -32768},
                          {"channel": 3, "name": "my_txt", "x": 10, "y": 11,
                           "width": 20, "height": 8, "originX": 655360, "originY": 720896,
                           "affine": True, "matrix": [1, 0.5, -0.25, 1.5, 10, 11]}]]},
                     {"cast": 1, "number": 5, "type": 2, "xtra": {"symbol": "flash"},
                      "flashVariantCount": 3, "filmLoop": 1, "flashRate": 24 * 65536,
                      "videoAudio": "intro.wav64", "rate": 44100, "frames": 3356,
                      "flashAudioPositions": [0, 1000], "flashPlaybackMode": 1,
                      "flashFixedRate": 15 * 65536, "flashDirectToStage": 1,
                      "filmAssets": ["0a.fdi", "0b.fdi", "0c.fdi",
                                     "1b.fdi", "1c.fdi", "1a.fdi"]}]},
        {"id": 2, "name": "CHILD.DXR", "directorVersion": 850, "tempo": 30,
         "casts": [{"number": 1, "name": "Internal", "file": "CHILD.DXR"}],
         "members": [{"cast": 1, "number": 1, "type": 3, "text": "AB",
                      "typography": {"styles": [{"foreground": [65535, 0, 0]},
                                                 {"foreground": [0, 0, 65535]}],
                                     "styleRuns": [{"offset": 0, "index": 0},
                                                   {"offset": 1, "index": 1}]},
                      "hyperlinks": [[1, 2, "synthetic-link"]]},
                     {"cast": 1, "number": 2, "type": 6, "asset": "sound.wav64"},
                     {"cast": 1, "number": 3, "type": 4, "palette": [0x123456] * 256},
                     {"cast": 1, "number": 4, "type": 1, "palette": 0, "width": 1, "height": 1}]},
    ]}
    root_layer = {"channel": 1, "clipId": 2, "name": "child", "x": -1, "y": 2,
                  "width": 20, "height": 10, "originX": 32768, "originY": -16384,
                  "affine": True, "matrix": [1, .5, -.25, 1, .5, -.25],
                  "placementGeneration": 3, "clipDepth": 5,
                  "cxform": [128, 256, 256, 192, 1, 2, 3, 4],
                  "actions": [{"frame": 0, "event": 0x20000, "key": 65, "code": [7, 0]}]}
    child_layer = dict(root_layer, channel=2, clipId=0, buttonId=3, clipDepth=0, actions=[])
    raster_layer = {"channel": 1, "asset": "button.fda", "maskAsset": "button-mask.fda",
                    "x": 0, "y": 0,
                    "width": 20, "height": 10, "originX": 0, "originY": 0}
    model["movies"][0]["members"].append({
        "cast": 1, "number": 6, "type": 2, "width": 20, "height": 10,
        "xtra": {"symbol": "flash"}, "flashRate": 12*65536, "flashVersion": 7,
        "filmAssets": ["empty.fdi"], "filmLayers": [[root_layer]],
        "flashRuntime": {
            "clips": [{"id": 0, "frameCount": 1, "localBounds": [0, 0, 20, 10],
                       "frames": [], "actions": [{"frame": 1, "event": 0, "code": [7, 0]}],
                       "labels": [], "soundEvents": []},
                      {"id": 2, "frameCount": 1, "localBounds": [-.5, .25, 20, 10],
                       "frames": [[child_layer]], "actions": [],
                       "labels": [{"name": "start", "frame": 1}],
                       "soundEvents": [{"frame": 1, "sound": 4, "loops": 2, "flags": 15,
                                        "inPoint": 1, "outPoint": 100,
                                        "envelopes": [[0, 32768, 16384], [441, 0, 32768]]}]}],
            "buttons": [{"id": 3, "trackAsMenu": True, "localBounds": [0, 0, 20, 10],
                         "states": [[raster_layer], [], [raster_layer], [raster_layer]],
                         "actions": [{"frame": 0, "event": 4, "code": [6, 0]}]}],
            "sounds": [{"id": 4, "asset": "click.wav64", "samples": 101, "rate": 22050}],
            "exports": [{"id": 2, "name": "ChildSymbol"}],
            "initializers": [{"clipId": 2, "frame": 1, "code": [7, 0]}]}})
    model["movies"][0]["members"].append({
        "cast": 1, "number": 7, "type": 1, "width": 20, "height": 12,
        "xtra": {"symbol": "vectorShape"}, "vectorShape": {
            "vertices": [[-7.5, -3], [9, -2], [1, 8]], "closed": True,
            "filled": True, "strokeWidth": 2.5, "strokeColor": 0x123456,
            "fillColor": 0xABCDEF}})
    model["movies"][0]["members"].append({
        "cast": 1, "number": 8, "type": 14, "name": "synthetic reveal",
        "transition": {"time_raw": 0, "chunk_size": 12, "type": 37,
                       "flags_raw": 2, "duration_ms": 1750}})
    model["movies"][0]["members"].append({
        "cast": 1, "number": 9, "type": 7, "text": "X😀Y", "filled": 7,
        "textBox": {"type": 3, "margin": 2, "border": 1,
                    "boxDropShadow": 2, "wordWrap": False, "background": 0x123456},
        "textFormats": [
            {"offset": offset, "fontId": 3, "size": 12, "face": 0, "align": 3,
             "ascent": 9, "lineHeight": 14, "color": 0x123456,
             "charSpacing": 2 if offset == 0 else -1}
            for offset in (0, 4)]})
    model["movies"][0]["members"].extend([
        {"cast": 1, "number": 10, "type": 7, "width": 14, "height": 16,
         "buttonType": 2, "hilite": True},
        {"cast": 1, "number": 11, "type": 7, "width": 14, "height": 16,
         "buttonType": 3, "hilite": False}])
    model["movies"][1]["members"][0]["textFormats"] = [
        {"offset": 0, "fontId": 3, "size": 12, "face": 1, "align": 0,
         "ascent": 9, "lineHeight": 14, "color": 0x123456},
        {"offset": 1, "fontId": 4, "size": 18, "face": 6, "align": 2,
         "ascent": -2, "lineHeight": 21, "color": 0xABCDEF, "charSpacing": -1000}]
    model["movies"][1]["members"][0]["textBox"] = {
        "type": 4, "margin": 0, "border": 0, "boxDropShadow": 0,
        "wordWrap": True, "background": 0xFFFFFF}
    # A dynamic cast has no authored sprite yet: packaging carries the
    # descriptor; runtime/capability tests separately qualify its playback.
    values = {
        "model.json": model,
        "program.json": {"files": [{"name": "MAIN.DXR.lingo"}, {"name": "CHILD.DXR.lingo"}],
                         "handlers": []},
        "meta.json": {"director_version": 850},
    }
    for name, value in values.items():
        (tmp_path / name).write_text(json.dumps(value))
    package = tmp_path / "game.d64p"
    subprocess.run([
        *converter(), "package", tmp_path / "program.json", tmp_path / "model.json",
        package, "--bytecode", root / "runtime/lingo/lingo_bytecode.h",
        "--meta", tmp_path / "meta.json",
    ], check=True, capture_output=True)
    assert package.read_bytes()[4:6] == b"\x09\0"
    executable = tmp_path / "load"
    subprocess.run([
        "cc", "-std=c17", "-Wall", "-Wextra", "-Werror", "-pedantic",
        "-DDIRECTOR64_DIRECTOR_VERSION=10", "-DDIRECTOR64_D85=1", "-DDIRECTOR64_EXTENDED_D6=1",
        "-Iruntime/director", "-Iruntime/lingo", "-Iruntime/package",
        "tests/native/test_linked_package.c", "runtime/package/package.c", "-o", executable,
    ], cwd=root, check=True, capture_output=True)
    subprocess.run([executable, package], check=True, capture_output=True)
    data = package.read_bytes()
    button_id = (1 << 21) | (1 << 16) | 10
    button_header = struct.pack("<IHHHHHhh", button_id, 10, 1, 7, 14, 16, 0, 0)
    assert data.count(button_header) == 1
    button_offset = data.index(button_header)
    for name, offset, fmt, value in (("kind", 46, "H", 4), ("checked", 50, "B", 2)):
        corrupt = bytearray(data)
        struct.pack_into("<" + fmt, corrupt, button_offset + offset, value)
        rejected = tmp_path / f"invalid-button-{name}.d64p"
        rejected.write_bytes(corrupt)
        subprocess.run([executable, rejected, "reject"], check=True, capture_output=True)
    # Optional vector records are bounded and cannot bind a non-vector member,
    # repeat an identity, carry NaNs or reference more vertices than present.
    sections = struct.unpack_from("<H", data, 10)[0]
    vector_offset = next(struct.unpack_from("<I", data, 32 + i * 12 + 4)[0]
                         for i in range(sections) if data[32 + i * 12:36 + i * 12] == b"VECT")
    for name, offset, fmt, value in (("member", 4, "I", 1), ("count", 8, "H", 1025),
                                   ("flags", 10, "B", 4), ("width", 12, "d", float("nan")),
                                   ("point", 28, "d", float("inf"))):
        corrupt = bytearray(data)
        struct.pack_into("<" + fmt, corrupt, vector_offset + offset, value)
        rejected = tmp_path / f"invalid-vector-{name}.d64p"
        rejected.write_bytes(corrupt)
        subprocess.run([executable, rejected, "reject"], check=True, capture_output=True)
    transition_offset = next(struct.unpack_from("<I", data, 32 + i * 12 + 4)[0]
                             for i in range(sections)
                             if data[32 + i * 12:36 + i * 12] == b"TRAN")
    assert data[transition_offset:transition_offset + 16] == struct.pack(
        "<IIHBBBBBB", 1, (1 << 21) | (1 << 16) | 8, 1750, 37, 12, 1, 0, 0, 0)
    for name, offset, fmt, value in (("member", 4, "I", 1), ("type", 10, "B", 53),
                                   ("area", 12, "B", 2), ("reserved", 13, "B", 1)):
        corrupt = bytearray(data)
        struct.pack_into("<" + fmt, corrupt, transition_offset + offset, value)
        rejected = tmp_path / f"invalid-transition-{name}.d64p"
        rejected.write_bytes(corrupt)
        subprocess.run([executable, rejected, "reject"], check=True, capture_output=True)
    rich_offset = next(struct.unpack_from("<I", data, 32 + i * 12 + 4)[0]
                       for i in range(sections) if data[32 + i * 12:36 + i * 12] == b"RICH")
    spacing_index = next(i for i in range(sections)
                         if data[32 + i * 12:36 + i * 12] == b"RSPC")
    spacing_offset = struct.unpack_from("<I", data, 32 + spacing_index * 12 + 4)[0]
    spacing_id = (1 << 21) | (1 << 16) | 9
    assert data[spacing_offset:spacing_offset + 40] == struct.pack(
        "<IIIhHIIhHIIhH", 3, spacing_id, 0, 2, 0, spacing_id, 4, -1, 0,
        (2 << 21) | (1 << 16) | 1, 1, -1000, 0)
    for name, offset, fmt, value in (
        ("member", 4, "I", 1), ("nontext", 4, "I", (1 << 21) | (1 << 16) | 7),
        ("offset", 8, "I", 3), ("zero", 12, "h", 0), ("reserved", 14, "H", 1),
        ("duplicate", 20, "I", 0), ("count", 0, "I", 4),
    ):
        corrupt = bytearray(data)
        struct.pack_into("<" + fmt, corrupt, spacing_offset + offset, value)
        rejected = tmp_path / f"invalid-spacing-{name}.d64p"
        rejected.write_bytes(corrupt)
        subprocess.run([executable, rejected, "reject"], check=True, capture_output=True)
    corrupt = bytearray(data)
    struct.pack_into("<I", corrupt, 32 + spacing_index * 12 + 8, 41)
    rejected = tmp_path / "invalid-spacing-trailing.d64p"
    rejected.write_bytes(corrupt)
    subprocess.run([executable, rejected, "reject"], check=True, capture_output=True)
    corrupt = bytearray(data)
    rich_index = next(i for i in range(sections)
                      if data[32 + i * 12:36 + i * 12] == b"RICH")
    corrupt[32 + rich_index * 12:36 + rich_index * 12] = b"OLD_"
    rejected = tmp_path / "invalid-spacing-without-rich.d64p"
    rejected.write_bytes(corrupt)
    subprocess.run([executable, rejected, "reject"], check=True, capture_output=True)
    box_offset = next(struct.unpack_from("<I", data, 32 + i * 12 + 4)[0]
                      for i in range(sections) if data[32 + i * 12:36 + i * 12] == b"TBOX")
    assert data[box_offset:box_offset + 16] == struct.pack(
        "<IIBBBBBBBB", 2, (1 << 21) | (1 << 16) | 9, 3, 2, 1, 2, 0, 0x12, 0x34, 0x56)
    for name, offset, fmt, value in (
        ("member", 4, "I", 1), ("type-zero", 8, "B", 0), ("type-range", 8, "B", 5),
        ("wrap", 12, "B", 2),
        ("duplicate", 16, "I", (1 << 21) | (1 << 16) | 9),
        ("nontext", 4, "I", (1 << 21) | (1 << 16) | 7),
    ):
        corrupt = bytearray(data)
        struct.pack_into("<" + fmt, corrupt, box_offset + offset, value)
        rejected = tmp_path / f"invalid-text-box-{name}.d64p"
        rejected.write_bytes(corrupt)
        subprocess.run([executable, rejected, "reject"], check=True, capture_output=True)
    assert data[rich_offset:rich_offset + 12] == struct.pack(
        "<IIHH", 2, (1 << 21) | (1 << 16) | 9, 2, 0)
    for name, offset, fmt, value in (
        ("member", 4, "I", 1), ("count", 8, "H", 0), ("reserved", 10, "H", 1),
        ("first-offset", 12, "I", 1),
        ("face", 18, "B", 16), ("align", 19, "B", 4),
        ("height", 22, "h", -1), ("color", 24, "I", 0x1000000),
        ("order", 28, "I", 0), ("extent", 28, "I", 5),
    ):
        corrupt = bytearray(data)
        struct.pack_into("<" + fmt, corrupt, rich_offset + offset, value)
        rejected = tmp_path / f"invalid-rich-{name}.d64p"
        rejected.write_bytes(corrupt)
        subprocess.run([executable, rejected, "reject"], check=True, capture_output=True)
    initial = {"member": (2 << 21) | (1 << 16) | 1, "x": 11, "y": 7,
               "width": 40, "height": 20, "ink": 8, "blend": 80, "type": 1,
               "fore": 2, "back": 3, "thickness": 18, "stretch": 1, "loc_z": 1,
               "fore_rgb": 0x123456, "back_rgb": 0xABCDEF, "rotation": -375, "skew": 125}
    expected = film_track_data([(0, initial), (2, {})])
    signature = struct.pack("<HHHI", 2, 1, 2, len(expected)) + expected
    assert data.count(signature) == 1
    tracks = data.index(signature)
    first_size = len(film_track_data([(0, initial)]))
    # Empty tracks, invalid masks/frames, duplicate keys/channels and a false
    # byte length fail before playback can use the compact representation.
    for offset, value in ((4, 0), (6, 0), (10, 3), (12, 0x8000),
                          (10 + first_size, 0), (10 + len(expected), 1)):
        corrupt = bytearray(data)
        struct.pack_into("<H", corrupt, tracks + offset, value)
        rejected = tmp_path / f"invalid-track-{offset}.d64p"
        rejected.write_bytes(corrupt)
        subprocess.run([executable, rejected, "reject"], check=True, capture_output=True)

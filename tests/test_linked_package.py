"""A source-free compiler/loader ABI check for embedded Director movies."""

import json
import struct
import subprocess

from director64.aot import generator_command, repository_root


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
    root = repository_root()
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
        *generator_command(root), "package", tmp_path / "program.json", tmp_path / "model.json",
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

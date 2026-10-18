"""The Rust writer and real C loader must agree in every runtime family."""

import json
import struct
import subprocess
from types import SimpleNamespace

import pytest

from director64.aot import generator_command, repository_root
from director64.web import validate_package


@pytest.mark.parametrize(
    ("version", "extended"),
    [(500, False), (600, False), (600, True), (700, True),
     (800, False), (850, True), (1000, True)],
    ids=["d5", "d6", "d6-extended", "d7", "d8", "d85", "d10"],
)
def test_rust_package_member_layout_loads_in_matching_c_profile(tmp_path, version, extended):
    root = repository_root()
    style = {"fontName": "Fixture", "fontId": 3, "size": 13, "color": 0x123456,
             "ascent": 10, "descent": 3, "leading": 2, "lineHeight": 15, "align": 1}
    film = {"cast": 1, "number": 1, "type": 2, "name": "Film", "width": 17, "height": 19,
            "filmAssets": ["first.fdi", "second.fdi"], "filmLoop": 1, "lineDirection": 6}
    if version == 500 or version >= 850:
        film.update(filmFlags=8, filmSounds=[
            [{"cast": 1, "member": 4}, None], [None, {"cast": 1, "member": 4}],
        ])
    text = {"cast": 1, "number": 2, "type": 3, "name": "Text",
            "text": "A\0B" if extended else "Text tail", "textHex": "410042",
            "editable": True, "sourceBytes": 3079, "textStyle": style,
            "textInsertStyle": dict(style, size=14),
            "cuePoints": [{"milliseconds": 1234, "name": "Tail cue"}]}
    model = {"problems": [], "extendedD6": extended, "movies": [{
        "id": 1, "name": "PROFILE.DXR", "directorVersion": version, "tempo": 27,
        "stageColor": 0x345678, "palette": [i * 0x010101 for i in range(256)],
        "casts": [{"number": 1, "name": "Internal", "file": "PROFILE.DXR"}],
        "members": [film, text,
                    {"cast": 1, "number": 3, "type": 4, "name": "Palette",
                     "palette": [0x123400 + i for i in range(16)]},
                    {"cast": 1, "number": 4, "type": 6, "name": "Sound",
                     "asset": "sound.wav64", "frames": 12345, "rate": 22050,
                     "loopStart": 123, "loopEnd": 456, "looping": 1},
                    {"cast": 1, "number": 5, "type": 1, "name": "Last member",
                     "asset": "last.fdi", "width": 23, "height": 31,
                     "regX": -7, "regY": 11}],
        "score": {"frames": [{"channels": []}], "labels": [{"name": "Tail", "frame": 1}]},
    }]}
    for name, value in {
        "model.json": model,
        "program.json": {"files": [{"name": "PROFILE.DXR.lingo"}], "handlers": []},
        "meta.json": {"director_version": version, "extended_d6": extended},
    }.items():
        (tmp_path / name).write_text(json.dumps(value))
    package = tmp_path / "game.d64p"
    subprocess.run([
        *generator_command(root), "package", tmp_path / "program.json", tmp_path / "model.json",
        package, "--bytecode", root / "runtime/lingo/lingo_bytecode.h",
        "--meta", tmp_path / "meta.json",
    ], check=True, capture_output=True, text=True)
    runtime_version = 10 if version == 850 else version // 100
    flags = [f"-DDIRECTOR64_DIRECTOR_VERSION={runtime_version}",
             f"-DDIRECTOR64_EXTENDED_D6={int(extended)}"]
    if version == 850:
        flags.append("-DDIRECTOR64_D85=1")
    executable = tmp_path / "load"
    subprocess.run([
        "cc", "-std=c17", "-Wall", "-Wextra", "-Werror", "-pedantic", *flags,
        "-Iruntime/director", "-Iruntime/lingo", "-Iruntime/package",
        "tests/native/test_package_profiles.c", "runtime/package/package.c", "-o", executable,
    ], cwd=root, check=True, capture_output=True, text=True)
    subprocess.run([executable, package], check=True, capture_output=True, text=True)
    # The production APK gate must exercise that same complete loader, not
    # just accept a valid format/profile header around broken member records.
    game = SimpleNamespace(root=root, directory=tmp_path,
                           data={"port": {"director_version": version, "extended_d6": extended}})
    (tmp_path / "host").mkdir()
    (tmp_path / "host/source-policy.json").write_text("{}")
    data = package.read_bytes()
    assert validate_package(game, tmp_path, package) == {
        "movies": 1, "globals": 0, "bytes": len(data),
    }
    colors = struct.pack("<H16I", 16, *(0x123400 + i for i in range(16)))
    assert data.count(colors) == 1
    corrupt = bytearray(data)
    struct.pack_into("<H", corrupt, data.index(colors), 257)
    rejected = tmp_path / "invalid-palette-count.d64p"
    rejected.write_bytes(corrupt)
    with pytest.raises(ValueError, match="runtime rejected converted package"):
        validate_package(game, tmp_path, rejected)

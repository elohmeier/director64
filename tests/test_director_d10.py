import subprocess
from pathlib import Path

import pytest

from director64.director import generate, score_tempo


@pytest.mark.parametrize("version,flags", [
    (600, []), (850, ["-DDIRECTOR64_DIRECTOR_VERSION=10", "-DDIRECTOR64_D85=1"]),
    (1000, ["-DDIRECTOR64_DIRECTOR_VERSION=10"]),
])
def test_generated_members_preserve_large_film_count_and_loop_flag(tmp_path, version, flags):
    model = {"problems": [], "movies": [{"name": "FILM.DXR", "id": 1, "tempo": 30,
                        "directorVersion": version,
                        "casts": [{"number": 1, "name": "Internal", "file": "FILM.DXR"}],
                        "members": [{"cast": 1, "number": 1, "type": 2,
                                     "filmLoop": 1, "filmAssets": ["frame.fdi"] * 505}]}]}
    generate(model, tmp_path)
    source = tmp_path / "verify.c"
    source.write_text('#include "film_dxr_scene.c"\n'
                      'const lv_movie_t aot_film_dxr={0};\n'
                      'int main(void){const dg_member_t *m=dg_film_dxr.members;'
                      'return m->film_count!=505 || m->film_loop!=1 || m->looping!=0;}\n')
    root = Path(__file__).parents[1]
    executable = tmp_path / "verify"
    subprocess.run(["cc", "-std=c17", "-Wall", "-Wextra", "-Werror", "-pedantic", *flags,
                    "-Iruntime/director", "-Iruntime/lingo", source, "-o", executable],
                   cwd=root, check=True, capture_output=True)
    subprocess.run([executable], check=True)


def movie(version, channel, record):
    return {
        "problems": [],
        "movies": [
            {
                "name": "D10.DXR",
                "id": 1,
                "tempo": 30,
                "directorVersion": version,
                "stageColorRGB": 0x123456,
                "casts": [],
                "members": [],
                "score": {
                    "recordSize": 48,
                    "version": 13,
                    "frames": [
                        {
                            "channels": [
                                {
                                    "channel": channel,
                                    "bytes": list(record),
                                    "changed": [1] * 48,
                                    "behaviors": [],
                                }
                            ]
                        }
                    ],
                },
            }
        ],
    }


def test_declared_director_1000_retains_d7_plus_lowering(tmp_path):
    rate = bytearray(48)
    rate[4:7] = bytes([3, 231, 246])
    assert score_tempo(rate, 10) == 999
    generate(movie(1000, 1, rate), tmp_path)
    scene = (tmp_path / "d10_dxr_scene.c").read_text()
    assert ",0,999,0,0,0,0,0}}" in scene
    assert ",palette,1193046,0,NULL}" in scene


def test_d10_accepts_the_full_authored_sprite_range(tmp_path):
    sprite = bytearray(48)
    sprite[0] = 1
    # Channel 988 is authored sprite 983, beyond the 806-channel D7/D8 bound.
    generate(movie(1000, 988, sprite), tmp_path)
    scene = (tmp_path / "d10_dxr_scene.c").read_text()
    assert "{988," in scene
    # loc_z retains the sprite number; tempo, delay, colors, rotation and
    # skew follow it in the record.
    assert ",983,0,0,0,0,0,0}}" in scene
    generate(movie(1000, 1005, sprite), tmp_path)


def test_d10_channel_bound_fails_closed_and_is_not_a_d8_bound(tmp_path):
    sprite = bytearray(48)
    with pytest.raises(ValueError, match="exceeds profile"):
        generate(movie(1000, 1006, sprite), tmp_path)
    with pytest.raises(ValueError, match="exceeds profile"):
        generate(movie(800, 988, sprite), tmp_path)

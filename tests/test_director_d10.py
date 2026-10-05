import pytest

from director64.director import generate, score_tempo


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
    assert ",palette,1193046}" in scene


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

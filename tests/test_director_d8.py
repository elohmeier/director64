import pytest

from director64.director import generate, score_tempo


@pytest.mark.parametrize("version", [7, 8])
def test_d7_d8_tempo_profile_retains_999_and_delay_operand(tmp_path, version):
    rate = bytearray(48)
    rate[4:7] = bytes([3, 231, 246])
    delay = bytearray(48)
    delay[4:7] = bytes([0, 2, 247])
    assert score_tempo(rate, version) == 999
    assert score_tempo(delay, version) == 0
    model = {
        "problems": [],
        "movies": [
            {
                "name": "D8.DXR",
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
                                    "channel": 1,
                                    "bytes": list(record),
                                    "changed": [1] * 48,
                                    "behaviors": [],
                                }
                            ]
                        }
                        for record in [rate, delay]
                    ],
                },
            }
        ],
    }
    generate(model, tmp_path)
    scene = (tmp_path / "d8_dxr_scene.c").read_text()
    # The record's tail is loc_z, tempo, delay, the two colors, rotation, skew.
    assert ",0,999,0,0,0,0,0}}" in scene
    assert ",0,0,2,0,0,0,0}}" in scene
    assert ",palette,1193046}" in scene


def test_long_authored_text_compiles_as_bytes_without_c_literal_limit(tmp_path):
    text = "Ä" * 5000
    generate(
        {
            "problems": [],
            "movies": [
                {
                    "name": "D8.DXR",
                    "id": 1,
                    "tempo": 30,
                    "directorVersion": 8,
                    "casts": [{"name": "Internal", "file": "D8.DXR", "number": 1}],
                    "members": [{"cast": 1, "number": 1, "type": 3, "text": text}],
                }
            ],
        },
        tmp_path,
    )
    scene = (tmp_path / "d8_dxr_scene.c").read_text()
    assert "static const unsigned char text_1_1[]=" in scene
    assert "(const char *)text_1_1" in scene
    assert scene.count("195,132") == 5000


def test_declared_director_600_retains_d6_native_layout(tmp_path):
    record = bytearray(24)
    record[4:7] = bytes([0, 30, 246])
    generate(
        {
            "problems": [],
            "movies": [
                {
                    "name": "D6.DXR",
                    "id": 1,
                    "tempo": 30,
                    "directorVersion": 600,
                    "members": [],
                    "casts": [],
                    "score": {
                        "recordSize": 24,
                        "version": 12,
                        "frames": [
                            {
                                "channels": [
                                    {
                                        "channel": 1,
                                        "bytes": list(record),
                                        "changed": [1] * 24,
                                        "behaviors": [],
                                    }
                                ]
                            }
                        ],
                    },
                }
            ],
        },
        tmp_path,
    )
    scene = (tmp_path / "d6_dxr_scene.c").read_text()
    assert "{1,2047,{0,0,0,0,0,0,0,0,30,0,0,0,0,0,0}}" in scene
    assert "casts,members,member_index,frames,deltas,labels,palette};" in scene

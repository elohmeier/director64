import re
import shutil
import subprocess

import pytest

from director64.aot import cstring
from director64.director import (
    generate,
    member_name_hash,
    score_tempo,
    validate_script_casts,
)


def test_external_behavior_namespace_must_match_resources():
    model = {
        "movies": [
            {
                "name": "SHARED.CXT",
                "casts": [
                    {"name": "Internal", "file": "SHARED.CXT"},
                ],
            }
        ]
    }
    program = {"handlers": [{"movie": "SHARED.CXT", "cast": "External", "member": 5}]}
    with pytest.raises(ValueError, match="namespace"):
        validate_script_casts(model, program)
    model["movies"][0]["casts"][0]["name"] = "External"
    validate_script_casts(model, program)


@pytest.mark.parametrize("rate", [1, 2, 3, 8, 13, 50, 120])
def test_d6_tempo_operand_is_preserved_in_generated_scene(tmp_path, rate):
    record = bytearray(24)
    record[4:6] = rate.to_bytes(2, "big")
    record[6] = 246
    model = {
        "problems": [],
        "movies": [
            {
                "name": "CLOCK.DXR",
                "id": 1,
                "tempo": 120,
                "members": [],
                "casts": [],
                "score": {
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
                    ]
                },
            }
        ],
    }
    generate(model, tmp_path)
    # The FPS word must survive into the native delta, not the opcode 246.
    scene = (tmp_path / "clock_dxr_scene.c").read_text()
    assert f"{{1,2047,{{0,0,0,0,0,0,0,0,{rate},0,0,0,0,0,0}}}}" in scene


def sprite_delta(tmp_path, record, changed):
    model = {
        "problems": [],
        "movies": [
            {
                "name": "SPRITE.DXR",
                "id": 1,
                "tempo": 30,
                "members": [],
                "casts": [{"name": "Internal", "file": "SPRITE.DXR", "number": 1}],
                "score": {
                    "frames": [
                        {
                            "channels": [
                                {
                                    "channel": 13,
                                    "bytes": list(record),
                                    "changed": [int(i in changed) for i in range(24)],
                                    "behaviors": [],
                                }
                            ]
                        }
                    ]
                },
            }
        ],
    }
    generate(model, tmp_path)
    match = re.search(r"\{13,(\d+),\{([^}]+)\}\}", (tmp_path / "sprite_dxr_scene.c").read_text())
    assert match
    return int(match[1]), list(map(int, match[2].split(",")))


@pytest.mark.parametrize("flag", [0, 128, 16, 144])
def test_credits_record_preserves_disabled_blend_amount_and_enable_flag(tmp_path, flag):
    # HALLTYST frames 222/223: logo member 75 has raw transparency 255 but
    # disabled blending. Retain both fields so later flag-only changes work.
    record = bytearray(24)
    record[0] = 16
    record[4:8] = bytes([0, 1, 0, 75])
    record[21:23] = bytes([255, flag])
    mask, values = sprite_delta(tmp_path, record, [22])
    assert mask == 512
    assert values[0] == 0x11004B
    assert values[6:8] == [0, 0]  # copy ink, stored opacity zero
    assert values[12] == flag


@pytest.mark.parametrize(
    "offsets,mask",
    [
        ([4], 1),
        ([7], 1),
        ([12], 2),
        ([15], 2),
        ([18], 4),
        ([19], 4),
        ([1], 8),
        ([21], 16),
        ([0], 32),
        ([16], 64),
        ([17], 64),
        ([2], 128),
        ([3], 256),
        ([22], 512),
        ([20], 1024),
        ([23], 0),
        ([2, 16, 20], 128 | 64 | 1024),
    ],
)
def test_d6_property_copyback_boundaries(tmp_path, offsets, mask):
    assert sprite_delta(tmp_path, bytes(24), offsets)[0] == mask


@pytest.mark.parametrize("opcode,rate", [(247, 1), (255, 2), (3, 0), (246, 0), (246, 121)])
def test_unimplemented_tempo_modes_and_invalid_rates_are_rejected(opcode, rate):
    record = bytearray(24)
    record[4:6] = rate.to_bytes(2, "big")
    record[6] = opcode
    with pytest.raises(ValueError, match="unsupported Director 6"):
        score_tempo(record)


def test_empty_tempo_channel_does_not_reset_the_rate():
    assert score_tempo(bytes(24)) == 0


NAME_HASH_SAMPLES = [
    "",
    "Bild",
    "bild",
    "BILD",
    "Mulle Meck",
    "pTask1",
    "pTask2",
    "Grünä",
    "picture1.pct",
    "a" * 64,
]


def test_member_name_hash_matches_the_runtime(tmp_path):
    """The generated member_index is useless if the two hashes disagree: the
    runtime would binary-search to the wrong run and report every member of
    that cast as absent."""
    compiler = shutil.which("cc")
    if not compiler:
        pytest.skip("native compiler unavailable")
    literals = ",".join(cstring(name) for name in NAME_HASH_SAMPLES)
    (tmp_path / "harness.c").write_text(
        '#include "director.h"\n'
        "#include <stdio.h>\n"
        f"static const char *const names[]={{{literals}}};\n"
        "int main(void) {\n"
        "  for (unsigned i = 0; i < sizeof(names)/sizeof(*names); i++)\n"
        '    printf("%u\\n", (unsigned)dg_member_name_hash(names[i]));\n'
        "}\n"
    )
    executable = tmp_path / "hash-test"
    subprocess.run(
        [
            compiler,
            "-std=c17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pedantic",
            "-Iruntime/director",
            "-Iruntime/lingo",
            "-Iruntime/interaction",
            str(tmp_path / "harness.c"),
            "runtime/director/director.c",
            "runtime/lingo/lingo_runtime.c",
            "-lm",
            "-o",
            str(executable),
        ],
        check=True,
    )
    printed = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
    assert printed.stdout.split() == [str(member_name_hash(n)) for n in NAME_HASH_SAMPLES]


def test_member_index_orders_the_whole_table_stably(tmp_path):
    members = [
        {"cast": 1, "number": 3, "type": 1, "name": "Zebra"},
        {"cast": 1, "number": 1, "type": 1, "name": ""},
        {"cast": 1, "number": 2, "type": 1, "name": "zebra"},
        {"cast": 2, "number": 1, "type": 1, "name": ""},
    ]
    generate(
        {
            "problems": [],
            "movies": [
                {
                    "name": "IDX.DXR",
                    "id": 1,
                    "tempo": 30,
                    "casts": [
                        {"name": "Internal", "file": "IDX.DXR", "number": 1},
                        {"name": "Second", "file": "IDX.DXR", "number": 2},
                    ],
                    "members": members,
                    "score": {"frames": []},
                }
            ],
        },
        tmp_path,
    )
    entries = re.search(
        r"static const dg_member_index_t member_index\[\]=\{(.*?)\};",
        (tmp_path / "idx_dxr_scene.c").read_text(),
        re.S,
    )
    index = [
        (int(h), int(position))
        for h, position in re.findall(r"\{(\d+)u,(\d+)\}", entries.group(1))
    ]
    # Sorted members are (cast, number): "", "zebra", "Zebra", "" — so both
    # unnamed entries share a hash and must stay in that order, and the two
    # spellings of one name must land in the same run.
    assert [h for h, _ in index] == sorted(h for h, _ in index)
    assert sorted(position for _, position in index) == list(range(len(members)))
    unnamed = [position for h, position in index if h == member_name_hash("")]
    assert unnamed == [0, 3]
    assert len({h for h, position in index if position in (1, 2)}) == 1

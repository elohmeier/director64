from __future__ import annotations

import json
import shutil
import subprocess
from copy import deepcopy

import pytest

from director64.captures import (
    CaptureError,
    compare_captures,
    semantic_trace,
    validate_comparison,
    validate_media,
    validate_probe,
    write_json,
)
from director64.evidence import sha256

SCENARIO_ID = "synthetic-media-contract"


@pytest.fixture
def capture_events():
    events = [
        {"type": "SCENARIO_START", "tick": 0},
        {
            "type": "MEMORY_SNAPSHOT",
            "tick": 0,
            "free_bytes": 5_000_000,
            "used_bytes": 2_600_000,
            "fragmented_bytes": 168,
            "scene_arena_used_bytes": 21,
            "scene_arena_high_water_bytes": 21,
        },
    ]
    for scene, tick in (("profile", 0), ("hub", 150), ("vemory", 270)):
        events.extend(
            [
                {"type": "SCENE_LOADED", "scene_id": scene, "tick": tick},
                {"type": "SCENE_FIRST_FRAME", "scene_id": scene, "tick": tick + 1},
            ]
        )
        if scene != "vemory":
            events.append({"type": "SCENE_UNLOADED", "scene_id": scene, "tick": tick + 100})
    for pair in range(1, 9):
        events.append(
            {
                "type": "CHECKPOINT",
                "tick": 287 + pair * 44,
                "assertion_id": f"prototype-pairs-{pair}",
                "actual": pair,
            }
        )
    events.append({"type": "SCENARIO_OK", "tick": 639})
    return [
        {
            **event,
            "schema_version": 1,
            "sequence": sequence,
            "run_id": SCENARIO_ID,
            "scenario_id": SCENARIO_ID,
        }
        for sequence, event in enumerate(events)
    ]


@pytest.fixture
def probe():
    return {
        "streams": [
            {
                "codec_type": "video",
                "codec_name": "ffv1",
                "width": 640,
                "height": 480,
                "avg_frame_rate": "60/1",
                "nb_read_frames": "720",
            }
        ],
        "format": {"duration": "12.000000"},
    }


@pytest.mark.parametrize(
    "mutation",
    [
        "small",
        "fps",
        "short",
        "long",
        "missing-count",
        "duplicate-stream",
        "nan-duration",
    ],
)
def test_media_metadata_has_exact_declared_shape(probe, mutation):
    if mutation == "small":
        probe["streams"][0]["width"] = 320
    elif mutation == "fps":
        probe["streams"][0]["avg_frame_rate"] = "30/1"
    elif mutation == "short":
        probe["streams"][0]["nb_read_frames"] = "719"
    elif mutation == "long":
        probe["format"]["duration"] = "12.5"
    elif mutation == "missing-count":
        del probe["streams"][0]["nb_read_frames"]
    elif mutation == "duplicate-stream":
        probe["streams"].append(deepcopy(probe["streams"][0]))
    elif mutation == "nan-duration":
        probe["format"]["duration"] = "nan"
    with pytest.raises(CaptureError):
        validate_probe(probe)


@pytest.fixture
def synthetic_video(tmp_path):
    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        pytest.skip("FFmpeg/FFprobe are required for actual media decoding")
    path = tmp_path / "synthetic.mkv"
    subprocess.run(
        [
            "ffmpeg",
            "-hide_banner",
            "-v",
            "error",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=size=640x480:rate=60",
            "-frames:v",
            "6",
            "-c:v",
            "ffv1",
            str(path),
        ],
        check=True,
        capture_output=True,
    )
    return path


def test_media_is_decoded_not_only_metadata_checked(synthetic_video):
    metrics, _ = validate_media(synthetic_video, expected_frames=6)
    assert metrics["decoded_without_errors"]
    assert metrics["frames"] == 6
    assert metrics["sha256"] == sha256(synthetic_video)


def test_truncated_media_fails_actual_decoder(synthetic_video):
    data = synthetic_video.read_bytes()
    synthetic_video.write_bytes(data[: len(data) // 2])
    with pytest.raises(CaptureError):
        validate_media(synthetic_video, expected_frames=6)


def test_random_bytes_cannot_be_claimed_as_video(tmp_path):
    video = tmp_path / "not-video.mp4"
    video.write_bytes(bytes(range(256)) * 1000)
    with pytest.raises(CaptureError):
        validate_media(video)


def test_records_cannot_be_overwritten(tmp_path):
    path = tmp_path / "record.json"
    write_json(path, {"immutable": True})
    with pytest.raises(FileExistsError):
        write_json(path, {"immutable": False})
    assert json.loads(path.read_text())["immutable"]


def test_semantic_comparison_ignores_presentation_timing_but_keeps_game_ticks(capture_events):
    changed = deepcopy(capture_events)
    changed[1]["free_bytes"] -= 100
    changed[3]["tick"] += 1
    assert semantic_trace(capture_events) == semantic_trace(changed)
    changed[-2]["tick"] += 1
    assert semantic_trace(capture_events) != semantic_trace(changed)


@pytest.mark.parametrize("mismatch", [None, "tick", "capture-id", "replay"])
def test_determinism_requires_independent_identical_simulations(
    tmp_path, evidence_factory, mismatch
):
    suite = tmp_path / "suite"
    suite.mkdir()
    for index in (1, 2):
        run = suite / f"run-{index}"
        run.mkdir()
        record, events = evidence_factory()
        record["capture_id"] = f"suite/run-{index}"
        if index == 2 and mismatch == "capture-id":
            record["capture_id"] = "suite/run-1"
        if index == 2 and mismatch == "replay":
            record["replay"]["seed"] += 1
        if index == 2 and mismatch == "tick":
            events[1]["tick"] = 2
        for artifact in record["artifacts"]:
            source = tmp_path / artifact["path"]
            target = run / source.name
            if artifact["role"] == "guest_events":
                target.write_text("\n".join(json.dumps(event) for event in events))
            else:
                shutil.copyfile(source, target)
            artifact.update(path=str(target.relative_to(tmp_path)), sha256=sha256(target))
        write_json(run / "evidence.json", record)
    if mismatch is None:
        assert compare_captures(tmp_path, suite)["status"] == "passing"
        path = suite / "determinism.json"
        reference = {"path": str(path.relative_to(tmp_path)), "sha256": sha256(path)}
        assert validate_comparison(reference, tmp_path)["status"] == "passing"
        record = json.loads(path.read_text())
        record["semantic_trace"][-1]["tick"] += 1
        path.write_text(json.dumps(record))
        reference["sha256"] = sha256(path)
        with pytest.raises(CaptureError, match="differs from evaluated runs"):
            validate_comparison(reference, tmp_path)
    else:
        with pytest.raises(CaptureError):
            compare_captures(tmp_path, suite)

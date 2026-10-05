import hashlib
import json
import os
import subprocess
from types import SimpleNamespace

import pytest

from director64.captures import CaptureError
from director64.evidence import sha256
from director64.project import GameSpec


@pytest.fixture
def boot(monkeypatch):
    monkeypatch.delenv("DIRECTOR64_BOOT_JOURNEY", raising=False)
    for key in ("DIRECTOR64_GAME", "DIRECTOR64_SOURCE", "DIRECTOR64_WORK_DIR"):
        monkeypatch.setenv(key, os.environ.get(key, ""))
    return GameSpec.load("findus-mucklas").host("full_boot")


def passing_log():
    return "\n".join(
        [
            "DIRECTOR64 NATIVE_SCENE name=START.DXR frame=1 tick=0",
            "DIRECTOR64 NATIVE_RENDER_READY",
            "DIRECTOR64 NATIVE_SCENE name=INTRO.DXR frame=1 tick=1",
            "DIRECTOR64 NATIVE_SCENE name=POFINTRO.DXR frame=1 tick=681",
            "DIRECTOR64 NATIVE_SCENE name=LO.DXR frame=1 tick=1441",
            "Capture marker matched at 24.391 emulated seconds; recording starts at 25.391",
            "DIRECTOR64 NATIVE_ALPHA_DRAW movie=LO.DXR format=FDIA rgb_bits=15 "
            "alpha_bits=8 tile_bytes=3072 member=16777401",
            "DIRECTOR64 NATIVE_SOURCE_STATE movie=LO.DXR name=lo_state value=intro tick=1441",
            'DIRECTOR64 NATIVE_TRACE ["File unavailable",-37]',
            "DIRECTOR64 NATIVE_TIMING_OVERRUN us=6226369",
            "DIRECTOR64 NATIVE_TICK movie=LO.DXR frame=10 tick=1500 free=1234",
            "DIRECTOR64 NATIVE_COST ticks_us=10 render_us=20 audio_us=30 renders=2 steps=4",
            "DIRECTOR64 NATIVE_TICK movie=LO.DXR frame=10 tick=1800 free=1234",
            "DIRECTOR64 NATIVE_COST ticks_us=2000000 render_us=3000000 "
            "audio_us=1000000 renders=200 steps=40",
            "DIRECTOR64 NATIVE_SOURCE_STATE movie=LO.DXR name=lo_state value=valjer tick=2500",
            "Capture complete: 3600 frames in 10.5 wall seconds",
        ]
    )


def test_log_limits_qualification_and_retains_guest_costs(boot):
    result = boot.validate_log(passing_log())
    assert result["max_reported_guest_timing_overrun_us"] == 6226369
    assert not result["realtime_qualified"]
    assert result["cost_samples"][0]["render_us"] == 20
    assert result["chooser_observed"]
    assert result["full_alpha_draws_queued"][0]["alpha_bits"] == 8
    assert len(result["steady_lo_cost_samples"]) == 1
    assert result["steady_lo_cost_samples"][0]["tick"] == 1800
    assert result["steady_lo_max_work_to_budget_ratio"] == 1.2
    with pytest.raises(CaptureError, match="scene sequence"):
        boot.validate_log(passing_log().replace("POFINTRO.DXR", "HUSET.DXR"))
    with pytest.raises(CaptureError, match="did not start and finish"):
        boot.validate_log(passing_log().replace("3600 frames", "1200 frames"))


def test_chooser_requires_authored_state_evidence(boot):
    with pytest.raises(CaptureError, match="player chooser"):
        boot.validate_log(passing_log().replace("value=valjer", "value=findusprat"))
    with pytest.raises(CaptureError, match="player chooser"):
        boot.validate_log(
            passing_log() + "\nDIRECTOR64 NATIVE_SOURCE_STATE movie=LO.DXR name=lo_state "
            "value=skriverNamn tick=3000"
        )


def test_alpha_path_requires_full_precision_draw_evidence(boot):
    with pytest.raises(CaptureError, match="full-alpha texture"):
        boot.validate_log(passing_log().replace("alpha_bits=8", "alpha_bits=1"))


def test_saved_chooser_requires_actual_original_font_glyphs(boot):
    with pytest.raises(CaptureError, match="Pettson font glyphs"):
        boot.validate_log(passing_log(), require_text=True)
    text = "\nDIRECTOR64 NATIVE_TEXT_DRAW movie=LO.DXR member=524329 font=1 size=12 glyphs=7"
    assert boot.validate_log(passing_log() + text, require_text=True)["text_draws_queued"][0] == {
        "movie": "LO.DXR",
        "member": 524329,
        "font": 1,
        "size": 12,
        "glyphs": 7,
    }
    for replacement in ("font=0", "font=2"):
        with pytest.raises(CaptureError, match="Pettson font glyphs"):
            boot.validate_log(
                passing_log() + text.replace("font=1", replacement), require_text=True
            )


def test_save_fixture_pins_native_receipts_and_isolates_gopher_bytes(boot, tmp_path):
    fixture = bytes(range(256)) * 512
    fixture_path = tmp_path / "native.fla"
    fixture_path.write_bytes(fixture)
    export = {
        "path": fixture_path.name,
        "sha256": sha256(fixture_path),
        "bytes": len(fixture),
        "origin": "synthetic native journey test",
    }
    journey_path = tmp_path / "journey.json"
    journey_path.write_text(json.dumps({"error": None, "flash_export": export}))
    report = {
        "status": "passing",
        "platform": "host-native",
        "scenario": "startup-player-create-reload",
        "journey_sha256": sha256(journey_path),
        "flash_export": export,
    }
    report_path = tmp_path / "validation.json"
    report_path.write_text(json.dumps(report))
    rom = tmp_path / "frozen.z64"
    header = bytearray(64)
    header[0x20:0x34] = b"Findus Mucklas".ljust(20)
    rom.write_bytes(header)
    run = tmp_path / "capture"
    result = boot.seed_save_fixture(report_path, rom, run)
    expected = f"data/gopher64/saves/Findus Mucklas-{sha256(rom).upper()}.fla"
    assert result["emulator_save_path"] == expected
    assert (run / expected).read_bytes() == fixture
    assert (run / result["fixture"]["path"]).read_bytes() == fixture
    assert result["report_sha256"] == sha256(report_path)
    assert not result["n64_input_journey_qualified"]
    fixture_path.write_bytes(bytes(131072))
    with pytest.raises(CaptureError, match="fixture bytes differ"):
        boot.seed_save_fixture(report_path, rom, tmp_path / "corrupt")
    fixture_path.write_bytes(fixture)
    journey_path.write_text(json.dumps({"error": "failed", "flash_export": export}))
    with pytest.raises(CaptureError, match="passing native journey"):
        boot.seed_save_fixture(report_path, rom, tmp_path / "changed-journey")
    report["journey_sha256"] = hashlib.sha256(journey_path.read_bytes()).hexdigest()
    report_path.write_text(json.dumps(report))
    with pytest.raises(CaptureError, match="passing native journey"):
        boot.seed_save_fixture(report_path, rom, tmp_path / "failed-journey")


@pytest.mark.parametrize(
    "marker",
    [
        "DIRECTOR64 NATIVE_FAIL bad service",
        "DIRECTOR64 NATIVE_LINK_ERROR overlay",
        "DIRECTOR64 NATIVE_RENDER_UNSUPPORTED member=123",
        "REQUIREMENT_FAIL rdram",
        "RSP CRASH",
        "ASSERTION FAILED",
        "Error: capture timed out",
        "thread panicked",
    ],
)
def test_structured_failure_markers_are_fatal(boot, marker):
    with pytest.raises(CaptureError, match="failure"):
        boot.validate_log(passing_log() + "\n" + marker)


def test_audio_requires_full_decoded_stereo_and_finite_signal(boot):
    probe = {
        "streams": [
            {
                "codec_type": "audio",
                "codec_name": "aac",
                "channels": 2,
                "sample_rate": "22050",
                "duration": "60.000000",
            }
        ]
    }
    stats = "RMS level dB: -18.79\nPeak level dB: -3.10\nNumber of samples: 1324032\n"
    assert boot.validate_audio(probe, stats)["non_silent"]
    with pytest.raises(CaptureError, match="silent or incomplete"):
        boot.validate_audio(probe, stats.replace("-18.79", "-inf"))
    probe["streams"][0]["duration"] = "10.0"
    with pytest.raises(CaptureError, match="full capture"):
        boot.validate_audio(probe, stats)


def test_measured_work_budget_requires_complete_contiguous_windows(boot):
    samples = [
        {
            "movie": "LO.DXR",
            "tick": tick,
            "ticks_us": 4_000_000,
            "render_us": 700_000,
            "audio_us": 300_000,
        }
        for tick in range(1500, 4200, 300)
    ]
    # Initial scene loading may be expensive; its mixed sample is excluded.
    samples[0]["ticks_us"] = 9_000_000
    result = boot.validate_work_budget({"cost_samples": samples})
    assert result["passed"] and result["contiguous_windows"] == 8
    assert result["maximum_work_to_budget_ratio"] == 1
    assert not result["realtime_qualified"]
    with pytest.raises(CaptureError, match="eight complete"):
        boot.validate_work_budget({"cost_samples": samples[:-1]})
    samples[4]["tick"] += 300
    with pytest.raises(CaptureError, match="not contiguous"):
        boot.validate_work_budget({"cost_samples": samples})
    samples[4]["tick"] -= 300
    samples[-1]["audio_us"] += 1
    with pytest.raises(CaptureError, match="exceeds 5000000"):
        boot.validate_work_budget({"cost_samples": samples})


def test_representative_frames_reject_blank_or_missing_images(boot):
    frame = bytes(range(256)) * 36
    assert len(boot.validate_pixels(frame * 6)) == 6
    with pytest.raises(CaptureError, match="frame count"):
        boot.validate_pixels(frame * 3)
    with pytest.raises(CaptureError, match="blank"):
        boot.validate_pixels(bytes(len(frame) * 6))


def test_freeze_preserves_checked_rom_and_sources(boot, tmp_path):
    game = SimpleNamespace(
        slug="findus-mucklas",
        work=tmp_path / "build/mucklas",
        directory=tmp_path / "games/findus-mucklas",
    )
    names = [
        "build/mucklas/n64/findus-mucklas.z64",
        "build/mucklas/n64/release/findus-mucklas.elf",
        "build/mucklas/n64/release/metadata.ini",
        "build/mucklas/director/model.json",
        "build/mucklas/director/packed.json",
        "build/mucklas/aot/c/manifest.json",
        "build/mucklas/build-inputs.json",
        "games/findus-mucklas/game.toml",
        "config/provenance.toml",
        "runtime/test.c",
    ]
    for name in names:
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(name)
    validation = {"rom_sha256": sha256(tmp_path / names[0])}
    frozen = boot.freeze(tmp_path, game, tmp_path / "capture", validation)
    for name in names:
        (tmp_path / name).write_text("next build")
        assert (tmp_path / "capture" / frozen["files"][name]["path"]).read_text() == name
    with pytest.raises(CaptureError, match="ROM changed"):
        boot.freeze(tmp_path, game, tmp_path / "later-capture", validation)


def test_failed_emulator_keeps_log_inputs_and_isolated_state(boot, tmp_path, monkeypatch):
    game = SimpleNamespace(
        root=tmp_path,
        slug="findus-mucklas",
        work=tmp_path / "build",
        source={"sha256": "synthetic"},
    )
    monkeypatch.setattr(boot, "selected_game", lambda: game)
    monkeypatch.setattr(boot, "validate_rom", lambda *_: {"rom_sha256": "synthetic"})
    monkeypatch.setattr(boot, "freeze", lambda *_: {"rom": "frozen.z64", "files": {}})
    monkeypatch.setattr(boot.shutil, "which", lambda _: "synthetic-gopher64")
    monkeypatch.setattr(
        boot,
        "command",
        lambda *_: " ".join(boot.capture_command("gopher64", "frozen.z64", tmp_path)),
    )
    commands = []

    def run(args, **kwargs):
        commands.append((args, kwargs))
        kwargs["stdout"].write("DIRECTOR64 NATIVE_SCENE name=START.DXR\nNATIVE_FAIL test\n")
        return subprocess.CompletedProcess(args, 1)

    monkeypatch.setattr(boot.subprocess, "run", run)
    before = dict(os.environ)
    with pytest.raises(CaptureError, match="preserved evidence"):
        boot.main()
    assert before == dict(os.environ)
    capture = next((game.work / "gopher-boot").iterdir())
    result = json.loads((capture / "result.json").read_text())
    assert result["status"] == "failed" and result["last_scene"] == "START.DXR"
    assert result["highest_gate"] is None and result["exit_code"] == 1
    assert (capture / "capture.json").is_file()
    args, kwargs = commands[0]
    assert kwargs["timeout"] == 250
    assert kwargs["env"]["XDG_DATA_HOME"] == str(capture / "data")
    assert args[args.index("--capture-duration") + 1] == "60"
    assert args[args.index("--capture-start-timeout") + 1] == "240"

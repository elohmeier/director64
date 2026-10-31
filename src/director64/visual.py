"""Real-browser journey captures and explicit, reviewed visual/audio baselines.

Captured game data stays local. A captured or self-consistent port is not an
original-projector comparison, and a web result never qualifies Android TV.
"""

from __future__ import annotations

import argparse
import array
import hashlib
import html
import json
import math
import os
import platform
import re
import shutil
import signal
import struct
import subprocess
import sys
import zlib
from collections import Counter
from pathlib import Path

from .journeys import plans

KINDS = {"port-regression", "original-wine", "original-windows"}


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    if len(rgba) != width * height * 4:
        raise ValueError("RGBA dimensions disagree")

    def chunk(kind, data):
        return (
            struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        )

    raw = b"".join(b"\0" + rgba[y * width * 4 : (y + 1) * width * 4] for y in range(height))
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw))
        + chunk(b"IEND", b"")
    )


def read_png(path: Path) -> tuple[int, int, bytes]:
    """Read the lossless RGB/RGBA8 PNGs emitted by Chromium and adb screencap."""
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("not a PNG")
    at, payload, header = 8, bytearray(), None
    ended = False
    while at + 12 <= len(data):
        length = struct.unpack_from(">I", data, at)[0]
        kind, body = data[at + 4 : at + 8], data[at + 8 : at + 8 + length]
        if len(body) != length or at + 12 + length > len(data):
            raise ValueError("truncated PNG")
        if zlib.crc32(kind + body) != struct.unpack_from(">I", data, at + 8 + length)[0]:
            raise ValueError("PNG checksum mismatch")
        if kind == b"IHDR":
            if header is not None or length != 13:
                raise ValueError("invalid PNG header")
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            payload.extend(body)
        elif kind == b"IEND":
            ended = True
            break
        at += length + 12
    if header is None or not ended:
        raise ValueError("incomplete PNG")
    width, height, depth, color, compression, filtering, interlace = header
    if (
        not (0 < width <= 4096 and 0 < height <= 4096)
        or depth != 8
        or color not in (2, 6)
        or any((compression, filtering, interlace))
    ):
        raise ValueError("requires non-interlaced RGB/RGBA8 PNG")
    channels = 4 if color == 6 else 3
    stride = width * channels
    expected = height * (stride + 1)
    decoder = zlib.decompressobj()
    raw = decoder.decompress(payload, expected + 1)
    if len(raw) != expected or not decoder.eof:
        raise ValueError("invalid PNG decompressed size")
    result, previous = bytearray(), bytearray(stride)
    for y in range(height):
        offset = y * (stride + 1)
        kind, row = raw[offset], bytearray(raw[offset + 1 : offset + 1 + stride])
        if kind > 4:
            raise ValueError("invalid PNG filter")
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            up, diagonal = previous[x], previous[x - channels] if x >= channels else 0
            if kind == 1:
                row[x] = (row[x] + left) & 255
            elif kind == 2:
                row[x] = (row[x] + up) & 255
            elif kind == 3:
                row[x] = (row[x] + (left + up) // 2) & 255
            elif kind == 4:
                p = left + up - diagonal
                distances = abs(p - left), abs(p - up), abs(p - diagonal)
                predictor = (left, up, diagonal)[distances.index(min(distances))]
                row[x] = (row[x] + predictor) & 255
        if channels == 4:
            result.extend(row)
        else:
            for x in range(0, stride, 3):
                result.extend(row[x : x + 3])
                result.append(255)
        previous = row
    return width, height, bytes(result)


def _rect(region: dict, width: int, height: int) -> tuple[int, int, int, int]:
    rect = region.get("rect", [0, 0, width, height])
    if (
        len(rect) != 4
        or any(type(v) is not int for v in rect)
        or not (0 <= rect[0] < rect[2] <= width and 0 <= rect[1] < rect[3] <= height)
    ):
        raise ValueError("comparison region must be inside the stage")
    return tuple(rect)


def compare_pixels(
    expected: bytes, actual: bytes, width: int, height: int, regions: list[dict] | None = None
) -> dict:
    if len(expected) != width * height * 4 or len(actual) != len(expected):
        raise ValueError("framebuffer dimensions disagree")
    results, changed_all = [], set()
    for region in regions or [{}]:
        left, top, right, bottom = _rect(region, width, height)
        tolerance = region.get("channel_tolerance", 12)
        maximum = region.get("max_changed_ratio", 0.001)
        if not 0 <= tolerance <= 255 or not 0 <= maximum <= 1:
            raise ValueError("invalid visual tolerance")
        changed = []
        maximum_error = 0
        for y in range(top, bottom):
            for x in range(left, right):
                at = (y * width + x) * 4
                error = max(abs(expected[at + c] - actual[at + c]) for c in range(4))
                maximum_error = max(maximum_error, error)
                if error > tolerance:
                    changed.append(y * width + x)
        changed_all.update(changed)
        ratio = len(changed) / ((right - left) * (bottom - top))
        bounds = (
            [
                min(p % width for p in changed),
                min(p // width for p in changed),
                max(p % width for p in changed) + 1,
                max(p // width for p in changed) + 1,
            ]
            if changed
            else None
        )
        results.append(
            {
                "name": region.get("name", "stage"),
                "rect": [left, top, right, bottom],
                "changed_pixels": len(changed),
                "changed_ratio": ratio,
                "bounds": bounds,
                "max_channel_error": maximum_error,
                "passing": ratio <= maximum,
            }
        )
    return {
        "passing": all(r["passing"] for r in results),
        "regions": results,
        "changed_pixels": len(changed_all),
    }


def difference_image(expected: bytes, actual: bytes) -> bytes:
    output = bytearray(len(actual))
    for at in range(0, len(actual), 4):
        error = max(abs(expected[at + c] - actual[at + c]) for c in range(4))
        output[at : at + 4] = (
            bytes((255, 0, 32, 255))
            if error > 12
            else bytes((actual[at] // 4, actual[at + 1] // 4, actual[at + 2] // 4, 255))
        )
    return bytes(output)


def sequence_findings(frames: list[bytes], width: int, height: int, rules: dict) -> list[str]:
    if not frames:
        return ["sequence has no frames"]
    errors, changes, blank, same_run, longest = [], 0, 0, 1, 1
    if "frames" in rules and len(frames) != rules["frames"]:
        errors.append(f"requested {rules['frames']} frames, captured {len(frames)}")
    for i, frame in enumerate(frames):
        if len(frame) != width * height * 4:
            raise ValueError("sequence framebuffer dimensions disagree")
        colors = Counter(frame[p : p + 4] for p in range(0, len(frame), 4))
        blank += max(colors.values()) / (width * height) >= rules.get("blank_color_ratio", 0.999)
        if i:
            diff = compare_pixels(
                frames[i - 1],
                frame,
                width,
                height,
                [
                    {
                        "rect": rules.get("rect", [0, 0, width, height]),
                        "channel_tolerance": rules.get("channel_tolerance", 12),
                        "max_changed_ratio": rules.get("min_motion_ratio", 0.00001),
                    }
                ],
            )
            moved = not diff["passing"]
            changes += moved
            same_run = 1 if moved else same_run + 1
            longest = max(longest, same_run)
    if changes < rules.get("min_changed_frames", 0):
        errors.append(f"frozen sequence: {changes} changing intervals")
    if blank > rules.get("max_blank_frames", len(frames)):
        errors.append(f"unexpected blank frames: {blank}")
    if longest > rules.get("max_identical_run", len(frames)):
        errors.append(f"stale frame run: {longest}")
    return errors


def _spectrum(values: array.array, rate: int, channels: int) -> list[float]:
    """Normalized coarse spectral energy; independent of phase and capture gain."""
    size, frames = 512, len(values) // channels
    energy = [0.0] * 8
    if frames < size:
        return energy
    boundaries = [125, 250, 500, 1000, 2000, 4000, 8000, rate / 2 + 1]
    for sample in range(min(24, max(1, frames // size))):
        start = sample * (frames - size) // max(1, min(24, frames // size) - 1)
        data = [
            complex(
                sum(values[(start + i) * channels + c] for c in range(channels))
                / channels
                * (0.5 - 0.5 * math.cos(2 * math.pi * i / (size - 1)))
            )
            for i in range(size)
        ]
        j = 0
        for i in range(1, size):
            bit = size >> 1
            while j & bit:
                j ^= bit
                bit >>= 1
            j ^= bit
            if i < j:
                data[i], data[j] = data[j], data[i]
        length = 2
        while length <= size:
            angle = -2 * math.pi / length
            root = complex(math.cos(angle), math.sin(angle))
            for first in range(0, size, length):
                phase = 1 + 0j
                for k in range(length // 2):
                    a, b = data[first + k], data[first + k + length // 2] * phase
                    data[first + k], data[first + k + length // 2] = a + b, a - b
                    phase *= root
            length *= 2
        for i in range(1, size // 2):
            band = next(k for k, edge in enumerate(boundaries) if i * rate / size < edge)
            energy[band] += abs(data[i]) ** 2
    total = sum(energy)
    return [value / total for value in energy] if total else energy


def audio_metrics(path: Path, rate: int, channels: int) -> dict:
    if not 0 < rate <= 192000 or channels not in (1, 2):
        raise ValueError("invalid PCM format")
    raw = path.read_bytes()
    if len(raw) % (4 * channels):
        raise ValueError("truncated float PCM")
    values = array.array("f", raw)
    if sys.byteorder != "little":
        values.byteswap()
    if any(not math.isfinite(value) for value in values):
        raise ValueError("non-finite PCM")
    frames = len(values) // channels
    step = max(1, rate // 20) * channels
    envelope = [
        math.sqrt(sum(v * v for v in values[i : i + step]) / len(values[i : i + step]))
        for i in range(0, len(values), step)
    ]
    return {
        "frames": frames,
        "duration": frames / rate,
        "peak": max(map(abs, values), default=0),
        "rms": math.sqrt(sum(v * v for v in values) / len(values)) if values else 0,
        "envelope": envelope,
        "spectrum": _spectrum(values, rate, channels),
    }


def audio_findings(metrics: dict, rules: dict, baseline: dict | None = None) -> list[str]:
    known = {
        "seconds",
        "min_rms",
        "max_rms",
        "max_peak",
        "audible_threshold",
        "min_relative_rms",
        "max_relative_rms",
        "duration_tolerance_seconds",
        "duration_tolerance_ratio",
        "max_spectral_distance",
        "max_envelope_error",
        "comparison",
        "comparison_note",
    }
    if rules.keys() - known:
        raise ValueError(f"unknown audio observation rules: {sorted(rules.keys() - known)}")
    if rules.get("comparison", "content") not in {"content", "exploratory"}:
        raise ValueError("audio comparison must be content or exploratory")
    if rules.get("comparison") == "exploratory" and not rules.get("comparison_note", "").strip():
        raise ValueError("exploratory audio needs an explicit scope note")
    errors = []
    if metrics["frames"] == 0:
        return ["audio tap returned no PCM samples"]
    if metrics["duration"] + rules.get("duration_tolerance_seconds", 0.1) < rules.get("seconds", 0):
        errors.append("requested audio observation is truncated")
    if metrics["rms"] < rules.get("min_rms", 0):
        errors.append("expected audible output is silent or too quiet")
    if metrics["rms"] > rules.get("max_rms", math.inf):
        errors.append("audio is louder than the allowed RMS")
    if metrics["peak"] > rules.get("max_peak", 1.01):
        errors.append("audio exceeds allowed peak")
    if baseline and baseline["rms"] >= rules.get("audible_threshold", 0.001):
        if metrics["rms"] < baseline["rms"] * rules.get("min_relative_rms", 0.1):
            errors.append("audible baseline became silent or substantially quieter")
        if metrics["rms"] > baseline["rms"] * rules.get("max_relative_rms", 10):
            errors.append("audio became substantially louder than baseline")
        if abs(metrics["duration"] - baseline["duration"]) > max(
            rules.get("duration_tolerance_seconds", 0.1),
            baseline["duration"] * rules.get("duration_tolerance_ratio", 0.2),
        ):
            errors.append("audio duration differs from baseline")
        if baseline.get("spectrum") and metrics.get("spectrum"):
            distance = (
                sum(
                    abs(a - b)
                    for a, b in zip(metrics["spectrum"], baseline["spectrum"], strict=True)
                )
                / 2
            )
            if distance > rules.get("max_spectral_distance", 0.35):
                errors.append("audio spectral content differs from baseline")
        if baseline.get("envelope") and metrics.get("envelope"):
            expected, actual = baseline["envelope"], metrics["envelope"]
            # A ScriptProcessor block boundary can move a window by 43 ms.
            # Compare envelopes with at most one 50 ms bin of alignment slack.
            distances = []
            for offset in (-1, 0, 1):
                pairs = [
                    (a, expected[i + offset])
                    for i, a in enumerate(actual)
                    if 0 <= i + offset < len(expected)
                ]
                if pairs:
                    distances.append(
                        sum(abs(a - b) for a, b in pairs) / len(pairs) / max(baseline["rms"], 0.001)
                    )
            if min(distances, default=math.inf) > rules.get("max_envelope_error", 0.5):
                errors.append("audio envelope differs from baseline")
    elif baseline and metrics["rms"] >= rules.get("audible_threshold", 0.001):
        errors.append("unexpected audible output against a silent baseline")
    return errors


def _artifact(directory: Path, name: str) -> Path:
    path = (directory / name).resolve()
    if not path.is_relative_to(directory.resolve()) or not path.is_file():
        raise ValueError("capture artifact is missing or outside its directory")
    return path


def _frame(directory: Path, frame: dict, width: int, height: int) -> bytes:
    if "rgba" in frame:
        data = _artifact(directory, frame["rgba"]).read_bytes()
    else:
        w, h, data = read_png(_artifact(directory, frame["png"]))
        if (w, h) != (width, height):
            raise ValueError("PNG stage dimensions disagree")
    if len(data) != width * height * 4:
        raise ValueError("capture frame dimensions disagree")
    return data


def assess_capture(directory: Path) -> dict:
    capture = json.loads((directory / "capture.json").read_text())
    if capture.get("status") != "captured":
        raise ValueError(capture.get("error", "capture did not complete"))
    width, height = capture["stage"]
    findings = []
    if not capture.get("checkpoints"):
        findings.append("capture reached no checkpoints")
    reached = {c["label"] for c in capture.get("checkpoints", [])}
    for label in capture.get("requested_checkpoints", []):
        if label not in reached:
            findings.append(f"requested checkpoint {label} was not exercised")
    for checkpoint in capture["checkpoints"]:
        rules = checkpoint.get("observation", {})
        frames = [_frame(directory, f, width, height) for f in checkpoint["frames"]]
        findings += [
            f"{checkpoint['label']}: {message}"
            for message in sequence_findings(frames, width, height, rules.get("sequence", {}))
        ]
        if "audio" in rules and "audio" not in checkpoint:
            findings.append(f"{checkpoint['label']}: requested audio observation is missing")
        if "audio" in checkpoint:
            audio = checkpoint["audio"]
            checkpoint["audio_metrics"] = metrics = audio_metrics(
                _artifact(directory, audio["file"]), audio["rate"], audio["channels"]
            )
            findings += [
                f"{checkpoint['label']}: {message}"
                for message in audio_findings(metrics, rules.get("audio", {}))
            ]
    return {
        "status": "unbaselined" if not findings else "failed",
        "findings": findings,
        "capture": capture,
        "timing_status": capture.get("timing_status", "unmeasured"),
        "timing_findings": capture.get("timing_findings", []),
    }


def _verify_original_binding(directory: Path, approved: dict) -> None:
    binding = approved.get("reference_binding", {})
    if (
        approved["capture"].get("provenance", {}).get("kind") != "original-reference-capture"
        or not binding.get("artifacts")
        or not binding.get("reviewed_alignment")
    ):
        raise ValueError("original screenshots require an artifact-bound original-reference import")
    if approved["kind"] != "original-wine":
        raise ValueError("Wine reference artifacts cannot qualify a native Windows baseline")
    for name, expected in binding["artifacts"].items():
        if sha256(_artifact(directory, name)) != expected:
            raise ValueError("original baseline artifact binding changed")
    if approved["capture"] != assess_capture(directory)["capture"]:
        raise ValueError("original baseline capture differs from its bound artifacts")
    metadata = json.loads(_artifact(directory, "reference-provenance.json").read_text())
    if metadata.get("runner", {}).get("name") != "wine-xvfb":
        raise ValueError("original baseline execution environment differs from its binding")


def _original_png(path: Path) -> tuple[int, int, bytes]:
    try:
        return read_png(path)
    except ValueError as error:
        if "requires non-interlaced" not in str(error):
            raise
    # ImageMagick can choose a paletted PNG for a Director 8-bit stage. Decode
    # that lossless source with the project's existing media tool, without
    # scaling, filtering, or color conversion through an image editor.
    width, height = struct.unpack_from(">II", path.read_bytes(), 16)
    result = subprocess.run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-i",
            str(path),
            "-frames:v",
            "1",
            "-f",
            "rawvideo",
            "-pix_fmt",
            "rgba",
            "pipe:1",
        ],
        capture_output=True,
        timeout=30,
        check=True,
    )
    if len(result.stdout) != width * height * 4:
        raise ValueError("original PNG decoder dimensions disagree")
    return width, height, result.stdout


def import_original_reference(
    reference_dir: Path, port_dir: Path, mapping_path: Path, baseline_dir: Path, reviewer: str
) -> dict:
    """Bind original-runner artifacts to an explicitly reviewed port checkpoint map.

    The mapping is a human/agent review assertion, not an inferred synchronization
    result. Synthetic LINGO.INI experiments cannot become gameplay references.
    """
    if not reviewer.strip() or baseline_dir.exists():
        raise ValueError("original import needs a reviewer and a new baseline directory")
    metadata = json.loads(_artifact(reference_dir, "provenance.json").read_text())
    original = json.loads(_artifact(reference_dir, "capture.json").read_text())
    port = json.loads(_artifact(port_dir, "capture.json").read_text())
    mapping = json.loads(mapping_path.read_text())
    if (
        metadata.get("kind") != "original-reference-capture"
        or not original.get("capture_complete")
        or port.get("status") != "captured"
    ):
        raise ValueError("original import needs completed original and port captures")
    if metadata.get("layout", {}).get("experiment") or metadata.get("experiment"):
        raise ValueError("synthetic projector experiments are not retail gameplay references")
    if (metadata.get("original_game_unmodified") is False
            or metadata.get("layout", {}).get("compatibility")
            or original.get("compatibility")):
        raise ValueError("modified compatibility captures are not unmodified original references")
    if metadata.get("runner", {}).get("name") != "wine-xvfb":
        raise ValueError("this importer supports verified Wine-runner artifact layouts only")
    source = port.get("provenance", {})
    if not all(
        re.fullmatch(r"[a-f0-9]{64}", str(source.get(key, "")))
        for key in ("source_sha256", "plan_sha256")
    ) or not source.get("game"):
        raise ValueError("port capture lacks an identified source and input plan")
    if metadata.get("source", {}).get("iso_sha256") != source.get(
        "selected_image_sha256", source.get("source_sha256")
    ) or metadata.get("game") != source.get("game"):
        raise ValueError("original reference game or source differs from the port capture")
    replay = _artifact(reference_dir, "replay.json")
    if sha256(replay) != metadata.get("replay", {}).get("sha256"):
        raise ValueError("original replay hash does not match its provenance")
    modules = original.get("loaded_modules", {})
    source_hashes = set(metadata.get("source", {}).get("files", {}).values())
    declared = metadata.get("declared_director_version")
    if type(declared) is not int or declared < 500:
        raise ValueError("original reference lacks a declared Director profile")
    expected_version = declared // 100, declared % 100 // 10
    engines = {}
    for name, module in modules.items():
        name = name.replace("\\", "/").split("/")[-1].lower()
        if name not in {"dirapi.dll", "iml32.dll"} or module.get("sha256") not in source_hashes:
            continue
        versions = [
            re.match(r"^(\d+)\.(\d+)", version) for version in module.get("file_versions", [])
        ]
        if any(match and tuple(map(int, match.groups())) == expected_version for match in versions):
            engines[name] = module
    if set(engines) != {"dirapi.dll", "iml32.dll"}:
        raise ValueError(
            "original reference requires mapped DIRAPI/IML32 hashes and versions "
            "matching the selected Director profile; launcher versions are insufficient"
        )
    if (
        mapping.get("schema_version") != 1
        or mapping.get("reviewed_alignment") is not True
        or not mapping.get("alignment_note", "").strip()
        or mapping.get("port_plan_sha256") != source.get("plan_sha256")
    ):
        raise ValueError("original checkpoint alignment must explicitly review the exact port plan")
    width, height = port["stage"]
    by_id = {item["id"]: item for item in original.get("checkpoints", [])}
    if len(by_id) != len(original.get("checkpoints", [])):
        raise ValueError("original checkpoint identifiers are ambiguous")
    selected = mapping.get("checkpoints", {})
    if set(selected) != {c["label"] for c in port["checkpoints"]}:
        raise ValueError("original mapping must cover every selected port checkpoint")
    assembled, artifacts = [], {}
    # Verify and assemble every artifact before creating a baseline directory.
    for checkpoint in port["checkpoints"]:
        label, frames = checkpoint["label"], []
        if not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", label):
            raise ValueError("unsafe original checkpoint label")
        alignment = selected[label]
        entries = alignment.get("frames", [])
        if len(entries) != len(checkpoint["frames"]):
            raise ValueError("original mapping must cover every requested sequence frame")
        for index, entry in enumerate(entries):
            record = by_id.get(entry.get("reference_id"))
            if record is None or entry.get("port_sample") != index:
                raise ValueError("original frame mapping is absent, reordered or ambiguous")
            image = _artifact(reference_dir, record["path"])
            if sha256(image) != record.get("sha256"):
                raise ValueError("original checkpoint image hash differs from the capture record")
            w, h, pixels = _original_png(image)
            left, top, right, bottom = _rect({"rect": entry.get("crop", [0, 0, w, h])}, w, h)
            if (right - left, bottom - top) != (width, height):
                raise ValueError("original stage crop dimensions differ from the port stage")
            pixels = b"".join(
                pixels[(y * w + left) * 4 : (y * w + right) * 4] for y in range(top, bottom)
            )
            name = f"{label}-{index:03d}.rgba"
            artifacts[name] = pixels
            frames.append(
                {
                    "rgba": name,
                    "png": f"{label}-{index:03d}.png",
                    "reference_id": record["id"],
                    "at_ms": record["at_ms"],
                }
            )
        item = {"label": label, "frames": frames, "observation": checkpoint.get("observation", {})}
        if checkpoint.get("audio") or item["observation"].get("audio"):
            import wave

            audio_map = alignment.get("audio")
            if not audio_map:
                raise ValueError("original mapping omitted a requested audio observation")
            wav_path = _artifact(reference_dir, "original.wav")
            if sha256(wav_path) != original.get("media", {}).get("original.wav"):
                raise ValueError("original PCM hash differs from its capture record")
            with wave.open(str(wav_path)) as wav:
                if wav.getsampwidth() != 2 or wav.getnchannels() not in (1, 2):
                    raise ValueError("original reference requires PCM16 mono/stereo")
                rate, channels = wav.getframerate(), wav.getnchannels()
                start = round(audio_map["at_ms"] * rate / 1000)
                count = round(audio_map["duration_ms"] * rate / 1000)
                if start < 0 or count <= 0 or start + count > wav.getnframes():
                    raise ValueError("original audio mapping is outside the recorded PCM")
                wav.setpos(start)
                samples = array.array("h", wav.readframes(count))
                if sys.byteorder != "little":
                    samples.byteswap()
                pcm = array.array("f", (sample / 32768 for sample in samples))
                if sys.byteorder != "little":
                    pcm.byteswap()
                artifacts[f"{label}.f32"] = pcm.tobytes()
                item["audio"] = {"file": f"{label}.f32", "rate": rate, "channels": channels}
        assembled.append(item)
    baseline_dir.mkdir(parents=True)
    for name, data in artifacts.items():
        (baseline_dir / name).write_bytes(data)
        if name.endswith(".rgba"):
            write_png(baseline_dir / name.replace(".rgba", ".png"), width, height, data)
    for name, data in (
        ("reference-provenance.json", metadata),
        ("reference-capture.json", original),
        ("alignment.json", mapping),
    ):
        (baseline_dir / name).write_text(json.dumps(data, indent=2) + "\n")
    imported = {
        "schema_version": 1,
        "status": "captured",
        "stage": [width, height],
        "environment": metadata["runner"],
        "clock": "original-host-monotonic-ms",
        "checkpoints": assembled,
        "provenance": {
            "kind": "original-reference-capture",
            "game": source["game"],
            "source_sha256": source["source_sha256"],
            "selected_image_sha256": source.get("selected_image_sha256", source["source_sha256"]),
            "plan_sha256": source["plan_sha256"],
            "original_loaded_modules": modules,
            "port_build_used_for_alignment_only": source.get("build_sha256"),
        },
    }
    (baseline_dir / "capture.json").write_text(json.dumps(imported, indent=2) + "\n")
    assessed = assess_capture(baseline_dir)
    if assessed["findings"]:
        raise ValueError(
            "original observation constraints fail: " + "; ".join(assessed["findings"])
        )
    result = {
        "schema_version": 1,
        "kind": "original-wine",
        "reviewed_by": reviewer,
        "capture": assessed["capture"],
        "reference_binding": {
            "reviewed_alignment": True,
            "artifacts": {p.name: sha256(p) for p in baseline_dir.iterdir() if p.is_file()},
        },
    }
    (baseline_dir / "baseline.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def compare_capture(current_dir: Path, baseline_dir: Path, output_dir: Path) -> dict:
    output_dir.mkdir(parents=True, exist_ok=True)
    current = assess_capture(current_dir)
    approved = json.loads((baseline_dir / "baseline.json").read_text())
    baseline = approved["capture"]
    if not approved.get("reviewed_by") or approved.get("kind") not in KINDS:
        raise ValueError("baseline has not been explicitly reviewed")
    if approved["kind"] != "port-regression":
        _verify_original_binding(baseline_dir, approved)
    capture, findings, differences = current["capture"], current["findings"], []
    audio_comparisons = []
    for key in ("stage",):
        if capture[key] != baseline[key]:
            raise ValueError(f"baseline {key} differs")
    if approved["kind"] == "port-regression":
        for key in ("environment", "clock"):
            if capture.get(key) != baseline.get(key):
                raise ValueError(f"baseline {key} differs")
    for key in ("source_sha256", "plan_sha256"):
        if capture.get("provenance", {}).get(key) != baseline.get("provenance", {}).get(key):
            raise ValueError(f"baseline {key} differs")
    actual_checkpoints, expected_checkpoints = capture["checkpoints"], baseline["checkpoints"]
    if [c["label"] for c in actual_checkpoints] != [c["label"] for c in expected_checkpoints]:
        raise ValueError("baseline checkpoint stream differs")
    width, height = capture["stage"]
    gallery = []
    for actual, expected in zip(actual_checkpoints, expected_checkpoints, strict=True):
        label = actual["label"]
        if len(actual["frames"]) != len(expected["frames"]):
            findings.append(f"{label}: baseline frame count differs")
            continue
        for i, (a, e) in enumerate(zip(actual["frames"], expected["frames"], strict=True)):
            a_pixels = _frame(current_dir, a, width, height)
            e_pixels = _frame(baseline_dir, e, width, height)
            result = compare_pixels(
                e_pixels, a_pixels, width, height, expected.get("observation", {}).get("regions")
            )
            result.update(label=label, frame=i)
            differences.append(result)
            expected_png, actual_png = (
                f"{label}-{i:03d}-expected.png",
                f"{label}-{i:03d}-actual.png",
            )
            for directory, frame, pixels, name in (
                (baseline_dir, e, e_pixels, expected_png),
                (current_dir, a, a_pixels, actual_png),
            ):
                if "png" in frame:
                    shutil.copyfile(_artifact(directory, frame["png"]), output_dir / name)
                else:
                    write_png(output_dir / name, width, height, pixels)
            gallery.append(
                f"<article><h2>{html.escape(label)} / {i}: "
                f"{'pass' if result['passing'] else 'difference'}</h2>"
                f'<img alt="Expected" src="{expected_png}">'
                f'<img alt="Actual" src="{actual_png}"></article>'
            )
            if not result["passing"]:
                findings.append(f"{label}/{i}: visual difference")
                path = output_dir / f"{label}-{i:03d}-diff.png"
                write_png(path, width, height, difference_image(e_pixels, a_pixels))
                result["difference_image"] = path.name
        if "audio_metrics" in expected:
            if "audio_metrics" not in actual:
                findings.append(f"{label}: recorded audio is missing")
            else:
                rules = expected.get("observation", {}).get("audio", {})
                if rules.get("comparison") == "exploratory":
                    audio_comparisons.append(
                        {
                            "label": label,
                            "status": "unbaselined",
                            "scope": "exploratory",
                            "note": rules["comparison_note"],
                        }
                    )
                else:
                    audio_errors = audio_findings(
                        actual["audio_metrics"], rules, expected["audio_metrics"]
                    )
                    findings += [f"{label}: {message}" for message in audio_errors]
                    audio_comparisons.append(
                        {
                            "label": label,
                            "status": "failed" if audio_errors else "passing",
                            "scope": "content",
                        }
                    )
    result = {
        "schema_version": 1,
        "status": "passing" if not findings else "failed",
        "findings": findings,
        "differences": differences,
        "baseline_kind": approved["kind"],
        "reviewed_by": approved["reviewed_by"],
        "hardware_qualified": False,
        "original_projector_compared": approved["kind"].startswith("original-"),
        "environment": capture.get("environment"),
        "provenance": capture.get("provenance"),
        "production_pacing_qualified": False,
        "timing_status": capture.get("timing_status", "unmeasured"),
        "timing_findings": capture.get("timing_findings", []),
        "audio_comparisons": audio_comparisons,
        "audio_content_compared": any(a["scope"] == "content" for a in audio_comparisons),
    }
    (output_dir / "comparison.json").write_text(json.dumps(result, indent=2) + "\n")
    (output_dir / "index.html").write_text(
        '<!doctype html><meta charset="utf-8"><title>Visual comparison</title>'
        "<style>body{font:16px sans-serif;background:#eee;color:#111;margin:2rem}"
        "img{width:min(46%,800px);height:auto;margin-right:1rem;image-rendering:pixelated}"
        "article{margin:2rem 0}h2{font-size:1rem}</style>"
        f"<h1>{result['status']}</h1><p>Expected left; actual right. "
        "Only configured regions and observations are judged.</p>" + "".join(gallery) + "\n"
    )
    return result


def approve_capture(
    capture_dir: Path,
    baseline_dir: Path,
    reviewer: str,
    kind: str = "port-regression",
    reference_metadata: dict | None = None,
) -> dict:
    if not reviewer.strip() or kind not in KINDS:
        raise ValueError("baseline requires a reviewer and an explicit reference kind")
    if kind != "port-regression":
        raise ValueError(
            "original screenshots require an artifact-bound original-reference import; "
            "port recordings cannot be relabelled as original output"
        )
    assessment = assess_capture(capture_dir)
    if assessment["findings"]:
        raise ValueError("cannot approve failed observations: " + "; ".join(assessment["findings"]))
    if baseline_dir.exists():
        raise ValueError("baseline already exists; choose a new reviewed baseline directory")
    if baseline_dir.resolve().is_relative_to(capture_dir.resolve()):
        raise ValueError("baseline must be outside the capture directory")
    shutil.copytree(capture_dir, baseline_dir)
    result = {
        "schema_version": 1,
        "kind": kind,
        "reviewed_by": reviewer.strip(),
        "reference_metadata": reference_metadata,
        "capture": assessment["capture"],
    }
    (baseline_dir / "baseline.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def selected_plan(
    game, name: str | None = None, path: Path | None = None, checkpoint: str | None = None
) -> dict:
    if path is None and name is None and (game.directory / "visual.json").is_file():
        path = game.directory / "visual.json"
    if path:
        plan = json.loads(path.read_text())
    else:
        candidates = plans(game)
        plan = next((p for p in candidates if p["name"] == name), None) if name else candidates[0]
        if plan is None:
            raise ValueError(f"unknown journey {name}")
    if not plan.get("steps"):
        raise ValueError(
            "visual replay needs input steps; supply --plan for a legacy Python journey"
        )
    result = json.loads(json.dumps(plan))
    if checkpoint:
        stop = next(
            (i for i, action in enumerate(result["steps"]) if action.get("label") == checkpoint),
            None,
        )
        if stop is None:
            raise ValueError(f"unknown visual checkpoint {checkpoint}")
        result["steps"] = result["steps"][: stop + 1]
        result["scope"] = {
            "ends_at_checkpoint": checkpoint,
            "remaining_journey_steps": len(plan["steps"]) - stop - 1,
        }
        allowed = {action["label"] for action in result["steps"] if "label" in action}
        result["observations"] = {
            key: value for key, value in result.get("observations", {}).items() if key in allowed
        }
    labels = [a["label"] for a in result["steps"] if "label" in a]
    if not labels:
        raise ValueError("visual plan needs named checkpoints")
    for label in labels:
        if not label or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789_-" for c in label):
            raise ValueError("unsafe checkpoint label")
    result.setdefault("observations", {}).setdefault(
        labels[-1],
        {
            "sequence": {"frames": 13, "stride_ticks": 5, "max_blank_frames": 0},
            "audio": {"seconds": 1},
        },
    )
    return result


def capture_game(
    game,
    plan: dict,
    output: Path,
    *,
    fast=False,
    seed=42,
    browser: str | None = None,
    timeout=900,
    site: Path | None = None,
) -> dict:
    site = site or game.dist / "web"
    if not (site / "build.json").is_file():
        raise ValueError("web player missing; build it with director64 web first")
    if not (site / "validation.js").is_file():
        raise ValueError("web player lacks validation hooks; rebuild it with director64 web")
    build = json.loads((site / "build.json").read_text())
    from .web import runtime_inputs

    if build.get("runtime", {}).get("inputs_sha256") != runtime_inputs(game):
        raise ValueError("web runtime inputs are stale or unrecorded; rebuild with director64 web")
    if (
        build.get("game", {}).get("slug") != game.slug
        or build.get("profile", {}).get("source") != game.source["id"]
    ):
        raise ValueError("web build belongs to a different game or edition")
    for name in ("player.js", "validation.js", "audio.js", "video.js"):
        if sha256(site / name) != sha256(game.root / "platforms/web/site" / name):
            raise ValueError(f"web build has stale {name}; rebuild it with director64 web")
    if sha256(site / "director64.wasm") != build["runtime"]["wasm_sha256"]:
        raise ValueError("web runtime does not match its build receipt")
    if sha256(site / build["package"]["file"]) != build["package"]["sha256"]:
        raise ValueError("web package does not match its build receipt")
    output.mkdir(parents=True, exist_ok=True)
    plan_path, options_path = output / "plan.json", output / "options.json"
    plan_path.write_text(json.dumps(plan, indent=2) + "\n")
    options_path.write_text(json.dumps({"fast": fast, "seed": seed, "browser": browser}) + "\n")
    script = game.root / "tools/validation/browser.mjs"
    with (output / "worker.log").open("w") as log:
        process = subprocess.Popen(
            [
                "node",
                str(script),
                str(site.resolve()),
                str(plan_path.resolve()),
                str(output.resolve()),
                str(options_path.resolve()),
            ],
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise ValueError(
                f"browser journey exceeded {timeout}s; replay retained in {output}"
            ) from None
    path = output / "capture.json"
    if not path.is_file():
        raise ValueError(f"browser runner failed; inspect {output / 'worker.log'}")
    report = json.loads(path.read_text())
    report["provenance"] = {
        "game": game.slug,
        "source_sha256": game.source["sha256"],
        "selected_image_sha256": game.image["sha256"],
        **game.image_identity,
        "plan_sha256": sha256(plan_path),
        "build_sha256": sha256(site / "build.json"),
        "runtime_sha256": {p.name: sha256(p) for p in site.glob("director64.*") if p.is_file()},
        "browser_harness_sha256": sha256(script),
        "seed": seed,
        "host": {"system": platform.system(), "machine": platform.machine()},
        "player_sha256": sha256(site / "player.js"),
        "validation_hook_sha256": sha256(site / "validation.js"),
        "build_revision": build.get("revision"),
        "runtime_inputs_sha256": build["runtime"]["inputs_sha256"],
        "asset_sha256": {
            str(path.relative_to(site)): sha256(path)
            for path in sorted((site / "assets").glob("*"))
            if path.is_file()
        },
    }
    path.write_text(json.dumps(report, indent=2) + "\n")
    if process.returncode or report.get("status") != "captured":
        raise ValueError(report.get("error", "browser capture failed"))
    return report


def main(game, argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="director64 visual")
    parser.add_argument(
        "operation",
        nargs="?",
        choices=("capture", "compare", "approve", "import-original"),
        default="capture",
    )
    parser.add_argument("--journey")
    parser.add_argument("--checkpoint", help="observe this checkpoint and stop before later input")
    parser.add_argument("--plan", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--recording", type=Path)
    parser.add_argument(
        "--reference", type=Path, help="completed original-runner capture directory"
    )
    parser.add_argument(
        "--alignment", type=Path, help="explicit reviewed original checkpoint mapping"
    )
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--reviewed-by")
    parser.add_argument("--reference-kind", choices=sorted(KINDS), default="port-regression")
    parser.add_argument("--reference-metadata", type=Path)
    parser.add_argument(
        "--fast", action="store_true", help="accelerated route; final observations remain realtime"
    )
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--browser")
    parser.add_argument("--timeout", type=float, default=900)
    args = parser.parse_args(argv)
    output = args.output or game.work / "visual/current"
    try:
        local = game.root / "build"
        for path in (output, args.recording, args.baseline, args.reference):
            if path is not None and not path.resolve().is_relative_to(local.resolve()):
                raise ValueError(
                    "visual captures, comparisons and baselines must stay under build/"
                )
        if args.operation == "import-original":
            if not all(
                (args.recording, args.reference, args.alignment, args.baseline, args.reviewed_by)
            ):
                raise ValueError(
                    "import-original needs --recording, --reference, --alignment, "
                    "--baseline and --reviewed-by"
                )
            result = import_original_reference(
                args.reference, args.recording, args.alignment, args.baseline, args.reviewed_by
            )
            print(
                json.dumps(
                    {"status": "approved", "kind": result["kind"], "reviewed_alignment": True}
                )
            )
            return 0
        if args.operation == "compare" and not args.baseline:
            raise ValueError("compare needs --baseline")
        if args.operation == "approve":
            if not args.recording or not args.baseline or not args.reviewed_by:
                raise ValueError("approve needs --recording, --baseline and --reviewed-by")
            metadata = (
                json.loads(args.reference_metadata.read_text()) if args.reference_metadata else None
            )
            result = approve_capture(
                args.recording, args.baseline, args.reviewed_by, args.reference_kind, metadata
            )
            print(json.dumps({"status": "approved", "kind": result["kind"]}))
            return 0
        recording = args.recording or output
        if args.operation == "capture":
            if args.recording:
                raise ValueError("--recording is for compare or approve, not capture")
            capture_game(
                game,
                selected_plan(game, args.journey, args.plan, args.checkpoint),
                output,
                fast=args.fast,
                seed=args.seed,
                browser=args.browser,
                timeout=args.timeout,
            )
        if args.baseline:
            result = compare_capture(recording, args.baseline, output / "comparison")
        else:
            result = assess_capture(recording)
            (output / "assessment.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps({k: v for k, v in result.items() if k not in ("capture", "differences")}))
        return int(result["status"] == "failed")
    except (
        OSError,
        ValueError,
        KeyError,
        TypeError,
        IndexError,
        struct.error,
        zlib.error,
        subprocess.SubprocessError,
    ) as error:
        parser.exit(1, f"visual validation: {error}\n")

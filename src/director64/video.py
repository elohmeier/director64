"""Convert authored MPEG Xtras for the N64 video decoder.

The shared converter resolves members and timing. The pinned SDK converts the
picture stream to seekable H.264; FFmpeg writes aligned PCM for audioconv64.
"""

from __future__ import annotations

import hashlib
import json
import subprocess
from concurrent.futures import ThreadPoolExecutor

from .toolchain import container_engine, inspect_image

MPEG_PORTS = frozenset(
    {"loewenzahn-6", "loewenzahn-7", "loewenzahn-8", "loewenzahn-optik", "loewenzahn-advent"}
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(game, image):
    output = game.work / "director"
    model_path = output / "model.json"
    media = game.work / "extracted" / game.data["port"]["media_root"]
    sdk = inspect_image(game.root, image)
    aot = game.root / "compiler/target/release/director64-aot"
    cache_root = output / "media-cache"
    for folder in (cache_root, output / "video", output / "wav"):
        folder.mkdir(parents=True, exist_ok=True)
    converter = cache_root / "videoconv64"
    converter.write_bytes(
        subprocess.check_output(
            [
                container_engine(),
                "run",
                "--rm",
                "--network=none",
                sdk["Id"],
                "cat",
                "/n64_toolchain/bin/videoconv64",
            ]
        )
    )
    converter.chmod(0o755)
    provenance = {
        "toolchain": sdk["Id"],
        "converter_sha256": digest(converter),
        "compiler_sha256": digest(aot),
        "quality": 80,
        "seek_seconds": 2,
        "strict_decode": 1,
        "ffmpeg": subprocess.check_output(["ffmpeg", "-version"], text=True).splitlines()[0],
    }

    def convert(path):
        source_hash = digest(path)
        info = json.loads(subprocess.check_output([aot, "director", "mpeg-info", path], text=True))
        # Preserve the authored dimensions and cadence; target runtime tests
        # distinguish decoder support from hardware throughput qualification.
        options = {
            "width": info["width"],
            "height": info["height"],
            "fps": info["rate"] / info["scale"],
        }
        key = hashlib.sha256(
            (source_hash + json.dumps([provenance, options], sort_keys=True)).encode()
        ).hexdigest()
        cache = cache_root / key
        cache.mkdir(exist_ok=True)
        receipt = cache / "receipt.json"
        if receipt.is_file():
            stored = json.loads(receipt.read_text())
            if all(
                (output / p).is_file() and digest(output / p) == sha
                for p, sha in stored["outputs"].items()
            ):
                return stored
        with (cache / "video.log").open("w") as log:
            # Input counts alone cannot detect a damaged slice whose decoder
            # substitutes pixels. Refuse that concealment before transcoding.
            subprocess.run(
                [
                    "ffmpeg",
                    "-hide_banner",
                    "-loglevel",
                    "error",
                    "-xerror",
                    "-err_detect",
                    "explode",
                    "-i",
                    path,
                    "-map",
                    "0:v:0",
                    "-an",
                    "-f",
                    "null",
                    "-",
                ],
                check=True,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            subprocess.run(
                [
                    converter,
                    "-c",
                    "h264",
                    "-w",
                    str(options["width"]),
                    "-r",
                    str(options["fps"]),
                    "-q",
                    "80",
                    "--profile",
                    "none",
                    "--deinterlace",
                    "off",
                    "--ffmpeg-opts",
                    f"-vf scale={info['width']}:{info['height']},setsar=1,"
                    f"fps={options['fps']}:eof_action=pass,format=yuv420p",
                    "--seek",
                    "2",
                    "--no-audio",
                    "--no-progress",
                    "-o",
                    cache,
                    path,
                ],
                check=True,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
        encoded = cache / (path.stem + ".h264")
        probe = json.loads(
            subprocess.check_output(
                [
                    "ffprobe",
                    "-v",
                    "error",
                    "-count_frames",
                    "-select_streams",
                    "v:0",
                    "-show_entries",
                    "stream=width,height,nb_read_frames,avg_frame_rate",
                    "-of",
                    "json",
                    encoded,
                ],
                text=True,
            )
        )["streams"][0]
        if (probe["width"], probe["height"], int(probe["nb_read_frames"])) != (
            info["width"],
            info["height"],
            info["decoded_frames"],
        ):
            raise ValueError(f"N64 video conversion changed geometry/frame count: {path}: {probe}")
        record = {
            "source": path.relative_to(media).as_posix(),
            "source_sha256": source_hash,
            "source_bytes": path.stat().st_size,
            "duration": info["duration_seconds"],
            "video": source_hash[:24] + ".h264",
            "audio": "",
            "videoFrameRate": options["fps"],
        }
        outputs = {}
        for ext in ("h264", "seek"):
            target = output / "video" / (source_hash[:24] + "." + ext)
            target.write_bytes((cache / (path.stem + "." + ext)).read_bytes())
            outputs[target.relative_to(output).as_posix()] = digest(target)
        if info.get("audio_frames", 0) > 0 and info.get("audio_rate", 0) > 0:
            lead = info.get("audio_lead_seconds") or 0
            align = (
                f"atrim=start={lead},asetpts=PTS-STARTPTS"
                if lead >= 0
                else f"adelay={-lead * 1000}:all=1"
            )
            wav = cache / "audio.wav"
            subprocess.run(
                [
                    "ffmpeg",
                    "-v",
                    "error",
                    "-nostdin",
                    "-y",
                    "-xerror",
                    "-err_detect",
                    "explode",
                    "-i",
                    path,
                    "-vn",
                    "-map",
                    "0:a:0",
                    "-af",
                    align,
                    "-c:a",
                    "pcm_s16le",
                    wav,
                ],
                check=True,
            )
            target = output / "wav" / (digest(wav)[:24] + ".wav")
            target.write_bytes(wav.read_bytes())
            outputs[target.relative_to(output).as_posix()] = digest(target)
            record["audio"] = target.with_suffix(".wav64").name
        stored = {
            "record": record,
            "info": info,
            "provenance": provenance,
            "options": options,
            "outputs": outputs,
        }
        receipt.write_text(json.dumps(stored) + "\n")
        return stored

    sources = sorted(media.rglob("*.MPG"))
    with ThreadPoolExecutor(max_workers=4) as pool:
        converted = list(pool.map(convert, sources))
    records = cache_root / "records.json"
    records.write_text(json.dumps([item["record"] for item in converted]) + "\n")
    # port-model's font argument is unused by these ports; retain its existing
    # CLI contract without adding a second member-binding implementation.
    font = game.root / "third_party/libdragon/examples/fontgallery/assets/droid-sans.ttf"
    result = subprocess.check_output(
        [aot, "director", "port-model", model_path, game.slug, font, "0", f"external={records}"],
        text=True,
    )
    model_path.write_text(result)
    (game.work / "analysis/external-media.json").write_text(json.dumps(converted, indent=2) + "\n")

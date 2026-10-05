"""Convert every linked movie and external AIFF from the selected local disc.

The sounds and the model changes run in Rust (director64-aot), as they do in
the browser importer; only the console's H.264 video uses videoconv64."""

import hashlib
import json
import os
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor

from director64.project import selected_game
from director64.toolchain import container_engine, inspect_image


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    game = selected_game()
    model_path = game.work / "director/model.json"
    model = json.loads(model_path.read_text())
    if any(p["error"] != "digital video requires target conversion" for p in model["problems"]):
        raise ValueError("unresolved non-video resources")
    root = game.work / "extracted/MEDIA"
    output = game.work / "director"
    for name in ("video", "wav", "media-cache"):
        (output / name).mkdir(exist_ok=True)
    image = inspect_image(
        game.root, os.environ.get("DIRECTOR64_TOOLCHAIN_IMAGE", "director64-toolchain:local")
    )
    # Execute the exact pinned host converter with local FFmpeg. The container
    # intentionally has no system FFmpeg; guest libraries remain untouched.
    converter = output / "media-cache/videoconv64"
    binary = subprocess.check_output(
        [
            container_engine(),
            "run",
            "--rm",
            "--network=none",
            image["Id"],
            "cat",
            "/n64_toolchain/bin/videoconv64",
        ]
    )
    converter.write_bytes(binary)
    converter.chmod(0o755)
    options = {"codec": "h264", "width": 160, "fps": 15, "quality": 25, "seek_seconds": 2}
    provenance = {
        "toolchain": image["Id"],
        "converter_sha256": sha(converter),
        "options": options,
        "ffmpeg": subprocess.check_output(["ffmpeg", "-version"], text=True).splitlines()[0],
    }
    sources = sorted(p for p in root.rglob("*") if p.suffix in (".MOV", ".AIF"))
    if (
        len([p for p in sources if p.suffix == ".MOV"]) != 66
        or len([p for p in sources if p.suffix == ".AIF"]) != 219
    ):
        raise ValueError("external media denominator changed")
    aot = game.root / "compiler/target/release/director64-aot"

    def convert(path):
        """One source's converted media. The sound and the record are what the
        browser importer reproduces from the same disc with the same Rust
        code (compiler/src/convert/quicktime.rs, ports.rs); only the console's
        H.264 video comes from videoconv64."""
        source_hash = sha(path)
        identity = hashlib.sha256(
            source_hash.encode()
            + json.dumps(provenance, sort_keys=True).encode()
            + sha(aot).encode()
        ).hexdigest()
        cache = output / "media-cache" / identity
        cache.mkdir(exist_ok=True)
        receipt = cache / "receipt.json"
        if receipt.exists():
            stored = json.loads(receipt.read_text())
            if all(
                (output / f).is_file() and sha(output / f) == digest
                for f, digest in stored["outputs"].items()
            ):
                return stored
        record = {
            "source": path.relative_to(root).as_posix(),
            "source_sha256": source_hash,
            "source_bytes": path.stat().st_size,
            "video": "",
            "audio": "",
        }
        analysis = {"provenance": provenance, "outputs": {}}
        wav = cache / "audio.wav"
        wav.unlink(missing_ok=True)
        if path.suffix == ".MOV":
            info = json.loads(
                subprocess.check_output([aot, "director", "mov-info", path], text=True)
            )
            record["duration"] = info["duration_seconds"]
            analysis["tracks"] = info["tracks"]
            if any(t["kind"] == "video" for t in info["tracks"]):
                with (cache / "video.log").open("w") as log:
                    subprocess.run(
                        [
                            str(converter),
                            "-c",
                            "h264",
                            "-w",
                            "160",
                            "-r",
                            "15",
                            "-q",
                            "25",
                            "--seek",
                            "2",
                            "--no-audio",
                            "--no-progress",
                            "-o",
                            str(cache),
                            str(path),
                        ],
                        check=True,
                        stdout=log,
                        stderr=subprocess.STDOUT,
                    )
                # Named by the source, so the browser's own conversion of
                # the same movie carries the same asset name.
                name = source_hash[:24] + ".h264"
                shutil.copyfile(cache / (path.stem + ".h264"), output / "video" / name)
                shutil.copyfile(
                    cache / (path.stem + ".seek"), output / "video" / name.replace(".h264", ".seek")
                )
                record["video"] = name
                for ext in (".h264", ".seek"):
                    relative = "video/" + name.replace(".h264", ext)
                    analysis["outputs"][relative] = sha(output / relative)
            subprocess.run(
                [aot, "director", "mov-audio", path, wav], check=True, stdout=subprocess.DEVNULL
            )
        else:
            subprocess.run(
                [aot, "director", "aiff-wav", path, wav], check=True, stdout=subprocess.DEVNULL
            )
        if wav.exists():
            name = sha(wav)[:24] + ".wav"
            shutil.copyfile(wav, output / "wav" / name)
            import wave

            with wave.open(str(wav)) as sound:
                record.update(
                    frames=sound.getnframes(),
                    rate=sound.getframerate(),
                    channels=sound.getnchannels(),
                )
            if path.suffix == ".AIF":
                record["duration"] = record["frames"] / record["rate"]
            record["audio"] = name.replace(".wav", ".wav64")
            analysis["outputs"]["wav/" + name] = sha(wav)
        stored = {"record": record, **analysis}
        stored["outputs"] = analysis["outputs"]
        receipt.write_text(json.dumps(stored) + "\n")
        return stored

    with ThreadPoolExecutor(max_workers=4) as pool:
        converted = list(pool.map(convert, sources))
    records = [c["record"] for c in converted]
    records_path = output / "media-cache/records.json"
    records_path.write_text(json.dumps(records) + "\n")
    # Geneva is a source system font, absent from the disc: the pinned SDK's
    # Droid Sans stands in (compiler/src/ports.rs loewenzahn::media).
    font_source = game.root / "third_party/libdragon/examples/fontgallery/assets/droid-sans.ttf"
    shutil.copyfile(font_source, output / "fonts/d5-system.ttf")
    model = json.loads(
        subprocess.check_output(
            [
                aot,
                "director",
                "port-model",
                model_path,
                game.slug,
                font_source,
                "0",
                f"external={records_path}",
            ],
            text=True,
        )
    )
    model_path.write_text(json.dumps(model) + "\n")
    links = [
        {
            "movie": movie["name"],
            "cast": m["cast"],
            "member": m["number"],
            "source": m["videoSource"],
        }
        for movie in model["movies"]
        for m in movie["members"]
        if m["type"] == 10
    ]
    from .printing import prepare_qr

    prepare_qr(game, model)
    (game.work / "analysis/external-media.json").write_text(
        json.dumps({"records": converted, "links": links}, indent=2) + "\n"
    )
    print(f"Converted {len(records)} external media files; resolved {len(links)} video members")

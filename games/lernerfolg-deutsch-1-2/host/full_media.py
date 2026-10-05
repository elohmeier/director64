"""Convert the external speech and effects files to ULC wav64 streams.

The disc stores 853 of the ``.aif`` files as MP3 (one with an ID3 tag) and
three as WAV; the container is selected by magic bytes, never by extension.
ULC is the measured selection: 17.4 MiB of speech against 77.6 MiB projected
for VADPCM and 33.5 MiB for Opus.
"""

import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from director64.full_assets import wav_resample_hz
from director64.project import selected_game
from director64.toolchain import container_engine

from .text_metrics import measure


def font_metrics(game, model) -> dict:
    """Measured advance tables for every authored (font, size) in use.

    Text-member styles use them for charPosOf highlight boxes; Flash edit
    fields use them for prompt wrapping and textHeight. Both surfaces read
    the same mkfont build that renders the glyphs.
    """
    fonts = {f["number"]: f for f in model.get("fonts", [])}
    aliases = {}
    for font in fonts.values():
        for alias in font.get("aliases", []):
            aliases[alias.casefold()] = font["number"]
    pairs = set()
    for movie in model["movies"]:
        for member in movie["members"]:
            for key in ("textStyle", "textInsertStyle"):
                style = member.get(key)
                if style and style.get("fontId"):
                    pairs.add((style["fontId"], style["size"]))
            for field in member.get("flashFields", []):
                number = aliases.get((field.get("fontName") or "").casefold())
                if number:
                    pairs.add((number, max(1, round(field.get("fontHeight") or 12))))
    metrics = {}
    for number, size in sorted(pairs):
        font = fonts[number]
        metrics[f"{number}:{size}"] = measure(
            game,
            game.work / "director" / font["asset"],
            font["codepoints"],
            size,
        )
    return metrics


def container(name: Path) -> str:
    head = name.open("rb").read(4)
    if head == b"FORM":
        return ".aiff"
    if head == b"RIFF":
        return ".wav"
    if head[:2] in (b"\xff\xf3", b"\xff\xfb") or head[:3] == b"ID3":
        return ".mp3"
    raise ValueError(f"unrecognized audio container: {name.name}")


def main():
    game = selected_game()
    image = os.environ.get("DIRECTOR64_TOOLCHAIN_IMAGE", "director64-toolchain:local")
    # The speech streams answer to the same policy knob as the packed sounds,
    # and the recorded flags are part of what makes a converted set current.
    flags = ["--wav-compress", "2"]
    if rate := wav_resample_hz(game.root):
        flags += ["--wav-resample", str(rate)]
    for folder in ("voc", "sfx"):
        source = game.work / "extracted" / folder
        target = game.work / "filesystem" / folder
        # Outside filesystem/: everything under it is packed into DragonFS.
        receipt = game.work / f"{folder}-audioconv.flags"
        files = sorted(p for p in source.iterdir() if p.is_file())
        expected = {p.stem.lower() + ".wav64" for p in files}
        if (
            target.is_dir()
            and {q.name for q in target.glob("*.wav64")} == expected
            and receipt.is_file()
            and receipt.read_text() == " ".join(flags)
        ):
            continue
        staging = Path(tempfile.mkdtemp(prefix=f"{folder}-", dir=game.work))
        listing = b""
        for p in files:
            # Lowercase stems make the stream set case-insensitive, as the
            # authored Windows paths were (sfx\silence.aif vs. Silence.aif).
            alias = staging / (p.stem.lower() + container(p))
            alias.symlink_to(Path(os.path.relpath(p, staging)))
            listing += str(alias.relative_to(game.root)).encode() + b"\0"
        (staging / "sources.list").write_bytes(listing)
        (staging / "out").mkdir()
        relative = staging.relative_to(game.root).as_posix()
        subprocess.run(
            [
                container_engine(),
                "run",
                "--rm",
                "--network=none",
                "--volume",
                f"{game.root}:/workdir",
                "--workdir",
                "/workdir",
                image,
                "sh",
                "-c",
                f"xargs -r -0 -a {relative}/sources.list -n 1 -P 8 "
                f"audioconv64 {' '.join(flags)} -o {relative}/out",
            ],
            check=True,
        )
        converted = sorted((staging / "out").glob("*.wav64"))
        if len(converted) != len(files):
            raise ValueError(f"{folder}: {len(converted)} of {len(files)} files converted")
        target.mkdir(parents=True, exist_ok=True)
        for p in converted:
            p.replace(target / p.name)
        receipt.write_text(" ".join(flags))
    databases = stage_databases(game)
    path = game.work / "director/model.json"
    model = json.loads(path.read_text())
    model["fontMetrics"] = font_metrics(game, model)
    notes = [
        {
            "kind": "external-speech-ulc-stream",
            "description": "voc/ and sfx/ files stream as ULC wav64 through sound playFile; "
            "the codec is selected by measured size against VADPCM and Opus.",
            "implemented": True,
        },
        {
            "kind": "exercise-task-databases",
            "description": f"{databases} tests_db/ task databases ship verbatim and are read "
            "through baReadBinFile; the authored Lingo parses them unchanged.",
            "implemented": True,
        },
        {
            "kind": "measured-text-metrics",
            "description": f"{len(model['fontMetrics'])} authored font variants carry mkfont-"
            "measured advance tables for charPosOf and Flash prompt layout.",
            "implemented": True,
        },
    ]
    for note in notes:
        if note not in model["approximations"]:
            model["approximations"].append(note)
    path.write_text(json.dumps(model) + "\n")


def stage_databases(game) -> int:
    """Copy the read-only exercise databases into the ROM filesystem."""
    source = game.work / "extracted/tests_db"
    target = game.work / "filesystem/tests_db"
    files = sorted(p for p in source.rglob("*") if p.is_file())
    for p in files:
        packed = target / p.relative_to(source)
        if packed.exists() and packed.stat().st_size == p.stat().st_size:
            continue
        packed.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(p, packed)
    return len(files)

"""Converting a game from the user's disc image into its package and asset
packs, for the Android TV APK and the web player alike (docs/architecture.md).

    build/<slug>/<source>/converted/   game.d64p, {images,audio,fonts,streams,
                                       data,video,print}.{bin,json}, manifest.json

Every stage is the Rust converter's (compiler/, `director64-aot`), run on the
host, except the Director file parse, which is ProjectorRays under Node
(tools/director/dump.mjs). FFmpeg encodes long sounds as Opus and video as
VP9; images pack as lossless WebP where that is smaller (tools/convert/
webp_tool.c), and the ports that measure their fonts do so with
tools/convert/font_metrics.c. The manifest is written last, so a directory without one
is incomplete.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import time
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from .project import GameSpec

AOT = Path("compiler/target/release/director64-aot")
TOOLS = Path("build/tools")
PARSER = Path("node_modules/projectorrays/dist")
PARSER_WASM_SHA256 = "e4d7ab15a9cb20586d0e5afc54ad4d48412254d98b78a0c834b5adc8e2e3983e"
# The ports' pinned substitute for unembedded system fonts (Apache-2.0).
DROID_SANS = Path("third_party/fonts/droid-sans.ttf")
WEBP = Path("third_party/libwebp")
FREETYPE = Path("third_party/freetype")
PACKS = ("images", "audio", "fonts", "streams", "data", "video", "print")
GAME_FILES = ("game.d64p", *(f"{kind}.{ext}" for kind in PACKS for ext in ("bin", "json")))
# libwebp lossless method 2 at quality 50: within 5% of its smallest output at
# a fiftieth of the time.
WEBP_METHOD, WEBP_QUALITY = 2, 50
# A sound of OPUS_SECONDS or more (soundtracks, long speech) goes to Ogg Opus,
# which the players decode in the background; shorter ones stay WAV so they
# start at once.
OPUS_SECONDS = 8
# VP9 profile 0 at constrained quality: CRF 30 up to 600 kbit/s, which on
# Löwenzahn's noisiest MPEG-1 clip scores above VP8 at 900 kbit/s (SSIM 0.960
# against 0.957) at 61% of its size. One coded frame per input frame (no
# hidden alternate references), as the players' decoders expect.
VP9_CODEC = "vp09.00.10.08"
VP9 = ["-c:v", "libvpx-vp9", "-crf", "30", "-b:v", "600k", "-row-mt", "1", "-deadline", "good",
       "-cpu-used", "2", "-auto-alt-ref", "0", "-lag-in-frames", "0"]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def jobs() -> int:
    return max(1, min(os.cpu_count() or 4, 16))


# ---- The converter's tools ----


def tree_digest(root: Path, paths: list[str]) -> str:
    digest = hashlib.sha256()
    files = set()
    for name in paths:
        path = root / name
        if path.is_dir():
            files.update(p for p in path.rglob("*") if p.is_file())
        elif path.is_file():
            files.add(path)
    for path in sorted(files):
        digest.update(path.relative_to(root).as_posix().encode() + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def built(root: Path, name: str, sources: list[str], build) -> Path:
    """A host tool under build/tools/<name>, rebuilt when its sources change."""
    folder = root / TOOLS / name
    stamp = folder / "sources.sha256"
    identity = tree_digest(root, sources)
    if not (stamp.is_file() and stamp.read_text() == identity):
        if folder.exists():
            shutil.rmtree(folder)
        folder.mkdir(parents=True)
        build(folder)
        stamp.write_text(identity)
    return folder


def webp_tool(root: Path) -> Path:
    """The image packer (tools/convert/webp_tool.c) with the vendored libwebp."""
    sources = [
        str(p.relative_to(root))
        for folder in ("src/dec", "src/dsp", "src/enc", "src/utils", "sharpyuv")
        for p in sorted((root / WEBP / folder).glob("*.c"))
        if not any(tag in p.name for tag in ("_mips", "_msa", "_neon"))
    ]

    def build(folder: Path) -> None:
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=gnu11", "-O2", f"-I{WEBP}", "-Iruntime/director",
             "-Iplatforms/web", *sources, "platforms/web/webp_image.c",
             "tools/convert/webp_tool.c", "-lm", "-lpthread", "-o", folder / "d64-webp"],
            cwd=root, check=True,
        )

    inputs = [str(WEBP / "src"), str(WEBP / "sharpyuv"), "platforms/web/webp_image.c",
              "platforms/web/webp_image.h", "runtime/director/bitmap_tiles.h",
              "tools/convert/webp_tool.c"]
    return built(root, "webp", inputs, build) / "d64-webp"


def metrics_tool(root: Path) -> Path:
    """The font measurer (tools/convert/font_metrics.c) with the vendored
    FreeType."""

    def build(folder: Path) -> None:
        cc = os.environ.get("CC", "cc")
        subprocess.run([cc, "-O2", "-w", f"-I{FREETYPE}", "-c", FREETYPE / "FreeTypeAmalgam.c",
                        "-o", folder / "freetype.o"], cwd=root, check=True)
        subprocess.run([cc, "-std=gnu11", "-O2", f"-I{FREETYPE}", "tools/convert/font_metrics.c",
                        folder / "freetype.o", "-lm", "-o", folder / "font-metrics"],
                       cwd=root, check=True)
        (folder / "freetype.o").unlink()

    return built(root, "font-metrics", [str(FREETYPE), "tools/convert/font_metrics.c"],
                 build) / "font-metrics"


def aot(root: Path) -> Path:
    subprocess.run(["cargo", "build", "--release", "--locked", "--quiet", "--manifest-path",
                    root / "compiler/Cargo.toml"], check=True)
    return root / AOT


def source_digest(root: Path) -> str:
    """Inputs that can change conversion output."""
    return tree_digest(root, [
        "compiler/src", "compiler/Cargo.toml", "compiler/Cargo.lock", "tools/convert",
        "tools/director/dump.mjs", "tools/director/dump-format.mjs",
        "tools/director/afterburner.mjs",
        str(WEBP / "src"), str(FREETYPE),
        "platforms/web/webp_image.c", "src/director64/convert.py",
        "src/director64/system_fonts.py", "runtime/director/director.h",
        "runtime/package/package.h", "runtime/lingo/names.txt",
        "runtime/lingo/lingo_bytecode.h", str(DROID_SANS), "package-lock.json",
    ])


def converter_revision(root: Path) -> str:
    """The conversion's identity: its sources and the host tools that run them."""
    if sha256(root / PARSER / "projectorrays.wasm") != PARSER_WASM_SHA256:
        raise ValueError("unsupported ProjectorRays WASM; restore the locked npm dependency")

    def version(*command) -> str:
        result = subprocess.run(command, capture_output=True, text=True, check=True)
        return (result.stdout or result.stderr).splitlines()[0]

    toolchain = [version("rustc", "-V"), version(os.environ.get("CC", "cc"), "--version"),
                 version(os.environ.get("FFMPEG", "ffmpeg"), "-version")]
    return hashlib.sha256(
        json.dumps([source_digest(root), toolchain]).encode()
    ).hexdigest()


# ---- Stages ----


class Converter:
    """One conversion of `game` into `out`, working under build/<slug>/<source>/."""

    def __init__(self, game: GameSpec, out: Path, meta: dict):
        self.game, self.out, self.meta = game, out, meta
        self.root = game.root
        self.work = game.work / "conversion"
        self.extracted = self.work / "extracted"
        self.table = game.data.get("convert", {})
        self.port_file = self.work / "port.json"
        self.aot = aot(self.root)
        self.report: dict = {"stages": {}}

    def run(self, *args, capture=True, **kwargs) -> str:
        result = subprocess.run([str(a) for a in args], capture_output=capture, text=True,
                                cwd=self.root, **kwargs)
        if result.returncode:
            detail = (result.stderr or "").strip() if capture else ""
            if len(detail) > 4000:
                detail = detail[:2000] + "\n...\n" + detail[-2000:]
            raise ValueError(f"{Path(str(args[0])).name} {args[1] if len(args) > 1 else ''} "
                             f"failed: {detail}")
        return result.stdout if capture else ""

    def director(self, *args) -> str:
        return self.run(self.aot, "director", *args)

    def stage(self, name: str, work):
        print(f"  {name}", file=sys.stderr, flush=True)
        started = time.monotonic()
        result = work()
        self.report["stages"][name] = round((time.monotonic() - started) * 1000)
        return result

    def convert(self) -> None:
        from .media import extract_source

        if self.work.exists():
            shutil.rmtree(self.work)
        self.work.mkdir(parents=True)
        port = self.game.data["port"]
        self.port_file.write_text(json.dumps({
            "slug": self.game.slug, "source_sha256": self.game.source["sha256"],
            "launcher_movie": port.get("launcher_movie"), "convert": self.table,
        }))
        # A conversion extracts its own copy: the policy installs original
        # system fonts into it.
        media = self.stage(
            "extract", lambda: extract_source(self.game, self.extracted) / port["media_root"]
        )
        policy = self.stage("policy", lambda: prepared_policy(self.game, self.work,
                                                              self.extracted))
        analysis = self.work / "analysis"
        dumps = analysis / "dumps"
        self.stage("parse", lambda: self.parse(media, policy, dumps))
        self.stage("analyze", lambda: self.director("analyze", media, dumps, analysis, policy))
        parser = sha256(self.root / PARSER / "projectorrays.wasm")
        self.stage("audit", lambda: self.director("audit", media, dumps, analysis / "source",
                                                  policy, parser))
        self.stage("scores", lambda: self.director("recover", analysis / "source/manifest.json",
                                                   analysis / "score-recovery"))
        director = self.work / "director"
        self.stage("convert", lambda: self.director(
            "compile", director, analysis / "score-recovery", media, policy, dumps,
            *(["--defer-video"] if port.get("defer_video") else [])))
        plan = self.stage("port", lambda: self.port(director))
        program = self.stage("compile", lambda: self.program(analysis))
        packs = self.stage("package", lambda: self.package(program, director, plan))
        self.stage("sound", lambda: self.sounds(packs))
        self.stage("image", lambda: self.images(packs))
        self.stage("video", lambda: self.videos(packs))
        self.stage("write", lambda: self.write(packs))

    def parse(self, media: Path, policy: Path, dumps: Path) -> None:
        """ProjectorRays over every Director file of the plan, largest files
        first across parallel Node processes; each writes its dumps."""
        self.director("plan", media, policy, dumps)
        plan = json.loads((dumps / "plan.json").read_text())
        entries = sorted(plan["dumps"], key=lambda e: -(self.root / e["input"]).stat().st_size
                         if (self.root / e["input"]).is_file() else 0)
        count = min(jobs(), max(1, len(entries)))
        chunks = [entries[i::count] for i in range(count)]
        plans = []
        for i, chunk in enumerate(chunks):
            path = dumps / f"plan-{i}.json"
            path.write_text(json.dumps({**plan, "dumps": chunk}))
            plans.append(path)
        with ThreadPoolExecutor(count) as pool:
            list(pool.map(lambda p: self.run("node", "tools/director/dump.mjs", p), plans))
        for path in plans:
            path.unlink()

    def port(self, director: Path) -> dict:
        """What the port adds (game.toml [convert], compiler/src/ports.rs): model
        corrections with its substitute font and measured metrics, external
        media, and the disc folders the runtime streams or reads as data."""
        model = director / "model.json"
        port = self.port_file
        plan = json.loads(self.director("port-plan", port, model))
        folder = self.work / "port"
        folder.mkdir()
        substitute = folder / "substitute.ttf"
        shutil.copyfile(self.root / DROID_SANS, substitute)
        measured = []
        if plan["measure"]:
            tool = metrics_tool(self.root)
            for n, request in enumerate(plan["measure"]):
                font = request["font"]
                source = substitute if font == "substitute" else director / font
                metrics = folder / f"measure-{n}.json"
                metrics.write_text(self.run(tool, source, request["size"], *request["ranges"]))
                measured.append(f"{request['key']}={metrics}")
        extracted = self.extracted
        data_files = sum(
            1 for name in plan["data"] for p in (extracted / name).rglob("*") if p.is_file()
        )
        arguments = [model, port, substitute, data_files, *measured]
        if plan.get("external_media"):
            records = folder / "external.json"
            records.write_text(json.dumps(self.external_media(plan["external_media"], director)))
            arguments.append(f"external={records}")
        model.write_text(self.director("port-model", *arguments).strip() + "\n")
        return plan

    def external_media(self, spec: dict, director: Path) -> list[dict]:
        """Linked movies and external sounds (Löwenzahn): each source's record,
        its sound as a WAV named by content, and the movies whose video the
        video stage encodes."""
        base = self.extracted / spec["root"]
        sources = sorted(
            p.relative_to(base).as_posix() for p in base.rglob("*")
            if p.is_file() and any(p.name.endswith(f".{e}") for e in spec["extensions"])
        )
        self.video_sources = []
        wavs = director / "wav"
        wavs.mkdir(exist_ok=True)
        records = []
        for path in sources:
            source_path = base / path
            source = sha256(source_path)
            record = {"source": path, "source_sha256": source,
                      "source_bytes": source_path.stat().st_size, "video": "", "audio": ""}
            wav = None
            scratch = self.work / "port/sound.wav"
            if path.endswith(".MPG"):
                info = json.loads(self.director("mpeg-info", source_path))
                record["duration"] = info["duration_seconds"]
                record["width"], record["height"] = info["width"], info["height"]
                record["video"] = f"{source[:24]}.vp8"
                self.video_sources.append({"path": source_path, "name": record["video"],
                                           "mpeg": info})
                wav = mpeg_audio(source_path.read_bytes(), info)
            elif path.endswith(".MOV"):
                info = json.loads(self.director("mov-info", source_path))
                video = next((t for t in info["tracks"] if t["kind"] == "video"), None)
                record["duration"] = info["duration_seconds"]
                record["audioOnly"] = video is None
                if video:
                    record["width"], record["height"] = video["width"], video["height"]
                    record["video"] = f"{source[:24]}.h264"
                    self.video_sources.append({"path": source_path, "name": record["video"],
                                               "width": video["width"],
                                               "height": video["height"]})
                if self.director("mov-audio", source_path, scratch).strip() != "no sound track":
                    wav = scratch.read_bytes()
            else:
                info = json.loads(self.director("aiff-wav", source_path, scratch))
                record["cuePoints"] = info["cuePoints"]
                wav = scratch.read_bytes()
            if wav:
                name = hashlib.sha256(wav).hexdigest()[:24]
                (wavs / f"{name}.wav").write_bytes(wav)
                channels, rate = struct.unpack_from("<HI", wav, 22)
                record["channels"], record["rate"] = channels, rate
                record["frames"] = struct.unpack_from("<I", wav, 40)[0] // (2 * channels)
                if path.endswith(".AIF"):
                    record["duration"] = record["frames"] / rate
                record["audio"] = f"{name}.wav64"
            records.append(record)
        return records

    def program(self, analysis: Path) -> Path:
        """The parsed Lingo program; with the port's patches and the embedded
        projector's launcher handlers when it has them."""
        program = self.work / "aot/program.json"
        self.run(self.aot, "lingo", analysis / "lingo", program)
        check_handlers(json.loads((analysis / "source/manifest.json").read_text()),
                       json.loads(program.read_text()))
        if not self.game.data["port"].get("launcher_movie") and not self.table.get("patch"):
            return program
        native = self.work / "aot/native-program.json"
        native.write_text(self.director("port-program", program,
                                        analysis / "source/manifest.json", self.port_file))
        return native

    def package(self, program: Path, director: Path, plan: dict) -> dict:
        root, extracted = self.root, self.extracted
        meta = self.work / "meta.json"
        meta.write_text(json.dumps(self.meta, sort_keys=True, separators=(",", ":")) + "\n")
        package = self.work / "game.d64p"
        self.run(self.aot, "package", program, director / "model.json", package,
                 "--names", root / "runtime/lingo/names.txt",
                 "--bytecode", root / "runtime/lingo/lingo_bytecode.h", "--meta", meta)
        model = json.loads((director / "model.json").read_text())
        packs = {"package": package}
        images = director / "images"
        paths = sorted(p for p in images.iterdir() if p.is_file())
        with ThreadPoolExecutor(jobs()) as pool:
            packs["images"] = dict(zip((p.name for p in paths),
                                       pool.map(lambda p: deflated(p.read_bytes()), paths),
                                       strict=True))
        # Sound members name their asset .wav64; the players play the WAV (or
        # the Opus the sound stage makes of it).
        packs["audio"] = {p.stem + ".wav64": p.read_bytes()
                          for p in sorted((director / "wav").glob("*.wav"))}
        # The fonts the players draw text with, by the font number styles use.
        packs["fonts"] = {str(f["number"]): (director / f["asset"]).read_bytes()
                          for f in model.get("fonts", [])
                          if f["asset"].endswith((".otf", ".ttf"))}
        # Sound streams for playFile, by "folder/stem" in lowercase, with their
        # durations.
        packs["streams"] = {}
        if plan.get("streams"):
            # A NAME=FOLDER entry streams FOLDER's files under NAME; a folder
            # the disc does not have streams nothing.
            specs = [s.partition("=") if "=" in s else (s, "=", s) for s in plan["streams"]]
            folders = [f"{name}={extracted / folder}" for name, _, folder in specs
                       if (extracted / folder).is_dir()]
            blob = self.work / "streams.bin"
            index = json.loads(self.director("stream-pack", blob, *folders))
            data = blob.read_bytes()
            packs["streams"] = {name: (data[at:at + length], rest)
                                for name, (at, length, *rest) in index.items()}
        # Files the runtime reads by their path on the disc.
        packs["data"] = {
            f"{folder}/{p.relative_to(extracted / folder).as_posix()}": p.read_bytes()
            for folder in plan.get("data", [])
            for p in sorted((extracted / folder).rglob("*")) if p.is_file()
        }
        # Print documents (Löwenzahn): the recovered text and the original
        # artwork, which the web player lays out for the browser's print dialog.
        packs["print"] = {}
        if plan.get("print"):
            blob = self.work / "print.bin"
            index = json.loads(self.director("print-pack", blob, director / "model.json",
                                             extracted / plan["print"]))
            data = blob.read_bytes()
            packs["print"] = {name: data[at:at + length] for name, (at, length) in index.items()}
        packs["video"] = {}
        return packs

    def sounds(self, packs: dict) -> None:
        def encode(item):
            return item[0], opus(item[1])

        with ThreadPoolExecutor(jobs()) as pool:
            packs["audio"] = dict(pool.map(encode, packs["audio"].items()))
            streams = pool.map(lambda item: (item[0], (opus(item[1][0]), item[1][1])),
                               packs["streams"].items())
            packs["streams"] = dict(streams)

    def images(self, packs: dict) -> None:
        """Each image as lossless WebP where that comes out smaller than its
        deflated form; the runtime decodes it back to the stored bytes."""
        tool = webp_tool(self.root)
        scratch = self.work / "webp"
        scratch.mkdir()
        names = list(packs["images"])
        lines = [f"{self.work / 'director/images' / n}\t{scratch / n}" for n in names]
        count = jobs()
        with ThreadPoolExecutor(count) as pool:
            list(pool.map(
                lambda chunk: self.run(tool, "pack", WEBP_METHOD, WEBP_QUALITY,
                                       input="".join(f"{line}\n" for line in chunk)),
                [lines[i::count] for i in range(count)],
            ))
        for name in names:
            packed = scratch / name
            if packed.is_file() and packed.stat().st_size < len(packs["images"][name]):
                packs["images"][name] = packed.read_bytes()
        shutil.rmtree(scratch)

    def videos(self, packs: dict) -> None:
        sources = getattr(self, "video_sources", [])

        def encode(source):
            if "mpeg" in source:
                return source["name"], encode_mpeg(source["path"].read_bytes(), source["mpeg"])
            frames = self.work / f"frames-{source['name']}"
            info = json.loads(self.director("cinepak-frames", source["path"], frames))
            try:
                return source["name"], encode_frames(frames, info)
            finally:
                frames.unlink()

        # Longest first, so no long movie starts last.
        sources = sorted(sources, key=lambda s: -s["path"].stat().st_size)
        with ThreadPoolExecutor(jobs()) as pool:
            packs["video"] = dict(pool.map(encode, sources))
        self.report["videos"] = len(packs["video"])

    def write(self, packs: dict) -> None:
        out = self.out
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)
        shutil.copyfile(packs["package"], out / "game.d64p")
        for kind in PACKS:
            index, offset = {}, 0
            with (out / f"{kind}.bin").open("wb") as blob:
                for name in sorted(packs[kind]):
                    entry = packs[kind][name]
                    data, rest = entry if isinstance(entry, tuple) else (entry, [])
                    blob.write(data)
                    index[name] = [offset, len(data), *rest]
                    offset += len(data)
            (out / f"{kind}.json").write_text(json.dumps(index, separators=(",", ":")))
        self.report.update(
            images=len(packs["images"]), sounds=len(packs["audio"]),
            fonts=len(packs["fonts"]), streams=len(packs["streams"]),
            data_files=len(packs["data"]), package_bytes=(out / "game.d64p").stat().st_size,
        )
        # The scene model and program beside the game, for inspection and the
        # journeys; not shipped.
        shutil.copyfile(self.work / "director/model.json", self.game.work / "model.json")
        program = self.work / "aot/native-program.json"
        if not program.is_file():
            program = self.work / "aot/program.json"
        shutil.copyfile(program, self.game.work / "program.json")


def deflated(data: bytes) -> bytes:
    """A stored image as "D64Z", its length (u32 big-endian) and raw deflate
    (compiler/src/convert/files.rs), which the players inflate on load."""
    pack = zlib.compressobj(1, zlib.DEFLATED, -15)
    return b"D64Z" + struct.pack(">I", len(data)) + pack.compress(data) + pack.flush()


def ffmpeg(args: list, data: bytes | None = None) -> bytes:
    # Decoder recovery is not a qualified repair of a damaged retail source:
    # fail instead of silently exporting it.
    command = [os.environ.get("FFMPEG", "ffmpeg"), "-hide_banner", "-loglevel", "error",
               "-xerror", "-err_detect", "explode", *[str(a) for a in args]]
    result = subprocess.run(command, input=data, capture_output=True, check=False)
    if result.returncode:
        raise ValueError(f"ffmpeg exited with {result.returncode}: "
                         f"{result.stderr.decode(errors='replace').strip()[-2000:]}")
    return result.stdout


def opus(wav: bytes) -> bytes:
    """A WAV of OPUS_SECONDS or more as Ogg Opus; anything else as it is."""
    if len(wav) < 44 or wav[:4] != b"RIFF":
        return wav
    channels = struct.unpack_from("<H", wav, 22)[0]
    per_second = struct.unpack_from("<I", wav, 28)[0]
    if not per_second or (len(wav) - 44) / per_second < OPUS_SECONDS:
        return wav
    return ffmpeg(["-f", "wav", "-i", "pipe:0", "-c:a", "libopus", "-b:a",
                   "96k" if channels > 1 else "64k", "-vbr", "on", "-application", "audio",
                   "-f", "ogg", "pipe:1"], wav)


def keyframe(data: bytes) -> bool:
    """A VP9 frame header: marker 2, profile 0, then show_existing_frame and
    frame_type, which is 0 on a keyframe."""
    return (data[0] & 0xC0) == 0x80 and not data[0] & 0x08 and not data[0] & 0x04


def ivf_frames(data: bytes) -> tuple[int, int, int, int, list[tuple[int, bytes]]]:
    """IVF: a 32-byte header, then per frame a 12-byte header (u32 size, u64
    timestamp) and the frame. Frame times in microseconds."""
    if len(data) < 32 or data[:4] != b"DKIF" or struct.unpack_from("<H", data, 4)[0] != 0:
        raise ValueError("ffmpeg wrote no valid IVF stream")
    header = struct.unpack_from("<H", data, 6)[0]
    width, height, rate, scale = struct.unpack_from("<HHII", data, 12)
    if not (width and height and rate and scale):
        raise ValueError("invalid IVF dimensions or rate")
    frames, at, last = [], header, -1
    while at < len(data):
        size, pts = struct.unpack_from("<IQ", data, at)
        if not size or size > len(data) - at - 12 or pts <= last:
            raise ValueError("invalid IVF frame length or timestamp")
        frames.append((round(pts * scale * 1e6 / rate), data[at + 12:at + 12 + size]))
        last, at = pts, at + 12 + size
    if not frames or not keyframe(frames[0][1]):
        raise ValueError("IVF has no initial keyframe")
    return width, height, rate, scale, frames


def video_container(width: int, height: int, frames: list[tuple[int, bytes]]) -> bytes:
    """The players' video container: a u32 header length, a JSON header
    {codec, width, height, frames: [[microseconds, key, offset, size]]} and the
    frames (platforms/web/site/video.js, platforms/android/jni/video.c)."""
    entries, offset = [], 0
    for time_us, data in frames:
        entries.append([time_us, 1 if keyframe(data) else 0, offset, len(data)])
        offset += len(data)
    header = json.dumps({"codec": VP9_CODEC, "width": width, "height": height,
                         "frames": entries}, separators=(",", ":")).encode()
    return struct.pack("<I", len(header)) + header + b"".join(d for _, d in frames)


def encode_frames(frames: Path, info: dict) -> bytes:
    """A linked movie's decoded Cinepak frames (RGBX) as VP9 at their own times."""
    ivf = ffmpeg(["-f", "rawvideo", "-pix_fmt", "rgb0", "-s", f"{info['width']}x{info['height']}",
                  "-r", "15", "-i", frames, *VP9, "-g", "30", "-fps_mode", "passthrough",
                  "-f", "ivf", "pipe:1"])
    *_, encoded = ivf_frames(ivf)
    times = info["times"]
    if len(encoded) != len(times):
        raise ValueError(f"ffmpeg wrote {len(encoded)} frames for {len(times)}")
    return video_container(info["width"], info["height"],
                           [(t, data) for t, (_, data) in zip(times, encoded, strict=True)])


def encode_mpeg(source: bytes, info: dict) -> bytes:
    """An MPEG-1 movie's picture as VP9 at the source frame rate. Piped program
    streams can omit later PTS; frames retime from display order rather than
    duplicating pictures."""
    ivf = ffmpeg(["-i", "pipe:0", "-an", "-vf", "setpts=N/(FRAME_RATE*TB)", *VP9, "-g", "50",
                  "-fps_mode", "passthrough", "-f", "ivf", "pipe:1"], source)
    width, height, rate, scale, frames = ivf_frames(ivf)
    if ((info.get("width") and width != info["width"])
            or (info.get("height") and height != info["height"])
            or (info.get("decoded_frames") and len(frames) != info["decoded_frames"])
            or (info.get("rate") and abs(rate / scale - info["rate"] / info["scale"]) > 0.0001)):
        raise ValueError(f"MPEG conversion changed geometry, rate or frame count: "
                         f"{width}x{height}, {len(frames)} frames at {rate}/{scale}")
    return video_container(width, height, frames)


def mpeg_audio(source: bytes, info: dict) -> bytes | None:
    """An MPEG movie's sound as a canonical 16-bit WAV, or None without one."""
    lead = info.get("audio_lead_seconds") or 0
    filters = (["-af", f"atrim=start={lead},asetpts=PTS-STARTPTS"] if lead > 0
               else ["-af", f"adelay=delays={-lead * 1000}:all=1"] if lead < 0 else [])
    try:
        wav = ffmpeg(["-i", "pipe:0", "-vn", "-map", "0:a:0", *filters, "-c:a", "pcm_s16le",
                      "-f", "wav", "pipe:1"], source)
    except ValueError as error:
        # Only a missing audio stream is a silent movie.
        if "matches no streams" in str(error):
            return None
        raise
    if len(wav) <= 44:
        return None
    at = 12
    while at + 8 <= len(wav) and wav[at:at + 4] != b"data":
        at += 8 + struct.unpack_from("<I", wav, at + 4)[0]
    if at + 8 > len(wav):
        raise ValueError("ffmpeg wrote no WAV data")
    data = wav[at + 8:]
    fmt = bytearray(wav[12:36])
    struct.pack_into("<I", fmt, 4, 16)
    return b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE" + bytes(fmt) + b"data" + \
        struct.pack("<I", len(data)) + data


def check_handlers(source: dict, program: dict) -> None:
    """The parsed program has exactly the handlers the source bytecode
    declares, by movie, cast, member and name."""
    from collections import Counter

    expected = Counter()
    for file in source["files"]:
        members = {m["id"]: m for m in file["members"]}
        casts = {c["library_id"]: c["name"] for c in file["libraries"]}
        for script in file["scripts"]:
            member = members[script["member_id"]]
            for handler in script["handlers"]:
                expected[(file["name"], casts[member["library_id"]].casefold(),
                          member["member_id"], handler["name"].casefold())] += 1
    actual = Counter(
        (h["movie"], h["cast"].casefold(), h["member"], h["name"]) for h in program["handlers"]
    )
    if expected != actual:
        raise ValueError("recovered AST handler identities differ from source bytecode")


# ---- Policy ----


def prepared_policy(game: GameSpec, work: Path, extracted: Path) -> Path:
    """The game's conversion policy with the disc's proven long names
    (Joliet aliases, relative to the media root) and its system fonts
    installed from the original installers."""
    from .iso import inventory_report

    policy = json.loads((game.directory / "host/source-policy.json").read_text())
    aliases = {}
    if game.source["kind"] == "iso9660":
        with game.media.open("rb") as handle:
            aliases = inventory_report(handle).get("aliases", {})
    root = game.data["port"]["media_root"].strip("/")
    prefix = root + "/" if root else ""
    depth = len(root.split("/")) if root else 0
    policy["source_path_aliases"] = {
        "/".join(long.split("/")[depth:]): short[len(prefix):]
        for long, short in aliases.items() if short.startswith(prefix)
    }
    if policy.get("system_font_resources"):
        from .system_fonts import prepare

        prepare(game.root, extracted / root, policy)
    output = work / "source-policy.json"
    output.write_text(json.dumps(policy, indent=2) + "\n")
    return output


# ---- The conversion, reused while its inputs stay the same ----


def profile_sha256(game: GameSpec) -> str:
    from .web import profile_entry

    return hashlib.sha256(json.dumps(profile_entry(game), sort_keys=True).encode()).hexdigest()


def convert(game: GameSpec, out: Path | None = None, force: bool = False) -> Path:
    """The game converted from its disc image into `out` (by default
    build/<slug>/<source>/converted/), unless a complete conversion by the same
    converter and runtime ABI is there already."""
    from .iso import file_sha256
    from .web import abi_digest, validate_package

    out = out or game.work / "converted"
    media = game.media
    if not media.is_file():
        raise ValueError(f"{media}: place the game's disc image there first")
    identity = game.image_identity
    if identity:
        image = identity["source_image"]
        if media.stat().st_size != image["bytes"] or file_sha256(media) != image["sha256"]:
            raise ValueError(f"{media}: source image differs from the selected repair edition")
    revision = converter_revision(game.root)
    abi = abi_digest(game.root).hex()
    profile = profile_sha256(game)
    try:
        existing = json.loads((out / "manifest.json").read_text())
        current = (
            existing["converter"] == revision
            and existing["abi"] == abi
            and existing["source_sha256"] == game.source["sha256"]
            and all(existing.get(key) == value for key, value in identity.items())
            and existing["profile_sha256"] == profile
            and all((out / n).stat().st_size == existing["bytes"][n]
                    and sha256(out / n) == existing["sha256"][n] for n in GAME_FILES)
        )
    except (OSError, KeyError, ValueError):
        current = False
    if current and not force:
        validate_package(game, out.parent, out / "game.d64p")
        return out
    (out / "manifest.json").unlink(missing_ok=True)
    meta = {
        "converter": revision,
        "director_version": game.data["port"]["director_version"],
        "game": game.slug,
        "renderer_target": "android",
        "schema_version": 1,
        "source_sha256": game.source["sha256"],
        **identity,
    }
    print(f"converting {media.name}", file=sys.stderr)
    started = time.monotonic()
    converter = Converter(game, out, meta)
    converter.convert()
    if game.image_identity != identity:
        raise ValueError("selected source image changed during conversion; retry conversion")
    validate_package(game, out.parent, out / "game.d64p")
    manifest = {
        "schema_version": 1,
        "slug": game.slug,
        "source_sha256": game.source["sha256"],
        **identity,
        "abi": abi,
        "converter": revision,
        "profile_sha256": profile,
        "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "bytes": {n: (out / n).stat().st_size for n in GAME_FILES},
        "sha256": {n: sha256(out / n) for n in GAME_FILES},
        "report": converter.report,
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    # The recovered sources stay for inspection (conversion/analysis/lingo);
    # the disc copy and the unpacked media go.
    for bulky in ("extracted", "director/images", "director/wav", "director/video"):
        shutil.rmtree(converter.work / bulky, ignore_errors=True)
    total = sum(manifest["bytes"].values())
    print(json.dumps({"seconds": round(time.monotonic() - started, 1), "bytes": total,
                      "sounds": converter.report["sounds"], "images": converter.report["images"],
                      "videos": converter.report.get("videos", 0)}), file=sys.stderr)
    return out


def main(argv: list[str]) -> int:
    import argparse

    parser = argparse.ArgumentParser(prog="director64 convert", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--force", action="store_true", help="convert again")
    parser.add_argument("--output", type=Path, help="output directory")
    args = parser.parse_args(argv)
    from .project import selected_game

    print(convert(selected_game(), args.output, args.force))
    return 0


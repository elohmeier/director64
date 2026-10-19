"""The standalone web player: one game, converted from the user's own disc
image, as a static site (docs/web.md).

    director64 web --game SLUG [--serve]   dist/<slug>/<source>/web/

The game is converted as for its Android TV APK (convert.py); the engine
is the same runtime built with Emscripten, and the page is the player under
platforms/web/site. Like an APK, the site contains the converted game and must
not be redistributed.
"""

from __future__ import annotations

import argparse
import functools
import hashlib
import http.server
import json
import os
import shutil
import subprocess
import zlib
from pathlib import Path

from .project import GameSpec, container_engine

# Emscripten, pinned: the tag names the release and the digest is what runs.
EMSDK_IMAGE = (
    "docker.io/emscripten/emsdk:6.0.10"
    "@sha256:e077d54e2b8970575ebc4f185ac1de0b95c05f2b266134d4ba27449af7aebf65"
)
EXPORTS = (
    "main",
    "malloc",
    "free",
    "d64_flash",
    "d64_flash_bytes",
    "d64_boot",
    "d64_boot_rpc",
    "d64_step",
    "d64_render",
    "d64_pixels",
    "d64_stage_width",
    "d64_stage_height",
    "d64_error",
    "d64_movie",
    "d64_ticks",
    "d64_frame",
    "d64_script_errors",
    "d64_last_script_error",
    "d64_save_generation",
    "d64_image_bytes",
    "d64_image_loads",
    "d64_rpc",
    "d64_probe_render",
    "d64_text_field",
    "d64_text_field_rect",
    "d64_text_value",
    "d64_edit_text",
    "d64_key",
    "d64_print_request",
    "d64_pad",
    "d64_pointer_warp",
    "d64_pointer_x",
    "d64_pointer_y",
    "d64_pointer_player",
    "d64_step_pads",
    "d64_render_pads",
    "d64_load_package",
    "d64_package_error",
)
# The asset packs a game plays from.
PACKS = ("images", "audio", "fonts", "streams", "data", "video", "print")
SITE_FILES = (
    "index.html",
    "app.js",
    "player.js",
    "ui.js",
    "audio.js",
    "flash-audio.js",
    "saves.js",
    "video.js",
    "gamepad.js",
    "keyboard.js",
    "screen.js",
    "settings.js",
    "strings.js",
    "style.css",
    "print.html",
    "print.css",
)
# CC0 (datagoblin): the builtin font unstyled text prints with.
FONT = Path("third_party/libdragon/examples/fontgallery/assets/monogram.ttf")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def player(game: GameSpec) -> dict:
    """The game's [player] table in its game.toml: the files its Lingo reads
    and writes (runtime/storage/saves.c) and its runtime switches."""
    game.require_port()
    table = game.data.get("player", {})
    known = {"saves", "files", "folders", "long_date", "movie_path", "data_folder",
             "key_field", "switches"}
    if set(table) - known:
        raise ValueError(f"{game.slug}: unknown [player] keys {sorted(set(table) - known)}")
    return table


def c_string(text: str) -> str:
    """A C string literal of `text`'s windows-1252 bytes."""
    out = []
    for byte in text.encode("cp1252"):
        char = chr(byte)
        out.append(char if 32 <= byte < 127 and char not in '"\\?' else f"\\{byte:03o}")
    return '"' + "".join(out) + '"'


def abi_digest(root: Path) -> bytes:
    """The runtime ABI a package binds to (docs/package-format.md): name ids and
    opcodes are positions in these two files."""
    digest = hashlib.sha256()
    for name in ("runtime/lingo/names.txt", "runtime/lingo/lingo_bytecode.h"):
        digest.update((root / name).read_bytes())
    return digest.digest()[:16]


def profile_header(game: GameSpec) -> str:
    """game_profile.h: the entry movie, the runtime ABI and the [player] table."""
    port, table = game.data["port"], player(game)
    abi = ",".join(f"0x{b:02x}" for b in abi_digest(game.root))
    lines = [
        "// Generated from the game's game.toml by director64; do not edit.",
        "#ifndef DIRECTOR64_GAME_PROFILE_H", "#define DIRECTOR64_GAME_PROFILE_H",
        f"#define DIRECTOR64_ENTRY_MOVIE {c_string(port['entry_movie'])}",
        f"#define DIRECTOR64_ABI {{{abi}}}",
    ]
    if table.get("saves"):
        lines.append("#define GAME_SAVES " + ", ".join(c_string(n) for n in table["saves"]))
    if table.get("files"):
        lines.append("#define GAME_FILES " + ", ".join(
            f"{{{c_string(name)}, {c_string(text)}}}" for name, text in table["files"].items()))
    if table.get("folders"):
        lines.append("#define GAME_FOLDERS " + ", ".join(
            f"{{{c_string(folder[0].lower())}, {c_string(name)}}}"
            for folder in table["folders"] for name in folder[1:]))
    for key, macro in (("long_date", "GAME_LONG_DATE"), ("movie_path", "GAME_MOVIE_PATH"),
                       ("data_folder", "GAME_DATA_FOLDER")):
        if key in table:
            lines.append(f"#define {macro} {c_string(table[key])}")
    if field := table.get("key_field"):
        lines += [
            f"#define GAME_KEY_FIELD_MOVIE {c_string(field['movie'])}",
            f"#define GAME_KEY_FIELD_MEMBER {c_string(field['member'])}",
            "#define GAME_KEY_FIELD_FRAMES " + ", ".join(str(int(f)) for f in field["frames"]),
        ]
    return "\n".join([*lines, "#endif", ""])


WEBP = Path("third_party/libwebp")


def webp_decoder_sources(root: Path) -> list[str]:
    """The vendored libwebp decoder (third_party/libwebp), for the images the
    importer packs as WebP (platforms/web/webp_image.c)."""
    found = []
    for folder in ("src/dec", "src/dsp", "src/utils"):
        for p in sorted((root / WEBP / folder).glob("*.c")):
            name = p.name
            if any(tag in name for tag in ("_mips", "_msa", "_enc", "enc_", "cost", "ssim",
                                           "histogram", "picture", "huffman_encode",
                                           "quant_levels_utils", "bit_writer")) or name == "enc.c":
                continue
            found.append(p.relative_to(root).as_posix())
    return found


def sources(game: GameSpec) -> list[str]:
    # The game's own input or printing code, when it has some.
    adapter = sorted(
        p.relative_to(game.root).as_posix() for p in (game.directory / "runtime").glob("*.c")
    )
    return [
        "platforms/web/web_main.c",
        "platforms/web/web_runtime.c",
        "platforms/web/compositor.c",
        "platforms/web/webp_image.c",
        *webp_decoder_sources(game.root),
        "runtime/lingo/lingo_runtime.c",
        "runtime/director/director.c",
        "runtime/director/shape.c",
        "runtime/interaction/input.c",
        "runtime/interaction/pointer.c",
        "runtime/package/package.c",
        "runtime/storage/saves.c",
        *adapter,
    ]


def compile_command(game: GameSpec, output: Path) -> list[str]:
    """The profile runtime's emcc command; `output` holds gen/ and receives
    director64.mjs and director64.wasm. Android TV compiles the same sources
    and flags without the Emscripten glue (android.makefile)."""
    generated = output.relative_to(game.root).as_posix()
    includes = [
        "runtime",
        "runtime/director",
        "runtime/lingo",
        "runtime/interaction",
        "runtime/storage",
        "runtime/package",
        "platforms/web",
        f"games/{game.slug}/runtime",
        f"games/{game.slug}/probe",
        f"{generated}/gen",
    ]
    return [
        "emcc",
        "-std=c17",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pedantic",
        *(f"-I{p}" for p in includes),
        f"-I{WEBP.as_posix()}",
        *family_defines(game),
        *game_defines(game),
        "-DDIRECTOR64_WEBP=1",
        *sources(game),
        "--js-library",
        "platforms/web/library.js",
        "-sMODULARIZE=1",
        "-sEXPORT_ES6=1",
        "-sEXPORT_NAME=createDirector64",
        "-sENVIRONMENT=web,node",
        "-sALLOW_MEMORY_GROWTH=1",
        "-sINITIAL_MEMORY=64MB",
        "-sSTACK_SIZE=8MB",
        "-sEXPORTED_FUNCTIONS=" + ",".join("_" + name for name in EXPORTS),
        "-sEXPORTED_RUNTIME_METHODS=ccall,UTF8ToString,HEAPU8,HEAPU32,HEAP32",
        "-o",
        f"{generated}/director64.mjs",
    ]


def git_revision(root: Path) -> dict:
    def git(*args):
        return subprocess.run(
            ["git", *args], cwd=root, capture_output=True, text=True, check=False
        ).stdout.strip()

    return {"commit": git("rev-parse", "HEAD"), "dirty": bool(git("status", "--porcelain"))}


def emscripten(root: Path, engine: str, command: list[str]) -> None:
    subprocess.run(
        [
            engine,
            "run",
            "--rm",
            "--network=none",
            "--user",
            f"{os.getuid()}:{os.getgid()}",
            "--volume",
            f"{root}:/workdir",
            "--workdir",
            "/workdir",
            EMSDK_IMAGE,
            *command,
        ],
        check=True,
    )


def build_runtime(game: GameSpec, output: Path, engine: str) -> list[str]:
    (output / "gen").mkdir(parents=True, exist_ok=True)
    (output / "gen/game_profile.h").write_text(profile_header(game))
    command = compile_command(game, output)
    emscripten(game.root, engine, command)
    return command


def web_policy(game: GameSpec) -> dict:
    """The game's conversion policy (games/<slug>/host/source-policy.json)."""
    return json.loads((game.directory / "host/source-policy.json").read_text())


def profile_entry(game: GameSpec) -> dict:
    """What a conversion depends on besides the disc and the converter: the
    game's pinned edition, port settings and policy. No disc content."""
    port = game.data["port"]
    return {
        "slug": game.slug,
        "title": game.data["title"],
        "source": {
            "id": game.source["id"],
            "file": game.source["file"],
            "sha256": game.source["sha256"],
            "bytes": game.source["bytes"],
            "kind": game.source["kind"],
            "alternates": [
                {key: image[key] for key in ("file", "sha256", "bytes")}
                for image in game.images[1:]
            ],
        },
        "port": {
            key: port[key]
            for key in (
                "director_version",
                "media_root",
                "entry_movie",
                "extended_d6",
                "launcher_movie",
                "defer_video",
            )
            if key in port
        },
        "policy": web_policy(game),
        "convert": game.data.get("convert", {}),
    }


NOTICES = """Director64 web player
======================

Revision {commit}. Source: https://github.com/elohmeier/director64

"""


def notices(root: Path, header: str) -> str:
    """The player's NOTICES.txt: the repository's license, its third-party
    notices and every license text they refer to."""
    parts = [header] + [
        (root / name).read_text(encoding="utf-8") for name in ("LICENSE", "THIRD_PARTY_NOTICES.md")
    ]
    for text in sorted((root / "licenses").glob("*.txt")):
        parts.append(f"{text.stem}\n{'=' * len(text.stem)}\n\n{text.read_text(encoding='utf-8')}")
    return "\n\n".join(part.rstrip("\n") for part in parts) + "\n"


def family_version(director_version: int) -> int:
    """The runtime family for a port's Director version: Director 8.5 runs as
    the D10 family (runtime/lingo/family.h)."""
    return 10 if director_version == 850 else director_version // 100


def family_defines(game: GameSpec) -> list[str]:
    """The runtime family a game's tables are laid out for
    (runtime/lingo/family.h)."""
    port = game.data["port"]
    defines = [
        f"-DDIRECTOR64_DIRECTOR_VERSION={family_version(port['director_version'])}",
        f"-DDIRECTOR64_EXTENDED_D6={int(port.get('extended_d6', False))}",
    ]
    # Director 8.5 media conventions inside the D10 family (ink.h masks).
    if port["director_version"] == 850:
        defines.append("-DDIRECTOR64_D85=1")
        # The disc folder the movies sit in, for the moviePath of a movie in a
        # subfolder (Löwenzahn's DATA\LABYRINTH\).
        root = str(port.get("media_root", ".")).strip("./\\")
        if root.isalnum():
            defines.append(f"-DDIRECTOR64_MEDIA_ROOT={root}")
    # An 800x600 game plays its authored stage unscaled (ink.h).
    if native_stage(game):
        defines.append("-DDIRECTOR64_NATIVE_STAGE=1")
    return defines


def native_stage(game: GameSpec) -> bool:
    """Whether the game's authored stage is 800x600 (the policy's
    `native_stage`), which the players draw unscaled; otherwise it is
    640x480."""
    return bool(web_policy(game).get("native_stage"))


def game_defines(game: GameSpec) -> list[str]:
    """The game's runtime switches (DIRECTOR64_<SWITCH>: its own input or
    printing code in games/<slug>/runtime), and its probe step and state
    (games/<slug>/probe) when it has them."""
    defines = [f"-DDIRECTOR64_{name}=1" for name in player(game).get("switches", [])]
    if (game.directory / "probe").is_dir():
        defines.append("-DDIRECTOR64_GAME_PROBE=1")
    return defines


def validate_package(game: GameSpec, output: Path, package: Path) -> dict:
    """Load every package record with the production, profile-specific loader."""
    folder = output / "gen"
    folder.mkdir(parents=True, exist_ok=True)
    tool = folder / "package-validate"
    subprocess.run(
        [
            os.environ.get("CC", "cc"),
            "-std=c17", "-O1", "-Wall", "-Wextra", "-Werror", "-pedantic",
            *family_defines(game),
            *(f"-I{p}" for p in (
                "runtime/director", "runtime/lingo", "runtime/interaction",
                "runtime/storage", "runtime/package",
            )),
            "tools/director/validate-package.c", "runtime/package/package.c",
            "-o", str(tool),
        ],
        cwd=game.root,
        check=True,
    )
    result = subprocess.run(
        [tool, package, abi_digest(game.root).hex()], capture_output=True, text=True, check=False
    )
    if result.returncode:
        raise ValueError(f"{package}: runtime rejected converted package:\n{result.stderr}")
    return json.loads(result.stdout)


def write_probe(game: GameSpec, site: Path) -> Path:
    """An executable speaking the probe line protocol over the site's Wasm
    runtime (platforms/web/node/probe.mjs), for `director64 fuzz`, `perf`,
    `parity` and the games' journey scripts."""
    probe = site.parent / "director-web-probe"
    harness = game.root / "platforms/web/node/probe.mjs"
    probe.write_text(f'#!/bin/sh\nexec node "{harness}" "{site}" "$@"\n')
    probe.chmod(0o755)
    return probe


def site_path(game: GameSpec) -> Path:
    return game.dist / "web"


def probe_path(game: GameSpec) -> Path:
    """The web build's probe (write_probe); `director64 web` makes it."""
    probe = game.dist / "director-web-probe"
    if not probe.is_file():
        raise ValueError(f"no web build to probe; run director64 web --game {game.slug}")
    return probe


# "D64Z", the raw length (u32 big-endian) and a raw deflate stream: an image
# the converter kept deflated (compiler/src/convert/files.rs), stored inflated
# here so the page reads it as it is; HTTP compresses it on the way.
def inflated(data: bytes) -> bytes:
    if data[:4] != b"D64Z":
        return data
    raw = zlib.decompress(data[8:], -15)
    if len(raw) != int.from_bytes(data[4:8], "big"):
        raise ValueError("deflated image length differs from its header")
    return raw


def write_pack(source: Path, kind: str, assets: Path) -> dict:
    """One asset pack of the converted game into the site, images inflated."""
    index = json.loads((source / f"{kind}.json").read_text())
    blob = (source / f"{kind}.bin").read_bytes()
    if kind == "images":
        out, rebuilt = bytearray(), {}
        for name, (offset, length, *rest) in sorted(index.items(), key=lambda e: e[1][0]):
            data = inflated(blob[offset : offset + length])
            rebuilt[name] = [len(out), len(data), *rest]
            out += data
        index, blob = rebuilt, bytes(out)
    (assets / f"{kind}.bin").write_bytes(blob)
    (assets / f"{kind}.json").write_text(json.dumps(index, separators=(",", ":")) + "\n")
    return {"file": f"assets/{kind}.bin", "index": f"assets/{kind}.json", "bytes": len(blob)}


def build_site(game: GameSpec, runtime: Path, converted: Path, command: list[str]) -> Path:
    """The page, the runtime and the converted game as one static site."""
    root = game.root
    site = site_path(game)
    if site.exists():
        shutil.rmtree(site)
    (site / "assets").mkdir(parents=True)
    for name in SITE_FILES:
        shutil.copyfile(root / "platforms/web/site" / name, site / name)
    shutil.copyfile(root / FONT, site / "monogram.ttf")
    for name in ("director64.mjs", "director64.wasm"):
        shutil.copyfile(runtime / name, site / name)
    shutil.copyfile(converted / "game.d64p", site / "game.d64p")
    packs = {
        kind: write_pack(converted, kind, site / "assets")
        for kind in PACKS
        if (converted / f"{kind}.bin").stat().st_size
    }
    revision = git_revision(root)
    (site / "NOTICES.txt").write_text(notices(root, NOTICES.format(commit=revision["commit"])))
    title = game.data["title"]
    (site / "manifest.webmanifest").write_text(
        json.dumps(
            {
                "name": title,
                "short_name": title,
                "start_url": ".",
                "display": "fullscreen",
                "background_color": "#000000",
            },
            indent=2,
        )
        + "\n"
    )
    port = game.data["port"]
    manifest = {
        "schema_version": 3,
        "revision": revision,
        "game": {"slug": game.slug, "entry_movie": port["entry_movie"]},
        "profile": {
            "slug": game.slug,
            "title": title,
            "source": game.source["id"],
            "port": {"director_version": port["director_version"]},
            "saves": bool(player(game).get("saves")),
        },
        "runtime": {
            "toolchain": EMSDK_IMAGE,
            "wasm_sha256": sha256(site / "director64.wasm"),
            "flags": [a for a in command if a.startswith(("-s", "-O", "-D"))],
            "abi": abi_digest(root).hex(),
        },
        "package": {"file": "game.d64p", "sha256": sha256(site / "game.d64p")},
        "packs": packs,
        "conversion": json.loads((converted / "manifest.json").read_text())["converter"],
    }
    (site / "build.json").write_text(json.dumps(manifest, indent=2) + "\n")
    write_probe(game, site)
    return site


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".mjs": "text/javascript",
        ".js": "text/javascript",
        ".wasm": "application/wasm",
        ".json": "application/json",
        ".webmanifest": "application/manifest+json",
        ".ttf": "font/ttf",
    }

    def end_headers(self):
        # Local testing: always revalidate, so a rebuild is what loads.
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()


def serve(site: Path, port: int) -> None:
    handler = functools.partial(Handler, directory=str(site))
    with http.server.ThreadingHTTPServer(("127.0.0.1", port), handler) as server:
        print(f"serving {site} at http://127.0.0.1:{port}/", flush=True)
        server.serve_forever()


def build(game: GameSpec, reconvert: bool = False) -> Path:
    from . import android
    game.require_port()
    converted = android.convert(game, game.work / "converted", reconvert)
    runtime = game.work / "web"
    command = build_runtime(game, runtime, container_engine())
    return build_site(game, runtime, converted, command)


def main(game: GameSpec, argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="director64 web", description=__doc__)
    parser.add_argument("--reconvert", action="store_true", help="convert the disc image again")
    parser.add_argument("--serve", action="store_true", help="serve the site on localhost")
    parser.add_argument("--port", type=int, default=18064)
    parser.add_argument("--no-build", action="store_true", help="serve the existing build")
    args = parser.parse_args(argv)
    site = site_path(game) if args.no_build else build(game, args.reconvert)
    print(site)
    if args.serve:
        serve(site, args.port)
    return 0

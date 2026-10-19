"""The Android TV player (docs/android.md): one APK per game, holding the
engine compiled for Android and the game converted from your own disc.

    director64 android build --game SLUG     dist/<slug>/<source>/director64-<slug>.apk
    director64 android install --game SLUG   adb install it

The game is converted from its disc image (src/director64/convert.py) into
build/<slug>/<source>/converted/, which the web build shares, and ships
uncompressed in the APK's assets. The APK contains the converted game and
must not be redistributed.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import time
import zipfile
from pathlib import Path
from xml.sax.saxutils import escape

from . import web
from .convert import GAME_FILES, convert, source_digest
from .project import GameSpec, container_engine

IMAGE = "director64-android:local"
# The projector runs 32-bit and 64-bit ARM userlands alike; x86_64 serves the
# emulator.
ABIS = {
    "armeabi-v7a": "armv7a-linux-androideabi",
    "arm64-v8a": "aarch64-linux-android",
    "x86_64": "x86_64-linux-android",
}
MIN_SDK = 28
TARGET_SDK = 34
# Decoded images the compositor keeps; the browser's 192 MiB is too much
# beside a 2 GiB TV's system.
IMAGE_BUDGET = 96 * 1024 * 1024
HOST_SOURCES = (
    "platforms/android/jni/main.c",
    "platforms/android/jni/packs.c",
    "platforms/android/jni/json.c",
    "platforms/android/jni/mixer.c",
    "platforms/android/jni/decode.c",
    "platforms/android/jni/video.c",
)
LIBS = ("-landroid", "-llog", "-lz", "-laaudio", "-lmediandk", "-lm")
KEYSTORE = (
    Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "director64/android.keystore"
)
KEYSTORE_PASSWORD = "director64"


def run_container(root: Path, script: str) -> None:
    KEYSTORE.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            container_engine(),
            "run",
            "--rm",
            "--network=none",
            "--user",
            f"{os.getuid()}:{os.getgid()}",
            "--env",
            "HOME=/tmp",
            "--volume",
            f"{root}:/workdir",
            "--volume",
            f"{KEYSTORE.parent}:/keystore",
            "--workdir",
            "/workdir",
            IMAGE,
            "sh",
            "-ec",
            script,
        ],
        check=True,
    )


def library(game: GameSpec) -> str:
    return "d64_" + game.slug.replace("-", "_")


# ---- The engine ----


def makefile(game: GameSpec, output: Path, abis: list[str]) -> Path:
    """The game's runtime as make rules: the browser runtime's sources
    (without its Emscripten glue) and flags, with the Android host."""
    root = game.root
    gen = output / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    header = web.profile_header(game)
    if not (gen / "game_profile.h").is_file() or (gen / "game_profile.h").read_text() != header:
        (gen / "game_profile.h").write_text(header)
    command = web.compile_command(game, output)
    arguments = command[1 : command.index("--js-library")]
    sources = [a for a in arguments if a.endswith(".c") and a != "platforms/web/web_main.c"] + list(
        HOST_SOURCES
    )
    flags = [a for a in arguments if not a.endswith(".c")] + [
        "-Iplatforms/android/jni",
        f"-DWEB_IMAGE_BUDGET={IMAGE_BUDGET}u",
        "-fPIC",
        "-fvisibility=hidden",
        "-ffunction-sections",
        "-fdata-sections",
    ]
    flags_text = " ".join(shlex.quote(f) for f in flags)
    rel = output.relative_to(root).as_posix()
    lines = ["NDK_BIN := $(ANDROID_NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin", ".SUFFIXES:"]
    targets = []
    for abi in abis:
        cc = f"$(NDK_BIN)/clang --target={ABIS[abi]}{MIN_SDK}"
        objects = []
        for source in sources:
            obj = f"{rel}/obj/{abi}/{source.removesuffix('.c').replace('/', '__')}.o"
            objects.append(obj)
            lines += [
                f"{obj}: {source} {rel}/Makefile",
                "\t@mkdir -p $(dir $@)",
                f"\t{cc} {flags_text} -MMD -MP -c {source} -o $@",
                f"-include {obj.removesuffix('.o')}.d",
            ]
        lib = f"{rel}/lib/{abi}/lib{library(game)}.so"
        lines += [
            f"{lib}: {' '.join(objects)}",
            "\t@mkdir -p $(dir $@)",
            f"\t{cc} -shared -Wl,--gc-sections -Wl,-z,max-page-size=16384 "
            f"-Wl,--exclude-libs,ALL -o $@ $^ {' '.join(LIBS)}",
            "\t$(NDK_BIN)/llvm-strip --strip-unneeded $@",
        ]
        targets.append(lib)
    lines.insert(2, "all: " + " ".join(targets))
    path = output / "Makefile"
    text = "\n".join(lines) + "\n"
    if not path.is_file() or path.read_text() != text:
        path.write_text(text)
    return path


def cover(game: GameSpec) -> Path | None:
    """The game's box art, games/<slug>/cover.*: local, never committed."""
    found = sorted(game.directory.glob("cover.*"))
    return found[0] if found else None


def artwork(game: GameSpec, res: Path) -> None:
    """The launcher's icon and Google TV banner, when ImageMagick is there
    (otherwise the player's own): with the game's box art, the cover as the
    icon and beside the title on the banner; without, the title alone."""
    magick = shutil.which("magick")
    if not magick:
        return
    font = str(game.root / web.FONT)
    title = [
        "(",
        "-size",
        "262x240" if cover(game) else "560x220",
        "-background",
        "none",
        "-fill",
        "white",
        "-font",
        font,
        "-gravity",
        "center",
        f"caption:{game.data['title']}",
        ")",
    ]
    banner = res / "drawable-xhdpi/banner.png"
    banner.parent.mkdir(parents=True, exist_ok=True)
    art = cover(game)
    if art:
        icon = res / "drawable-xhdpi/icon.png"
        icon.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [
                magick,
                str(art),
                "-resize",
                "192x192",
                "-background",
                "#101418",
                "-gravity",
                "center",
                "-extent",
                "192x192",
                "-strip",
                str(icon),
            ],
            check=True,
        )
        # The cover, blurred and darkened, fills the banner; the cover itself
        # stands on the left and the title on the right.
        layout = [
            str(art),
            "-resize",
            "640x360^",
            "-gravity",
            "center",
            "-extent",
            "640x360",
            "-blur",
            "0x12",
            "-fill",
            "#000000",
            "-colorize",
            "55%",
            "(",
            str(art),
            "-resize",
            "x312",
            ")",
            "-gravity",
            "west",
            "-geometry",
            "+24+0",
            "-composite",
            *title,
            "-gravity",
            "east",
            "-geometry",
            "+18-20",
            "-composite",
        ]
    else:
        layout = [
            "-size",
            "640x360",
            "gradient:#1b2735-#090a0f",
            *title,
            "-gravity",
            "north",
            "-geometry",
            "+0+40",
            "-composite",
        ]
    subprocess.run(
        [
            magick,
            *layout,
            "-font",
            font,
            "-fill",
            "#9fb3c8",
            "-pointsize",
            "36",
            "-gravity",
            "southeast" if art else "south",
            "-annotate",
            "+28+22" if art else "+0+28",
            "Director64",
            "-strip",
            str(banner),
        ],
        check=True,
    )


def place(source: Path, target: Path) -> None:
    """A large file in the staging tree without copying it."""
    target.unlink(missing_ok=True)
    try:
        os.link(source, target)
    except OSError:
        shutil.copyfile(source, target)


# ---- The APK ----


def version_code() -> int:
    """Minutes since 2026: every build installs as an update, and the TV's
    launcher only reloads an app's icon and banner when this changes."""
    return int(time.time() - 1767225600) // 60


def apk_path(game: GameSpec) -> Path:
    return game.dist / f"director64-{game.slug}.apk"


def apk_receipt_path(game: GameSpec) -> Path:
    return apk_path(game).with_suffix(".apk.json")


def apk_input_digest(game: GameSpec, abis: list[str]) -> str:
    """Inputs to this APK, separate from the reusable converted asset packs."""
    import hashlib

    root = game.root
    paths = set()
    for directory in (
        "runtime", "platforms/web", "platforms/android", "docker/android",
        f"games/{game.slug}/runtime", f"games/{game.slug}/probe",
    ):
        paths.update(p for p in (root / directory).rglob("*") if p.is_file())
    paths.update(game.directory.glob("cover.*"))
    paths.update(game.directory.rglob("*.h"))
    paths.update(root / name for name in (
        "src/director64/android.py", "src/director64/project.py",
        "src/director64/web.py", str(web.FONT),
        "tools/director/validate-package.c",
    ))
    digest = hashlib.sha256(json.dumps({
        "source": game.source["sha256"], "abis": abis,
        **game.image_identity,
        "profile": web.profile_entry(game),
        "converter": source_digest(root),
    }, sort_keys=True).encode())
    for path in sorted(paths):
        digest.update(path.relative_to(root).as_posix().encode() + b"\0")
        digest.update(bytes.fromhex(web.sha256(path)))
    return digest.hexdigest()


def verify_apk(game: GameSpec) -> Path:
    """Refuse a stale APK or an artifact left behind by a failed rebuild."""
    path = apk_path(game)
    try:
        receipt = json.loads(apk_receipt_path(game).read_text())
        valid = (
            receipt["source_sha256"] == game.source["sha256"]
            and all(receipt.get(key) == value
                    for key, value in game.image_identity.items())
            and receipt["inputs"] == apk_input_digest(game, receipt["abis"])
            and receipt["apk_sha256"] == web.sha256(path)
        )
    except (OSError, KeyError, ValueError, TypeError):
        valid = False
    if not valid:
        raise ValueError(f"{path}: missing or stale build receipt; rebuild this game's APK")
    return path


def build(game: GameSpec, jobs: int, abis: list[str], reconvert: bool) -> Path:
    root = game.root
    game.require_port()
    apk_receipt_path(game).unlink(missing_ok=True)
    identity = apk_input_digest(game, abis)
    work = root / "build/android" / game.slug / game.source["id"]
    data = convert(game, force=reconvert)
    make = makefile(game, work, abis)

    # Assets: the game under game/, its profile, and the builtin font.
    assets = work / "assets"
    if assets.exists():
        shutil.rmtree(assets)
    (assets / "game").mkdir(parents=True)
    for name in GAME_FILES:
        place(data / name, assets / "game" / name)
    port = game.data["port"]
    (assets / "game/profile.json").write_text(
        json.dumps(
            {
                "slug": game.slug,
                "title": game.data["title"],
                "library": library(game),
                "director_version": port["director_version"],
                "source_sha256": game.source["sha256"],
            },
            indent=2,
        )
        + "\n"
    )
    shutil.copyfile(root / web.FONT, assets / "monogram.ttf")
    (assets / "NOTICES.txt").write_text(
        web.notices(root, "Director64 Android TV player\n============================\n\n")
    )
    res = work / "res"
    if res.exists():
        shutil.rmtree(res)
    artwork(game, res)
    revision = web.git_revision(root)
    version = revision["commit"][:12] + ("-dirty" if revision["dirty"] else "")
    (work / "AndroidManifest.xml").write_text(
        (root / "platforms/android/AndroidManifest.xml")
        .read_text()
        .replace("@TITLE@", escape(game.data["title"], {'"': "&quot;"}))
    )
    package = "org.director64.tv." + game.slug.replace("-", "_")
    w = work.relative_to(root).as_posix()
    overlay = (
        f"$BUILD_TOOLS/aapt2 compile --dir {w}/res -o {w}/out/game-res.zip\n"
        if any(res.rglob("*.png"))
        else ""
    )
    run_container(
        root,
        f"""
$ANDROID_NDK/prebuilt/linux-x86_64/bin/make -s -j{jobs} -f {make.relative_to(root).as_posix()} all
rm -rf {w}/out && mkdir -p {w}/out/classes {w}/out/dex {w}/out/gen
$BUILD_TOOLS/aapt2 compile --dir platforms/android/res -o {w}/out/res.zip
{overlay}$BUILD_TOOLS/aapt2 link -o {w}/out/base.apk -I $ANDROID_JAR \\
  --manifest {w}/AndroidManifest.xml --rename-manifest-package {package} \\
  -A {w}/assets --no-compress --java {w}/out/gen \\
  --min-sdk-version {MIN_SDK} --target-sdk-version {TARGET_SDK} \\
  --version-code {version_code()} --version-name {version} \\
  {w}/out/res.zip {f"-R {w}/out/game-res.zip" if overlay else ""}
javac -nowarn --release 11 -cp $ANDROID_JAR -d {w}/out/classes \\
  $(find platforms/android/java {w}/out/gen -name '*.java')
$BUILD_TOOLS/d8 --release --min-api {MIN_SDK} --lib $ANDROID_JAR --output {w}/out/dex \\
  $(find {w}/out/classes -name '*.class')
if [ ! -f /keystore/android.keystore ]; then
  keytool -genkeypair -keystore /keystore/android.keystore -alias director64 \\
    -keyalg RSA -keysize 3072 -validity 36500 -dname CN=Director64 \\
    -storepass {KEYSTORE_PASSWORD} -keypass {KEYSTORE_PASSWORD} >/dev/null
fi
""",
    )
    # The dex and native libraries join the linked resources and assets;
    # libraries stay stored and page-aligned so they load in place.
    unsigned = work / "out/unsigned.apk"
    shutil.move(work / "out/base.apk", unsigned)
    with zipfile.ZipFile(unsigned, "a", allowZip64=True) as apk:
        apk.write(work / "out/dex/classes.dex", "classes.dex", zipfile.ZIP_DEFLATED)
        for abi in abis:
            name = f"lib{library(game)}.so"
            apk.write(work / "lib" / abi / name, f"lib/{abi}/{name}", zipfile.ZIP_STORED)
    game.dist.mkdir(parents=True, exist_ok=True)
    apk = apk_path(game)
    out = apk.relative_to(root).as_posix()
    run_container(
        root,
        f"""
$BUILD_TOOLS/zipalign -P 16 -f 4 {w}/out/unsigned.apk {w}/out/aligned.apk
rm {w}/out/unsigned.apk
$BUILD_TOOLS/apksigner sign --ks /keystore/android.keystore --ks-pass pass:{KEYSTORE_PASSWORD} \\
  --out {out} {w}/out/aligned.apk
rm -f {w}/out/aligned.apk {out}.idsig
""",
    )
    if apk_input_digest(game, abis) != identity:
        raise ValueError("APK inputs changed during build; rebuild before installing")
    receipt = {
        "source_sha256": game.source["sha256"], "abis": abis, "inputs": identity,
        **game.image_identity,
        "apk_sha256": web.sha256(apk), "bytes": apk.stat().st_size,
        "conversion": json.loads((data / "manifest.json").read_text()),
    }
    apk_receipt_path(game).write_text(json.dumps(receipt, indent=2) + "\n")
    return apk


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="director64 android",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="action", required=True)
    b = sub.add_parser("build", help="build the game's APK")
    b.add_argument("--game", required=True)
    b.add_argument("--source")
    b.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    b.add_argument(
        "--abi", action="append", choices=sorted(ABIS), help="only these ABIs (default: all)"
    )
    b.add_argument("--reconvert", action="store_true", help="convert the disc image again")
    i = sub.add_parser("install", help="adb install the game's APK")
    i.add_argument("--game", required=True)
    i.add_argument("--source")
    i.add_argument("--serial", help="adb device serial or host:port")
    args = parser.parse_args(argv)
    game = GameSpec.load(args.game, args.source)
    game.activate()
    if args.action == "build":
        print(build(game, args.jobs, args.abi or list(ABIS), args.reconvert))
    elif args.action == "install":
        apk = verify_apk(game)
        serial = ["-s", args.serial] if args.serial else []
        subprocess.run(["adb", *serial, "install", "-r", apk], check=True)
    return 0

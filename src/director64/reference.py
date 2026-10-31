"""Original-projector capture layout and CLI, without invented fidelity claims."""

from __future__ import annotations

import hashlib
import importlib.util
import itertools
import json
import os
import re
import struct
import subprocess
from pathlib import Path, PurePosixPath


def optical_metadata(iso: Path, *, layout: tuple[int, int] | list[int] = (2048, 0),
                     windows_version: str = "win95", letter: str = "D",
                     raw_image: bool = False) -> dict:
    """Describe the pinned ISO as Wine 8 presents it, without inventing disc facts.

    Wine's ISO descriptor selection, byte-lane checksum and Win9x/NT byte order
    are observed format/OS behavior. This is an independent implementation of
    those facts, not copied Wine code. Reference: wine-8.0 mountmgr.sys/device.c,
    VOLUME_FindCdRomDataBestVoldesc and VOLUME_GetSuperblockSerial.
    """
    from .iso import Layout

    if not isinstance(letter, str) or not re.fullmatch(r"[D-Y]", letter):
        raise ValueError("optical drive letter must be D..Y (C and Z are reserved)")
    if type(raw_image) is not bool or windows_version not in {"win31", "win95", "win98", "win7"}:
        raise ValueError("unsupported optical drive settings")
    if raw_image and tuple(layout) != (2048, 0):
        raise ValueError("Wine raw image backing requires an original cooked 2048-byte ISO")
    reader = Layout(*layout)
    descriptors = []
    with iso.open("rb") as handle:
        # Wine 8 checks these four logical sectors and keeps the first highest
        # descriptor type. Do not substitute a protection routine's return value.
        for sector in range(16, 20):
            descriptor = reader.read(handle, sector * 2048, 2048)
            if descriptor[1:7] != b"CD001\x01":
                raise ValueError("optical metadata requires valid ISO9660 descriptors")
            kind = descriptor[0]
            if kind == 255:
                break
            if kind not in {0, 1, 2}:
                raise ValueError("unsupported optical volume descriptor type")
            if kind in {1, 2}:
                descriptors.append((kind, sector, descriptor))
    if not descriptors:
        raise ValueError("optical metadata has no supported ISO volume descriptor")
    kind, sector, descriptor = max(descriptors, key=lambda item: item[0])
    joliet = kind == 2 and descriptor[88:91] in {b"%/@", b"%/C", b"%/E"}
    label = descriptor[40:72].decode("utf-16-be" if joliet else "ascii").rstrip(" \0")
    if not label or any(ord(c) < 32 or ord(c) == 127 for c in label):
        raise ValueError("ISO volume label is empty or contains control characters")
    lanes = bytes(sum(descriptor[lane::4]) % 256 for lane in range(4))
    byte_order = "big" if windows_version == "win7" else "little"
    return {"letter": letter, "type": "cdrom", "label": label,
            "serial_hex": f"{int.from_bytes(lanes, byte_order):08x}",
            "serial_algorithm": "wine-8.0-iso9660-descriptor-checksum",
            "windows_version": windows_version, "descriptor_sector": sector,
            "descriptor_sha256": hashlib.sha256(descriptor).hexdigest(),
            "namespace": "joliet" if joliet else "iso9660", "raw_image": raw_image}


def relative(path: str) -> str:
    value = PurePosixPath(path)
    if not path or value.is_absolute() or ".." in value.parts or "\\" in path or ":" in path:
        raise ValueError(f"reference path must be disc-relative: {path}")
    return value.as_posix()


def compatibility_plan(value: dict) -> dict:
    """Validate a declared private-reference workaround, never an implicit patch."""
    if (not isinstance(value, dict) or set(value) != {"id", "reason", "patches"}
            or not isinstance(value["id"], str)
            or not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,95}", value["id"])
            or not isinstance(value["reason"], str) or not value["reason"].strip()
            or not isinstance(value["patches"], list) or not 1 <= len(value["patches"]) <= 8):
        raise ValueError("invalid reference compatibility recipe")
    paths = set()
    for patch in value["patches"]:
        if (not isinstance(patch, dict)
                or set(patch) != {"path", "source_sha256", "export", "return_u32"}
                or not all(isinstance(patch[k], str) for k in ("path", "source_sha256", "export"))
                or Path(relative(patch["path"])).suffix.lower() != ".dll"
                or not re.fullmatch(r"[a-f0-9]{64}", patch["source_sha256"])
                or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", patch["export"])
                or type(patch["return_u32"]) is not int
                or not 0 <= patch["return_u32"] <= 0xffffffff
                or patch["path"].casefold() in paths):
            raise ValueError("invalid reference compatibility DLL patch")
        paths.add(patch["path"].casefold())
    return value


def pe32_gui_launcher(path: Path) -> bool:
    """Recognize the bounded headers of an original x86 GUI executable."""
    size = path.stat().st_size
    with path.open("rb") as handle:
        dos = handle.read(64)
        if len(dos) != 64 or dos[:2] != b"MZ":
            return False
        offset = struct.unpack_from("<I", dos, 60)[0]
        if not 64 <= offset <= min(1048576, size - 24):
            return False
        handle.seek(offset)
        header = handle.read(24)
        machine, sections = struct.unpack_from("<HH", header, 4)
        optional_size, flags = struct.unpack_from("<HH", header, 20)
        if (header[:4] != b"PE\0\0" or machine != 0x14c or not 1 <= sections <= 96
                or not flags & 2 or flags & 0x2000 or not 70 <= optional_size <= 4096
                or optional_size + sections * 40 > size - offset - 24):
            return False
        optional = handle.read(optional_size)
        return (struct.unpack_from("<H", optional)[0] == 0x10b
                and struct.unpack_from("<I", optional, 16)[0] != 0
                and struct.unpack_from("<H", optional, 68)[0] == 2)


def layout_for(game, source: Path, *, iso: Path | None = None,
               windows_version: str = "win95", inventory: dict | None = None,
               unmodified: bool = False) -> dict:
    from .web import native_stage

    stage = [800, 600] if native_stage(game) else [640, 480]
    if game.slug == "findus-workshop":
        for name in ("ANNAT/PETT16.EXE", "ANNAT/DATA/FILEIO.DLL", "POFMEDIA/GINTRO.DXR"):
            if not (source / name).is_file():
                raise ValueError(f"original source file missing: {name}")
        return {"stage": stage, "install": "workshop", "launcher": "Findus.exe", "cwd": ".",
                "files": {}}
    recipe_path = game.directory / "host/reference.json"
    recipe = json.loads(recipe_path.read_text()) if recipe_path.is_file() else None
    if recipe is not None:
        required = {"schema_version", "source_sha256", "launcher", "cwd"}
        if (not isinstance(recipe, dict) or not required <= set(recipe)
                or set(recipe) - required - {"optical_drive", "compatibility", "memory_report_mib",
                                            "launcher_sha256"}
                or recipe["schema_version"] != 1
                or recipe["source_sha256"] != game.source["sha256"]):
            raise ValueError("original reference layout must match the selected source")
        root = relative(recipe["cwd"])
        launcher = source / relative(recipe["launcher"])
        if not launcher.is_file() or launcher.suffix.lower() not in {".exe", ".dat"}:
            raise ValueError("original reference launcher is missing")
        pinned = recipe.get("launcher_sha256")
        if pinned is not None or launcher.suffix.lower() == ".dat":
            if (not isinstance(pinned, str) or not re.fullmatch(r"[a-f0-9]{64}", pinned)
                    or not launcher.resolve().is_relative_to(source.resolve())):
                raise ValueError("declared launcher requires a pinned contained source file")
            with launcher.open("rb") as handle:
                actual = hashlib.file_digest(handle, "sha256").hexdigest()
            if actual != pinned:
                raise ValueError("declared launcher hash differs from the original source")
        if launcher.suffix.lower() == ".dat" and not pe32_gui_launcher(launcher):
            raise ValueError("declared .dat launcher must be an x86 PE32 GUI executable, not a DLL")
        candidates = [launcher]
    else:
        root = relative(game.data["port"].get("media_root", "."))
        candidates = None
    name = Path(game.data["port"].get("launcher_movie", game.data["port"]["entry_movie"])).stem
    if candidates is None:
        candidates = [p for p in (source / root).iterdir()
                      if p.is_file() and p.suffix.lower() == ".exe"
                      and p.stem.casefold() == name.casefold()]
    if len(candidates) != 1:
        raise ValueError(f"expected one original {name}.exe in {root}; found {len(candidates)}")
    files = {}
    for names, body in game.data.get("player", {}).get("files", {}).items():
        for path in names.split("|"):
            files[relative(path)] = body.replace("\\r", "\r").replace("\\n", "\n")
    result = {"stage": stage, "install": "disc-copy",
            "launcher": candidates[0].relative_to(source).as_posix(),
            "cwd": root, "files": files}
    if recipe and "memory_report_mib" in recipe:
        value = recipe["memory_report_mib"]
        if type(value) is not int or not 16 <= value <= 1024:
            raise ValueError("legacy memory report must be16..1024MiB")
        result["memory_report_mib"] = value
    if recipe and "compatibility" in recipe and not unmodified:
        result["compatibility"] = compatibility_plan(recipe["compatibility"])
    if recipe and "optical_drive" in recipe:
        settings = recipe["optical_drive"]
        if (not isinstance(settings, dict) or set(settings) - {"letter", "raw_image"}
                or iso is None):
            raise ValueError("optical drive recipe needs the pinned ISO and letter/raw_image only")
        with iso.open("rb") as handle:
            if hashlib.file_digest(handle, "sha256").hexdigest() != game.source["sha256"]:
                raise ValueError("optical metadata image differs from the selected source")
        result["optical_drive"] = optical_metadata(
            iso, layout=inventory.get("layout", (2048, 0)) if inventory else (2048, 0),
            windows_version=windows_version, **settings)
    return result


def runner(root: Path):
    spec = importlib.util.spec_from_file_location(
        "director64_reference_runner", root / "games/findus-workshop/host/reference_runner.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_precision(text: str) -> list[list[int]]:
    records = []
    for line in text.splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r"\[(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*rgb\(\s*(\d+),"
                             r"\s*(\d+),\s*(\d+)\s*\)\]", line.strip())
        if not match:
            raise ValueError("malformed original ink observation")
        record = [int(v) for v in match.groups()]
        if record[0] not in {40, 41} or any(not 0 <= v <= 255 for v in record[1:]):
            raise ValueError("invalid original ink observation")
        records.append(record)
    keys = [tuple(row[:4]) for row in records]
    grid = set(itertools.product((40, 41), (0, 1, 63, 64, 127, 128, 254, 255), (0, 50),
                                 (0, 1, 2, 63, 64, 65, 127, 128, 129, 191, 192, 253, 254, 255)))
    if len(records) != 448 or set(keys) != grid:
        raise ValueError("original ink-precision experiment requires its exact 448-input grid")
    return records


def compare_experiment(game, name: str, output: Path) -> int:
    if name != "ink-precision":
        print("Synthetic output recorded; no automatic comparator is registered for " + name)
        return 0
    if game.data["port"]["director_version"] != 850:
        raise ValueError("ink-precision comparison currently qualifies Director 8.5 only")
    capture = json.loads((output / "capture.json").read_text())
    modules = capture.get("loaded_modules", {})
    engines = {Path(p).name.lower(): info for p, info in modules.items()
               if Path(p).name.lower() in {"dirapi.dll", "iml32.dll"}}
    if (set(engines) != {"dirapi.dll", "iml32.dll"} or not capture.get("experiment_complete")
            or not all(any(v.startswith("8.5") for v in info.get("file_versions", []))
                       and re.fullmatch(r"[a-f0-9]{64}", info.get("sha256", ""))
                       for info in engines.values())):
        raise ValueError("comparison requires observed Director 8.5 DLLs and a completed probe")
    rows = parse_precision((output / "experiment-result.txt").read_text())
    binary = output / "ink-compare"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c17", "-O1", "-Iruntime/director",
                    "-Iruntime/lingo", "tools/reference/compare-ink.c", "-o", str(binary)],
                   cwd=game.root, check=True)
    completed = subprocess.run([binary], input="\n".join(" ".join(map(str, r)) for r in rows),
                               text=True, capture_output=True, check=False)
    report = {"schema_version": 1, "experiment": name,
              "status": "passing" if not completed.returncode else "failed",
              "observations": len(rows), "source_sha256": game.source["sha256"],
              "environment": "original-projector-under-Wine", "native_windows_validated": False,
              "hardware_qualified": False, "detail": completed.stdout + completed.stderr,
              "loaded_modules": modules}
    (output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "loaded_modules"}))
    return int(bool(completed.returncode))


def main(game, argv: list[str]) -> int:
    import sys

    sys.argv = ["director64 reference", "capture", "--game", game.slug,
                "--edition", game.source["id"], *argv]
    return runner(game.root).main()

#!/usr/bin/env python3
"""Capture the user's original projector in an isolated Wine container.

Captured media is observational evidence. This runner never declares semantic or
platform equivalence, injects random choices, or awards scene coverage.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import time
import uuid
from datetime import UTC, datetime
from itertools import product
from pathlib import Path

SCHEMA = 1
IDENTIFIER = re.compile(r"[a-zA-Z0-9][a-zA-Z0-9_.-]{0,95}\Z")
INTERRUPT_ERRORS = (subprocess.TimeoutExpired, KeyboardInterrupt)
MAX_DISC_PATH_VARIANTS = 256
FULL_SCRIPT_MARKER = "-- director64: full-script"
XOBJECT_FILEIO_MARKER = "-- director64: fileio-xobject"
FUNCTION_FILEIO_MARKER = "-- director64: fileio-functions"


def experiment_output(body: str) -> tuple[str, str]:
    """Bound the two explicitly declared original FileIO probe interfaces."""
    legacy = XOBJECT_FILEIO_MARKER in body.splitlines()[:2]
    pattern = (r'out\s*=\s*FileIO\(mNew,\s*"write",\s*"C:\\([a-z-]+\.txt)"\)'
               if legacy else r'out\.createFile\("C:\\([a-z-]+\.txt)"\)')
    functional = FUNCTION_FILEIO_MARKER in body.splitlines()[:2]
    if functional:
        pattern = r'createFile\(out,\s*"C:\\([a-z-]+\.txt)"\)'
    close = "out(mDispose)" if legacy else "closeFile(out)" if functional else "out.closeFile()"
    match = re.search(pattern, body)
    if not match or close not in body:
        raise ValueError("experiment must write and close a named synthetic C:\\ result")
    return match[1], "xobject" if legacy else "xtra-functions" if functional else "xtra"


def experiment_lingo(body: str) -> str:
    """Keep ordinary probes in startup; allow explicit authored event probes."""
    legacy = XOBJECT_FILEIO_MARKER in body.splitlines()[:2]
    functional = FUNCTION_FILEIO_MARKER in body.splitlines()[:2]
    close = body.rfind("out(mDispose)" if legacy else
                       "closeFile(out)" if functional else "out.closeFile()")
    if close < 0:
        raise ValueError("synthetic experiment must close its output")
    marker = ('out(mWriteString, "DIRECTOR64_PROBE_COMPLETE" & RETURN)\n'
              if legacy else 'writeString(out, "DIRECTOR64_PROBE_COMPLETE" & RETURN)\n'
              if functional else 'out.writeString("DIRECTOR64_PROBE_COMPLETE" & RETURN)\n')
    body = body[:close] + marker + body[close:]
    return (body if body.startswith(FULL_SCRIPT_MARKER + "\n")
            else "on startup\n" + body + "\nend\n")


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def patch_pe32_return(path: Path, expected_sha256: str, export: str, value: int) -> dict:
    """Patch one pinned private DLL export; never locate code by guessed offsets."""
    if (not isinstance(expected_sha256, str) or not isinstance(export, str)
            or not re.fullmatch(r"[0-9a-f]{64}", expected_sha256)
            or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", export)
            or type(value) is not int or not 0 <= value <= 0xffffffff):
        raise ValueError("invalid compatibility patch specification")
    original = path.read_bytes()
    before = hashlib.sha256(original).hexdigest()
    if before != expected_sha256:
        raise ValueError(f"compatibility DLL source hash differs: {path.name}")

    def span(offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > len(original):
            raise ValueError("truncated PE32 compatibility DLL")
        return original[offset:offset + size]

    def u16(offset: int) -> int:
        return struct.unpack("<H", span(offset, 2))[0]

    def u32(offset: int) -> int:
        return struct.unpack("<I", span(offset, 4))[0]

    if span(0, 2) != b"MZ":
        raise ValueError("compatibility target is not a PE32 DLL")
    pe = u32(0x3c)
    if span(pe, 4) != b"PE\0\0" or u16(pe + 4) != 0x14c:
        raise ValueError("compatibility patch requires an i386 PE32 DLL")
    count, optional_size = u16(pe + 6), u16(pe + 20)
    optional = pe + 24
    if (not 0 < count <= 96 or optional_size < 104 or u16(optional) != 0x10b
            or not u16(pe + 22) & 0x2000 or u32(optional + 92) < 1):
        raise ValueError("compatibility patch requires an i386 PE32 DLL export table")
    span(optional, optional_size + count * 40)
    sections = []
    for index in range(count):
        entry = optional + optional_size + index * 40
        sections.append((u32(entry + 12), u32(entry + 8), u32(entry + 20),
                         u32(entry + 16), u32(entry + 36)))

    def offset(rva: int, size: int, *, executable: bool = False) -> int:
        matches = [s for s in sections if s[0] <= rva < s[0] + max(s[1], s[3])]
        if len(matches) != 1:
            raise ValueError("unmapped or ambiguous compatibility export RVA")
        address, _, raw, length, flags = matches[0]
        delta = rva - address
        if delta + size > length or executable and not flags & 0x20000000:
            raise ValueError("compatibility export is not backed by executable file bytes")
        span(raw + delta, size)
        return raw + delta

    directory_rva, directory_size = u32(optional + 96), u32(optional + 100)
    if not directory_rva or directory_size < 40:
        raise ValueError("compatibility DLL has no export directory")
    directory = offset(directory_rva, 40)
    functions, names = u32(directory + 20), u32(directory + 24)
    if not 0 < names <= functions <= 65536:
        raise ValueError("invalid compatibility DLL export counts")
    function_table = offset(u32(directory + 28), functions * 4)
    name_table = offset(u32(directory + 32), names * 4)
    ordinal_table = offset(u32(directory + 36), names * 2)
    found = []
    for index in range(names):
        name_rva = u32(name_table + index * 4)
        start = offset(name_rva, 1)
        end = original.find(b"\0", start, min(len(original), start + 256))
        if end < 0:
            raise ValueError("unterminated compatibility export name")
        offset(name_rva, end - start + 1)
        if original[start:end] == export.encode("ascii"):
            ordinal = u16(ordinal_table + index * 2)
            if ordinal >= functions:
                raise ValueError("invalid compatibility export ordinal")
            found.append(u32(function_table + ordinal * 4))
    if len(found) != 1:
        raise ValueError(f"compatibility DLL needs exactly one {export} export")
    rva = found[0]
    if directory_rva <= rva < directory_rva + directory_size:
        raise ValueError("forwarded compatibility exports cannot be patched")
    replacement = b"\xb8" + struct.pack("<I", value) + b"\xc3"
    at = offset(rva, len(replacement), executable=True)
    patched = original[:at] + replacement + original[at + len(replacement):]
    path.write_bytes(patched)
    return {"before_sha256": before, "after_sha256": hashlib.sha256(patched).hexdigest(),
            "export": export, "rva": rva, "file_offset": at, "return_u32": value,
            "bytes_changed": sum(a != b for a, b in zip(original, patched, strict=True))}


def apply_compatibility(installed: Path, plan: dict) -> dict:
    """Modify only the installed private copy and record every resulting hash."""
    records = []
    root = installed.resolve()
    for patch in plan["patches"]:
        name = patch["path"]
        path = root / name
        if (Path(name).is_absolute() or ".." in Path(name).parts
                or not path.resolve().is_relative_to(root)):
            raise ValueError("compatibility patch escapes private installation")
        record = patch_pe32_return(path, patch["source_sha256"], patch["export"],
                                   patch["return_u32"])
        records.append({"path": name, **record})
    return {"id": plan["id"], "reason": plan["reason"], "patches": records,
            "original_game_unmodified": False}


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def read_replay(path: Path, stage: tuple[int, int] = (640, 480)) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict) or set(value) != {
        "schema_version",
        "id",
        "clock",
        "duration_ms",
        "actions",
    }:
        raise ValueError("replay has missing or unsupported fields")
    if (
        type(value["schema_version"]) is not int
        or value["schema_version"] != SCHEMA
        or not isinstance(value["id"], str)
        or not IDENTIFIER.fullmatch(value["id"])
    ):
        raise ValueError("replay requires schema_version=1 and a safe id")
    if value.get("clock") != "host_monotonic_ms":
        raise ValueError("original runner currently requires clock=host_monotonic_ms")
    duration = value.get("duration_ms")
    if type(duration) is not int or not 1000 <= duration <= 600_000:
        raise ValueError("duration_ms must be between 1000 and 600000")
    previous = -1
    checkpoint_ids = set()
    if not isinstance(value["actions"], list) or len(value["actions"]) > 10000:
        raise ValueError("actions must be a list containing at most 10000 entries")
    for action in value.get("actions", []):
        if not isinstance(action, dict):
            raise ValueError("actions must be objects")
        at = action.get("at_ms")
        if type(at) is not int or not previous <= at <= duration:
            raise ValueError("actions must be in timestamp order within duration_ms")
        previous = at
        kind = action.get("type")
        fields = {
            "pointer": {"x", "y"},
            "button_down": {"button"},
            "button_up": {"button"},
            "key": {"key"},
            "checkpoint": {"id"},
        }
        if not isinstance(kind, str) or kind not in fields:
            raise ValueError(f"unsupported action type: {kind}")
        if set(action) != {"at_ms", "type"} | fields[kind]:
            raise ValueError("action has missing or unsupported fields")
        if kind == "pointer":
            if any(type(action.get(k)) is not int for k in ("x", "y")):
                raise ValueError("pointer x/y must be integers")
            if not 0 <= action["x"] < stage[0] or not 0 <= action["y"] < stage[1]:
                raise ValueError(f"pointer must remain within {stage[0]}x{stage[1]}")
        elif kind in {"button_down", "button_up"}:
            if action.get("button") not in ("left", "right"):
                raise ValueError("button must be left or right")
        elif kind == "key":
            key = action.get("key")
            if not isinstance(key, str) or not (key in (
                "Return", "Escape", "space", "Tab", "BackSpace", "Left", "Right", "Up", "Down"
            ) or re.fullmatch(r"[a-zA-Z0-9]", key)):
                raise ValueError("unsupported key")
        elif kind == "checkpoint":
            identifier = action.get("id", "")
            if (
                not isinstance(identifier, str)
                or not IDENTIFIER.fullmatch(identifier)
                or identifier in checkpoint_ids
            ):
                raise ValueError("checkpoint ids must be safe and unique")
            checkpoint_ids.add(identifier)
        else:
            raise ValueError(f"unsupported action type: {kind}")
    if not checkpoint_ids:
        raise ValueError("replay must include a checkpoint")
    return value


def command(argv: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(argv, check=True, text=True, capture_output=True, **kwargs)


def capture(args: argparse.Namespace) -> int:
    # Host-only import: the isolated Python 3.11 container runs _capture, and
    # deliberately has no access to the repository's host conversion package.
    from director64.media import extract_source
    from director64.project import GameSpec
    from director64.reference import layout_for

    game_spec = GameSpec.load(args.game, args.edition)
    game_spec.activate()
    source = (args.source or game_spec.work / "extracted").resolve()
    iso = (args.iso or game_spec.media).resolve(strict=True)
    if iso != game_spec.media.resolve():
        raise ValueError("--iso must identify the selected, hash-pinned game image")
    extract_source(game_spec, source)
    inventory = None
    if game_spec.source["kind"] == "iso9660":
        from director64.iso import inventory_report

        with iso.open("rb") as handle:
            inventory = inventory_report(handle)
    layout = layout_for(game_spec, source, iso=iso, windows_version=args.windows_version,
                        inventory=inventory, unmodified=args.unmodified)
    experiment_source = None
    if args.experiment:
        if not IDENTIFIER.fullmatch(args.experiment):
            raise ValueError("experiment must name a tools/reference synthetic Lingo probe")
        probe = game_spec.root / "tools/reference" / (args.experiment + ".lingo")
        experiment_source = probe.read_text(encoding="ascii")
        result_name, io_mode = experiment_output(experiment_source)
        layout["experiment"] = {"name": args.experiment, "result": result_name, "io_mode": io_mode,
                                "source_sha256": digest(probe),
                                "script_mode": ("full-script" if experiment_source.startswith(
                                    FULL_SCRIPT_MARKER + "\n") else "startup")}
    if inventory:
        layout["aliases"] = inventory.get("aliases", {})
    replay_path = (args.replay or game_spec.directory / "tests/reference/boot.json").resolve()
    if not args.replay and not replay_path.is_file():
        replay_path = game_spec.work / "reference" / "inputs" / "boot.json"
        replay_path.parent.mkdir(parents=True, exist_ok=True)
        write_json(replay_path, {
            "schema_version": 1, "id": f"{game_spec.slug}-boot", "clock": "host_monotonic_ms",
            "duration_ms": 15_000, "actions": [
                {"at_ms": 10_000, "type": "checkpoint", "id": "boot"}]})
    replay = read_replay(replay_path, tuple(layout["stage"]))
    output = (
        args.output
        or game_spec.work
        / "reference"
        / (datetime.now(UTC).strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:8])
    ).resolve()
    # Prefixes, copied saves, and licensed captures must stay in the ignored local tree.
    if not output.is_relative_to(game_spec.work / "reference"):
        raise ValueError(
            "output must be inside the selected source workspace’s reference directory"
        )
    output.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(replay_path, output / "replay.json")
    if layout.get("memory_report_mib") is not None:
        report = output / "legacy-meminfo"
        report.write_text(legacy_meminfo(layout["memory_report_mib"]), encoding="ascii")
        layout["memory_environment"] = {"kind": "private-wine-proc-meminfo",
            "reported_ram_mib": layout["memory_report_mib"],
            "reported_swap_mib": layout["memory_report_mib"],
            "report_sha256": digest(report), "game_files_modified": False,
            "actual_container_memory_limit": "host-default", "process_affinity": "host-default"}
    write_json(output / "layout.json", layout)
    if layout.get("optical_drive"):
        prepare_optical_source(source, output / "disc", layout)
    if experiment_source is not None:
        (output / "experiment.lingo").write_text(experiment_source, encoding="ascii")
    runner_directory = output / "runner"
    runner_directory.mkdir()
    shutil.copyfile(Path(__file__), runner_directory / "reference_runner.py")
    image = json.loads(
        command([os.environ.get("DOCKER") or "docker", "image", "inspect", args.image]).stdout
    )[0]
    source_hashes = {
        path.relative_to(source).as_posix(): digest(path)
        for path in sorted(source.rglob("*"))
        if path.is_file()
    }
    metadata = {
        "schema_version": SCHEMA,
        "kind": ("compatibility-reference-capture" if layout.get("compatibility")
                 else "original-reference-capture"),
        "original_game_unmodified": not bool(layout.get("compatibility")),
        "run_id": output.name,
        "created_utc": datetime.now(UTC).isoformat(),
        "runner": {
            "name": "wine-xvfb",
            "verified": False,
            "image_id": image["Id"],
            "image_name": args.image,
            "script_sha256": digest(runner_directory / "reference_runner.py"),
            "container_init": True,
            "windows_version": args.windows_version,
            "display": {"width": layout["stage"][0], "height": layout["stage"][1], "depth": 24},
        },
        "source": {"iso_sha256": digest(iso), "files": source_hashes},
        "game": game_spec.slug,
        "declared_director_version": game_spec.data["port"]["director_version"],
        "layout": layout,
        "replay": {"id": replay["id"], "sha256": digest(replay_path)},
        "limitations": [
            "Wine is not qualified as equivalent to the original supported Windows runtime.",
            "Input uses host monotonic time; semantic movie/frame synchronization is unobserved.",
            "Original random seed, state hashes, sprite state and Director ticks are unobserved.",
            "No scene tolerance or fidelity claim is derived from successful media capture.",
        ],
    }
    if layout.get("memory_environment"):
        metadata["runner"]["memory_environment"] = layout["memory_environment"]
        metadata["limitations"].append(
            "Wine reads an explicitly declared private legacy memory report; actual "
            "container memory limits and affinity remain at host defaults.")
    if layout.get("compatibility"):
        metadata["limitations"].append(
            "The private installation uses a declared protection compatibility patch; "
            "this is not an unmodified original-game capture.")
    if layout.get("prerequisites"):
        metadata["limitations"].append(
            "Pinned original-disc installers ran first in a prefix on a 1.5 GiB tmpfs, "
            "discarded after the capture.")
    write_json(output / "provenance.json", metadata)
    container = "director64-reference-" + uuid.uuid4().hex[:12]
    argv = [
        (os.environ.get("DOCKER") or "docker"),
        "run",
        "--rm",
        "--init",
        "--name",
        container,
        "--network",
        "none",
        "--user",
        f"{os.getuid()}:{os.getgid()}",
        "--cap-drop",
        "ALL",
        "--security-opt",
        "no-new-privileges",
        "--pids-limit",
        "512",
        "--env",
        "WINEPREFIX=/session/wine",
        "--env",
        "XDG_CACHE_HOME=/session/cache",
        "--env",
        "XDG_CONFIG_HOME=/session/config",
        "--env",
        "XDG_RUNTIME_DIR=/session/runtime",
        "--env",
        "PULSE_SERVER=unix:/session/pulse.sock",
        "--env",
        "PULSE_SINK=reference",
        "--env",
        "PULSE_SOURCE=reference.monitor",
        "--env",
        "OMP_NUM_THREADS=1",
        "--env",
        "MAGICK_THREAD_LIMIT=1",
        "--volume",
        f"{source}:/media/findus:ro",
        "--volume",
        f"{runner_directory}:/runner:ro",
        "--volume",
        f"{output}:/session",
    ]
    if layout.get("memory_report_mib") is not None:
        argv += ["--volume", f"{output / 'legacy-meminfo'}:/proc/meminfo:ro"]
    if layout.get("prerequisites"):
        # Legacy installers keep free space in 32 bits: a multi-terabyte host
        # disk wraps to "not enough space". The prefix lives on a small tmpfs.
        argv += ["--tmpfs", f"/prefix:rw,exec,size=1536m,uid={os.getuid()},"
                            f"gid={os.getgid()},mode=0700"]
    if layout.get("optical_drive"):
        argv += ["--volume", f"{output / 'disc'}:/media/disc:ro"]
        if layout["optical_drive"]["raw_image"]:
            argv += ["--volume", f"{iso}:/media/original.iso:ro"]
    argv += [image["Id"], "--windows-version", args.windows_version]
    try:
        with (output / "container.log").open("w") as log:
            result = subprocess.run(
                argv,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=replay["duration_ms"] / 1000 + 180,
                check=False,
            )
    except INTERRUPT_ERRORS:
        subprocess.run(
            [(os.environ.get("DOCKER") or "docker"), "stop", "--time", "3", container],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        print(f"Capture interrupted; diagnostics retained in {output}", file=sys.stderr)
        return 1
    if not (output / "capture.json").is_file():
        write_json(
            output / "capture.json",
            {
                "schema_version": SCHEMA,
                "capture_complete": False,
                "runner_verified": False,
                "semantic_assertions": [],
                "error": f"container exited {result.returncode} without a capture record",
            },
        )
    print(output / "capture.json")
    if args.experiment and not result.returncode:
        from director64.reference import compare_experiment

        return compare_experiment(game_spec, args.experiment, output)
    return result.returncode


def visible_windows() -> list[dict]:
    found = subprocess.run(
        ["xdotool", "search", "--onlyvisible", "--name", ".*"],
        capture_output=True,
        text=True,
        check=False,
    )
    windows = []
    for identifier in found.stdout.splitlines():
        name = subprocess.run(
            ["xdotool", "getwindowname", identifier], capture_output=True, text=True, check=False
        ).stdout.strip()
        if name:
            windows.append({"id": identifier, "name": name})
    return windows


def playback_process(maps: str) -> bool:
    """Early projectors embed the engine in the mapped retail executable."""
    names = maps.lower()
    return (any(name in names for name in ("dirapi.dll", "iml32.dll"))
            or any("/drive_c/original/" in row and row.endswith(".exe")
                   for row in names.splitlines()))


def loaded_modules() -> dict:
    """Hash mapped playback engines as well as their thin launcher."""
    modules = {}
    for process in Path("/proc").glob("[0-9]*"):
        try:
            maps = (process / "maps").read_text()
        except (OSError, UnicodeError):
            continue
        if not playback_process(maps):
            continue
        for row in maps.splitlines():
            fields = row.split(None, 5)
            if len(fields) != 6:
                continue
            path = Path(fields[5])
            if (path.name.lower() not in ("dirapi.dll", "iml32.dll", "proj.dll")
                    and path.suffix.lower() != ".exe"):
                continue
            if str(path) in modules or not path.is_file():
                continue
            data = path.read_bytes()
            strings = [s.decode("utf-16le") for s in re.findall(rb"(?:[\x20-\x7e]\x00){5,}", data)]
            versions = [strings[i + 1] for i, s in enumerate(strings[:-1]) if s == "FileVersion"]
            modules[str(path)] = {"sha256": digest(path), "file_versions": sorted(set(versions))}
    return modules


def install_aliases(root: Path, aliases: dict) -> None:
    """Present verified long/short component combinations inside a private copy."""
    root = root.resolve()
    for long_name, short_name in sorted(aliases.items()):
        parts = Path(long_name).parts
        short_parts = Path(short_name).parts
        if (not parts or not short_parts
                or Path(long_name).is_absolute() or Path(short_name).is_absolute()
                or ".." in parts or ".." in short_parts
                or any(c in name for name in (long_name, short_name) for c in ("\\", ":", "\0"))):
            raise ValueError("disc alias escapes installed source")
        source = root / short_name
        if not source.resolve().is_relative_to(root):
            raise ValueError("disc alias source escapes installed source")
        if not source.is_file():
            raise ValueError(f"disc alias source is absent: {short_name}")
        candidates = [parts]
        if len(parts) == len(short_parts):
            choices, variants = [], 1
            for pair in zip(parts, short_parts, strict=True):
                names = {}
                for name in pair:
                    names.setdefault(name.casefold(), name)
                choices.append(tuple(names.values()))
                variants *= len(names)
                if variants > MAX_DISC_PATH_VARIANTS:
                    raise ValueError(f"disc alias needs more than {MAX_DISC_PATH_VARIANTS} "
                                     f"mixed-name variants: {short_name} -> {long_name}")
            candidates = product(*choices)
        source_hash = None
        for candidate in candidates:
            parent = root
            for name in candidate[:-1]:
                matches = [p for p in parent.iterdir() if p.name.casefold() == name.casefold()]
                if len(matches) > 1:
                    raise ValueError(f"conflicting disc alias parents: {parent / name}")
                parent = matches[0] if matches else parent / name
                if not parent.resolve().is_relative_to(root):
                    raise ValueError("disc alias parent escapes installed source")
                if matches and not parent.is_dir():
                    raise ValueError(f"conflicting disc alias parent: {parent}")
                parent.mkdir(exist_ok=True)
            matches = [p for p in parent.iterdir()
                       if p.name.casefold() == candidate[-1].casefold()]
            for target in matches:
                if not target.resolve().is_relative_to(root):
                    raise ValueError("disc alias target escapes installed source")
                if not target.is_file() or target.stat().st_size != source.stat().st_size:
                    raise ValueError(f"conflicting disc alias target: {target} -> {source}")
                if not target.samefile(source):
                    if source_hash is None:
                        source_hash = digest(source)
                    if digest(target) != source_hash:
                        raise ValueError(f"conflicting disc alias target: {target} -> {source}")
            if not matches:
                (parent / candidate[-1]).symlink_to(os.path.relpath(source, parent))


def legacy_meminfo(mebibytes: int) -> str:
    """A declared Wine API report, independent of actual resource limits."""
    if type(mebibytes) is not int or not 16 <= mebibytes <= 1024:
        raise ValueError("legacy memory report must be16..1024MiB")
    total = mebibytes * 1024
    return (f"MemTotal: {total} kB\nMemFree: {total // 2} kB\n"
            f"SwapTotal: {total} kB\nSwapFree: {total // 2} kB\n"
            "Buffers: 0 kB\nCached: 0 kB\nSReclaimable: 0 kB\n")


def prepare_optical_source(source: Path, output: Path, layout: dict) -> None:
    """A private CD namespace; the container mounts this copy read-only."""
    optical = layout["optical_drive"]
    if (not re.fullmatch(r"[D-Y]", optical.get("letter", ""))
            or not re.fullmatch(r"[0-9a-f]{8}", optical.get("serial_hex", ""))
            or optical.get("type") != "cdrom"
            or optical.get("serial_algorithm") != "wine-8.0-iso9660-descriptor-checksum"
            or type(optical.get("raw_image")) is not bool
            or not isinstance(optical.get("label"), str)
            or not optical["label"]
            or any(ord(c) < 32 for c in optical["label"])):
        raise ValueError("invalid source-derived optical drive metadata")
    shutil.copytree(source, output)
    install_aliases(output, layout.get("aliases", {}))
    (output / ".windows-label").write_text(optical["label"] + "\n", encoding="utf-8")
    (output / ".windows-serial").write_text(optical["serial_hex"] + "\n", encoding="ascii")


def map_optical_drive(dosdevices: Path, optical: dict, *, mount: Path = Path("/media/disc"),
                      image: Path = Path("/media/original.iso")) -> None:
    """Map only the declared optical letter inside a private Wine prefix."""
    letter = optical.get("letter", "")
    if not re.fullmatch(r"[D-Y]", letter) or type(optical.get("raw_image")) is not bool:
        raise ValueError("invalid optical drive mapping")
    if not mount.is_dir() or (optical["raw_image"] and not image.is_file()):
        raise ValueError("original optical drive backing is absent")
    for name, target in ((letter.lower() + ":", mount),
                         (letter.lower() + "::", image if optical["raw_image"] else None)):
        link = dosdevices / name
        if link.exists() or link.is_symlink():
            if not link.is_symlink():
                raise ValueError(f"optical mapping would replace a non-link: {link}")
            link.unlink()
        if target:
            link.symlink_to(target)


def installed_file(root: Path, name: str) -> Path:
    path = root
    for part in Path(name).parts:
        matches = [p for p in path.iterdir() if p.name.casefold() == part.casefold()]
        path = matches[0] if matches else path / part
        if part != Path(name).parts[-1]:
            path.mkdir(exist_ok=True)
    return path


def stop(process: subprocess.Popen, graceful: bool = False) -> None:
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGINT if graceful else signal.SIGTERM)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def container_capture(args: argparse.Namespace) -> int:
    session = Path("/session")
    layout = json.loads((session / "layout.json").read_text())
    width, height = layout["stage"]
    replay = read_replay(session / "replay.json", (width, height))
    processes = []
    logs = []
    checkpoints = []
    performed = []
    result = {
        "schema_version": SCHEMA,
        "capture_complete": False,
        "semantic_assertions": [],
        "runner_verified": False,
    }

    def launch(argv: list[str], log_name: str, **kwargs) -> subprocess.Popen:
        log = (session / log_name).open("w")
        logs.append(log)
        process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, **kwargs)
        processes.append(process)
        return process

    try:
        for directory in ("runtime", "cache", "config"):
            (session / directory).mkdir(mode=0o700)
        if layout.get("prerequisites"):
            Path("/prefix/wine").mkdir()
            (session / "wine").symlink_to("/prefix/wine")
        launch(["Xvfb", ":99", "-screen", "0", f"{width}x{height}x24", "-nolisten", "tcp"],
               "xvfb.log")
        deadline = time.monotonic() + 10
        while not Path("/tmp/.X11-unix/X99").exists():
            if time.monotonic() > deadline:
                raise RuntimeError("Xvfb startup timeout")
            time.sleep(0.05)
        boot = command(["wineboot", "-u"], timeout=90)
        (session / "wineboot.log").write_text(boot.stdout + boot.stderr, encoding="utf-8")
        command(
            [
                "wine",
                "reg",
                "add",
                r"HKCU\Software\Wine",
                "/v",
                "Version",
                "/d",
                args.windows_version,
                "/f",
            ],
            timeout=30,
        )
        installed = session / "wine" / "drive_c" / "Original"
        if layout["install"] == "workshop":
            installed.mkdir()
            shutil.copy2("/media/findus/ANNAT/PETT16.EXE", installed / "Findus.exe")
            shutil.copytree("/media/findus/ANNAT/DATA", installed / "DATA")
        else:
            shutil.copytree("/media/findus", installed)
            install_aliases(installed, layout.get("aliases", {}))
        if layout.get("compatibility"):
            result["compatibility"] = apply_compatibility(installed, layout["compatibility"])
            write_json(session / "compatibility.json", result["compatibility"])
        game = installed / layout["cwd"]
        for name, content in layout["files"].items():
            target = installed_file(game, name)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding="cp1252")
        if layout.get("experiment"):
            body = (session / "experiment.lingo").read_text(encoding="ascii")
            installed_file(game, "LINGO.INI").write_bytes(
                experiment_lingo(body).replace("\n", "\r").encode("ascii"))
        fonts = session / "wine" / "drive_c" / "windows" / "Fonts"
        for path in installed.rglob("*"):
            if path.is_file() and path.suffix.lower() in {".ttf", ".otf"}:
                shutil.copy2(path, fonts / path.name)
        for item in layout.get("prerequisites", []):
            # A pinned original installer, before any retail launch.
            installer = "C:\\Original\\" + item["path"].replace("/", "\\")
            completed = subprocess.run(["wine", installer, *item["arguments"]], timeout=300,
                                       capture_output=True, text=True, check=False)
            # Self-extracting installers return before their child finishes.
            waited = subprocess.run(["wineserver", "-w"], timeout=300, check=False)
            with (session / "prerequisites.log").open("a") as log:
                log.write(f"{installer} {item['arguments']}: exit {completed.returncode}, "
                          f"wineserver {waited.returncode}\n{completed.stdout}{completed.stderr}\n")
            result.setdefault("prerequisites", []).append(
                {**item, "exit_code": completed.returncode})
        result["initial_save_hashes"] = {
            path.name: digest(path) for path in sorted((game / "DATA").glob("*.TXT"))
        }
        if layout.get("optical_drive"):
            optical = layout["optical_drive"]
            map_optical_drive(session / "wine/dosdevices", optical)
            command(["wine", "reg", "add", r"HKLM\Software\Wine\Drives", "/v",
                     optical["letter"].lower() + ":", "/d", "cdrom", "/f"], timeout=30)
        else:
            (session / "wine" / "dosdevices" / "d:").symlink_to("/media/findus")
        pulse = launch(
            [
                "pulseaudio",
                "-n",
                "--daemonize=no",
                "--exit-idle-time=-1",
                "--load=module-native-protocol-unix auth-anonymous=1 socket=/session/pulse.sock",
                "--load=module-null-sink sink_name=reference rate=44100 channels=2",
            ],
            "pulse.log",
        )
        deadline = time.monotonic() + 10
        while not (session / "pulse.sock").exists():
            if pulse.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("isolated PulseAudio startup failed; inspect pulse.log")
            time.sleep(0.05)
        command(["pactl", "set-default-sink", "reference"])
        command(
            [
                "wine",
                "reg",
                "add",
                r"HKCU\Software\Wine\Drivers",
                "/v",
                "Audio",
                "/d",
                "pulse",
                "/f",
            ],
            timeout=30,
        )
        if layout.get("optical_drive"):
            # Re-enumerate private-prefix devices after setting their type and
            # raw image mapping. No retail process has been launched yet.
            stopped_server = subprocess.run(["wineserver", "-k"], timeout=15,
                                            capture_output=True, text=True, check=False)
            # Wine's idle server may already have exited after the registry
            # clients. Exit 1 with no diagnostics means there is no server.
            if stopped_server.returncode not in {0, 1} or stopped_server.stderr.strip():
                raise RuntimeError("private Wine server could not restart: "
                                   + stopped_server.stderr.strip())
            command(["wineserver", "-w"], timeout=15)
            command(["wineboot", "-u"], timeout=90)
            volume = command(["wine", "cmd", "/c", "vol", optical["letter"] + ":"], timeout=30)
            result["optical_drive"] = {**optical, "volume_information": volume.stdout.strip(),
                                       "server_stop_code": stopped_server.returncode,
                                       "namespace_readonly": True,
                                       "raw_image_readonly": optical["raw_image"]}
        recorder = launch(
            [
                "ffmpeg",
                "-nostdin",
                "-hide_banner",
                "-loglevel",
                "warning",
                "-thread_queue_size",
                "512",
                "-f",
                "x11grab",
                # The host pointer is not stage content; image comparisons
                # would report it as a difference in every frame.
                "-draw_mouse",
                "0",
                "-video_size",
                f"{width}x{height}",
                "-framerate",
                "30",
                "-i",
                ":99.0",
                "-thread_queue_size",
                "512",
                "-f",
                "pulse",
                "-sample_rate",
                "44100",
                "-channels",
                "2",
                "-i",
                "reference.monitor",
                "-c:v",
                "ffv1",
                "-threads",
                "2",
                "-level",
                "3",
                "-pix_fmt",
                "bgr0",
                "-c:a",
                "pcm_s16le",
                str(session / "original.mkv"),
            ],
            "ffmpeg.log",
        )
        time.sleep(0.5)
        if recorder.poll() is not None:
            raise RuntimeError("lossless recorder failed to start; inspect ffmpeg.log")
        launcher = "C:\\Original\\" + layout["launcher"].replace("/", "\\")
        launch(["wine", launcher], "wine.log", cwd=game)
        epoch = time.monotonic()
        result["tools"] = {
            name: command(argv).stdout.splitlines()[0]
            for name, argv in {
                "wine": ["wine", "--version"],
                "ffmpeg": ["ffmpeg", "-version"],
            }.items()
        }
        for action in replay["actions"]:
            delay = epoch + action["at_ms"] / 1000 - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            actual = round((time.monotonic() - epoch) * 1000, 3)
            kind = action["type"]
            if kind == "pointer":
                command(["xdotool", "mousemove", str(action["x"]), str(action["y"])])
            elif kind in {"button_down", "button_up"}:
                command(
                    [
                        "xdotool",
                        "mousedown" if kind == "button_down" else "mouseup",
                        "1" if action["button"] == "left" else "3",
                    ]
                )
            elif kind == "key":
                command(["xdotool", "key", action["key"]])
            else:
                png = session / f"{action['id']}.png"
                command(["import", "-window", "root", str(png)])
                checkpoints.append(
                    {
                        "id": action["id"],
                        "at_ms": actual,
                        "path": png.name,
                        "sha256": digest(png),
                        "windows": visible_windows(),
                        "semantic_status": "unobserved",
                    }
                )
                result["loaded_modules"] = {**result.get("loaded_modules", {}), **loaded_modules()}
            performed.append({**action, "actual_at_ms": actual})
        delay = epoch + replay["duration_ms"] / 1000 - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        stop(recorder, graceful=True)
        if recorder.returncode not in {0, 255}:
            raise RuntimeError(f"recorder exited {recorder.returncode}; inspect ffmpeg.log")
        probe = json.loads(
            command(
                [
                    "ffprobe",
                    "-v",
                    "error",
                    "-count_frames",
                    "-show_streams",
                    "-show_format",
                    "-of",
                    "json",
                    str(session / "original.mkv"),
                ]
            ).stdout
        )
        video = next(item for item in probe["streams"] if item["codec_type"] == "video")
        audio = next(item for item in probe["streams"] if item["codec_type"] == "audio")
        if (video["codec_name"], video["width"], video["height"]) != ("ffv1", width, height):
            raise RuntimeError(f"capture must contain lossless {width}x{height} FFV1 video")
        if audio["codec_name"] != "pcm_s16le" or int(video["nb_read_frames"]) < 1:
            raise RuntimeError("capture must contain decoded video and PCM audio")
        command(
            [
                "ffmpeg",
                "-v",
                "error",
                "-i",
                str(session / "original.mkv"),
                "-map",
                "0:a:0",
                "-c:a",
                "copy",
                str(session / "original.wav"),
            ]
        )
        write_json(session / "media.json", probe)
        if layout.get("experiment"):
            observed = session / "wine/drive_c" / layout["experiment"]["result"]
            if not observed.is_file():
                raise RuntimeError("original engine produced no synthetic experiment result")
            contents = observed.read_text(encoding="ascii")
            if "DIRECTOR64_PROBE_COMPLETE" not in contents:
                raise RuntimeError("original synthetic experiment did not complete")
            (session / "experiment-result.txt").write_text(
                contents.replace("DIRECTOR64_PROBE_COMPLETE\n", ""), encoding="ascii")
            result["experiment_complete"] = True
            result["experiment"] = layout["experiment"]
        result.update(
            {
                "capture_complete": True,
                "media": {
                    name: digest(session / name)
                    for name in ("original.mkv", "original.wav", "media.json")
                },
            }
        )
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
        if isinstance(error, subprocess.CalledProcessError):
            result["command_stderr"] = error.stderr
    finally:
        for process in reversed(processes):
            stop(process)
        for log in logs:
            log.close()
        result["checkpoints"] = checkpoints
        result["actions"] = performed
        data = session / "wine" / "drive_c" / "Original" / "DATA"
        result["final_save_hashes"] = {
            path.name: digest(path) for path in sorted(data.glob("*.TXT"))
        }
        result["logs"] = {
            path.name: digest(path)
            for path in session.glob("*.log")
            if path.name != "container.log"
        }
        write_json(session / "capture.json", result)
    return 0 if result["capture_complete"] else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    host = sub.add_parser("capture", help="run a new capture with a fresh private Wine prefix")
    host.add_argument("--game", default="findus-workshop")
    host.add_argument("--edition")
    host.add_argument("--experiment", help="run tools/reference/<name>.lingo at startup")
    host.add_argument("--unmodified", action="store_true",
                      help="disable declared private-copy compatibility patches")
    host.add_argument("--source", type=Path)
    host.add_argument("--iso", type=Path)
    host.add_argument("--replay", type=Path)
    host.add_argument(
        "--image", default=os.getenv("DIRECTOR64_REFERENCE_IMAGE", "director64-reference:local")
    )
    host.add_argument(
        "--output",
        type=Path,
        default=None,
    )
    host.add_argument(
        "--windows-version", choices=("win31", "win95", "win98", "win7"), default="win95"
    )
    internal = sub.add_parser("_capture", help=argparse.SUPPRESS)
    internal.add_argument("--windows-version", default="win95")
    args = parser.parse_args()
    try:
        return capture(args) if args.operation == "capture" else container_capture(args)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"reference capture: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

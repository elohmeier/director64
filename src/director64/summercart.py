"""Copy a validated game ROM to a SummerCart SD card, mounting it with udisksctl
(or the mount-media/umount-media wrappers when they are on PATH)."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path

from .project import GameSpec, repository, selected_game
from .rom import ROM_TARGET_MIB, validate_header, validate_size


@dataclass(frozen=True)
class RomCopy:
    source: Path
    data: bytes
    filename: str
    metadata: dict[str, str]
    artwork: bytes | None = None


def all_games() -> list[GameSpec]:
    """Executable ports, each using its declared default source."""
    games = []
    for path in sorted(repository().glob("games/*/game.toml")):
        game = GameSpec.load(path.parent.name)
        if game.data["status"] in {"supported", "experimental"}:
            game.require_port()
            games.append(game)
    if not games:
        raise ValueError("no executable games found")
    return games


def prepare_rom(game: GameSpec, source: Path, artwork_path: Path | None = None) -> RomCopy:
    filename = game.slug + ".z64"
    release_budget_mib = game.data["port"].get("rom_budget_mib", ROM_TARGET_MIB)
    try:
        validate_size(source.stat().st_size, release_budget_mib=release_budget_mib)
        data = source.read_bytes()
        metadata = validate_header(
            data,
            title=game.data["port"]["rom_title"],
            save_type=game.data["port"]["rom_save_type"],
            controller_count=game.data["port"].get("controller_count", 1),
            release_budget_mib=release_budget_mib,
        )
    except (OSError, ValueError) as error:
        raise ValueError(f"{source}: {error}") from error
    artwork = None
    if artwork_path is not None:
        if artwork_path.stat().st_size > 1024 * 1024:
            raise ValueError("artwork PNG exceeds 1 MiB")
        artwork = artwork_path.read_bytes()
        validate_artwork(artwork)
    return RomCopy(source, data, filename, metadata, artwork)


def partition_for(devices: list[dict], requested: str | None = None) -> str:
    if requested is None:
        candidates = [
            partition["path"]
            for disk in devices
            if disk["type"] == "disk" and disk["rm"]
            for partition in disk.get("children") or [disk]
            if partition.get("fstype") in {"vfat", "exfat"}
        ]
        if not candidates:
            raise ValueError("no removable FAT/exFAT partition found; connect the SD card")
        if len(candidates) != 1:
            raise ValueError(
                "multiple removable FAT/exFAT partitions found: "
                + ", ".join(candidates)
                + "; select one with --device PATH"
            )
        return candidates[0]
    for disk in devices:
        children = disk.get("children", [])
        matches = [p for p in [disk, *children] if p["path"] == requested]
        if not matches:
            continue
        if disk["type"] != "disk" or not disk["rm"]:
            raise ValueError("target must belong to a removable disk")
        selected = matches[0]
        candidates = children if selected is disk and children else [selected]
        candidates = [p for p in candidates if p.get("fstype") in {"vfat", "exfat"}]
        if len(candidates) != 1:
            raise ValueError("select exactly one FAT/exFAT partition with --device")
        return candidates[0]["path"]
    raise ValueError(f"device is absent or is not a disk/partition: {requested}")


def mountpoint(partition: str) -> Path | None:
    result = subprocess.run(
        ["findmnt", "--json", "--source", partition, "--output", "TARGET"],
        capture_output=True,
        text=True,
    )
    if result.returncode == 1 and not result.stdout.strip():
        return None
    result.check_returncode()
    mounts = json.loads(result.stdout).get("filesystems", [])
    if len(mounts) != 1:
        raise ValueError(f"expected one mountpoint for {partition}")
    return Path(mounts[0]["target"])


def copy_rom(data: bytes, target: Path, filename: str) -> str:
    if Path(filename).name != filename or not filename.endswith(".z64"):
        raise ValueError("ROM destination must be a .z64 basename")
    return copy_file(data, target, filename)


def copy_file(data: bytes, target: Path, filename: str) -> str:
    """Atomically replace one preselected file after verifying the temporary copy."""
    if Path(filename).name != filename:
        raise ValueError("copy destination must be a basename")
    destination = target / filename
    if destination.is_symlink() or (destination.exists() and not destination.is_file()):
        raise ValueError(f"destination is not a regular file: {destination}")
    if shutil.disk_usage(target).free < len(data):
        raise ValueError("SD card needs enough free space for a complete temporary copy")
    expected = hashlib.sha256(data).hexdigest()
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            prefix=".director64-", suffix=".tmp", dir=target, delete=False
        ) as output:
            temporary = Path(output.name)
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != expected:
            raise OSError("copied file checksum differs; existing file was not replaced")
        # Keep the previous file until the complete replacement has been verified.
        os.replace(temporary, destination)
        subprocess.run(["sync", "--file-system", str(target)], check=True)
        if hashlib.sha256(destination.read_bytes()).hexdigest() != expected:
            raise OSError("final file checksum differs")
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return expected


def validate_artwork(data: bytes) -> None:
    if len(data) > 1024 * 1024 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("artwork must be a PNG of at most 1 MiB")
    offset = 8
    kinds = []
    while offset + 12 <= len(data):
        length, kind = struct.unpack_from(">I4s", data, offset)
        end = offset + 12 + length
        if end > len(data):
            raise ValueError("truncated artwork PNG")
        checksum = struct.unpack_from(">I", data, end - 4)[0]
        if zlib.crc32(data[offset + 4 : end - 4]) != checksum:
            raise ValueError("artwork PNG CRC mismatch")
        if not kinds:
            if kind != b"IHDR" or length != 13:
                raise ValueError("artwork PNG is missing its header")
            dimensions = struct.unpack_from(">II", data, offset + 8)
            if dimensions not in {(158, 112), (112, 158)}:
                raise ValueError("artwork must be 158x112 or 112x158 pixels")
        kinds.append(kind)
        offset = end
        if kind == b"IEND":
            if length or offset != len(data) or b"IDAT" not in kinds:
                raise ValueError("invalid artwork PNG end")
            return
    raise ValueError("incomplete artwork PNG")


def copy_artwork(data: bytes, target: Path, title: str) -> str:
    if not title or title in {".", ".."} or any(c in title for c in "/\\\0"):
        raise ValueError("unsafe artwork title")
    folder = target
    for name in ("menu", "metadata", "homebrew", title):
        folder /= name
        if folder.is_symlink():
            raise ValueError(f"artwork directory is a symlink: {folder}")
        folder.mkdir(exist_ok=True)
        if folder.stat().st_dev != target.stat().st_dev:
            raise ValueError("artwork directory is outside the selected SD filesystem")
    return copy_file(data, folder, "boxart_front.png")


def sdcard(action: str, partition: str):
    helper = "mount-media" if action == "mount" else "umount-media"
    if shutil.which(helper):
        command = [helper, "--device", partition]
    else:
        command = ["udisksctl", action, "--block-device", partition]
    with subprocess.Popen(command) as process:
        try:
            result = process.wait()
        finally:
            # Let the command stop its authentication agent before unmounting.
            if process.poll() is None:
                process.terminate()
                process.wait()
        if result:
            raise subprocess.CalledProcessError(result, command)


def transfer(
    partition: str,
    data: bytes,
    filename: str,
    *,
    artwork: bytes | None = None,
    title: str = "",
    wait_before_unmount: bool = False,
) -> str:
    rom = RomCopy(Path(filename), data, filename, {"name": title}, artwork)
    return transfer_many(partition, [rom], wait_before_unmount=wait_before_unmount)[0]


def transfer_many(
    partition: str, roms: list[RomCopy], *, wait_before_unmount: bool = False
) -> list[str]:
    if wait_before_unmount and not sys.stdin.isatty():
        raise ValueError("--wait-before-unmount requires an interactive terminal")
    mounted = mountpoint(partition) is not None
    try:
        if not mounted:
            sdcard("mount", partition)
            mounted = True
        target = mountpoint(partition)
        if target is None or target.stat().st_dev != Path(partition).stat().st_rdev:
            raise ValueError("mountpoint does not refer to the selected SD partition")
        checksums = []
        for rom in roms:
            print(f"Copying {len(rom.data):,} bytes to {target / rom.filename}", flush=True)
            checksums.append(copy_rom(rom.data, target, rom.filename))
            if rom.artwork is not None:
                copy_artwork(rom.artwork, target, rom.metadata["name"])
        if wait_before_unmount:
            label = "Copy" if len(roms) == 1 else "Copies"
            print(f"{label} verified. SD card remains mounted at {target}.", flush=True)
            try:
                input("Finish your manual changes, then press Enter to unmount: ")
            except EOFError as error:
                raise ValueError("input closed while waiting to unmount") from error
        return checksums
    finally:
        # Also run after a failed copy, Ctrl-C or SIGTERM. Never force an unmount.
        if mounted or mountpoint(partition) is not None:
            sdcard("unmount", partition)


def interrupted(signum, _frame):
    raise SystemExit(128 + signum)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--device",
        help="exact disk or partition path (default: the only removable FAT/exFAT partition)",
    )
    parser.add_argument(
        "--rom", type=Path, help="ROM to copy (default: selected game/source build in dist)"
    )
    parser.add_argument(
        "--all", action="store_true", help="copy every executable game's default-source ROM"
    )
    parser.add_argument("--artwork", type=Path, help="optional 158x112 or 112x158 front-cover PNG")
    parser.add_argument(
        "--dry-run", action="store_true", help="check ROM and device without mounting"
    )
    parser.add_argument(
        "--wait-before-unmount",
        action="store_true",
        help="after copying, keep the SD card mounted until Enter is pressed (requires a terminal)",
    )
    args = parser.parse_args(argv)
    if args.all and (args.rom is not None or args.artwork is not None):
        parser.error("--rom and --artwork require a single --game")
    signal.signal(signal.SIGTERM, interrupted)
    try:
        games = all_games() if args.all else [selected_game()]
        roms = [
            prepare_rom(game, args.rom or game.dist / (game.slug + ".z64"), args.artwork)
            for game in games
        ]
        requested = None
        if args.device is not None:
            device = Path(args.device).resolve(strict=True)
            if not device.is_block_device():
                raise ValueError(f"not a block device: {device}")
            requested = str(device)
        inventory = json.loads(
            subprocess.check_output(
                ["lsblk", "--json", "--tree", "--paths", "--output", "PATH,TYPE,RM,FSTYPE"],
                text=True,
            )
        )
        partition = partition_for(inventory["blockdevices"], requested)
        if not Path(partition).is_block_device():
            raise ValueError(f"not a block device: {partition}")
        for rom in roms:
            checksum = hashlib.sha256(rom.data).hexdigest()
            print(f"ROM: {rom.source}\nSHA-256: {checksum}", flush=True)
            print(f"Description: {rom.metadata['short-desc']}", flush=True)
            if rom.artwork is not None:
                print(
                    f"Artwork: menu/metadata/homebrew/{rom.metadata['name']}/boxart_front.png",
                    flush=True,
                )
        print(f"SD partition: {partition}", flush=True)
        if args.dry_run:
            return
        if args.all:
            transfer_many(partition, roms, wait_before_unmount=args.wait_before_unmount)
        else:
            rom = roms[0]
            transfer(
                partition,
                rom.data,
                rom.filename,
                artwork=rom.artwork,
                title=rom.metadata["name"],
                wait_before_unmount=args.wait_before_unmount,
            )
        label = "ROM" if len(roms) == 1 else "ROMs"
        print(f"{label} verified and SD card unmounted. It can now be removed.")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"SummerCart copy failed: {error}") from error


if __name__ == "__main__":
    main()

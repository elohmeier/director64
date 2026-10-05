import json
import struct
import subprocess
import sys
import zlib
from dataclasses import replace
from pathlib import Path
from unittest.mock import MagicMock

import pytest

from director64 import cli, summercart
from director64.project import GameSpec


def disk(*partitions, removable=True):
    return {
        "path": "/dev/sda",
        "type": "disk",
        "rm": removable,
        "children": [{"path": name, "type": "part", "fstype": "vfat"} for name in partitions],
    }


def test_card_or_exact_partition():
    inventory = [disk("/dev/sda1")]
    assert summercart.partition_for(inventory, "/dev/sda") == "/dev/sda1"
    assert summercart.partition_for(inventory, "/dev/sda1") == "/dev/sda1"
    inventory = [disk("/dev/sda1", "/dev/sda2")]
    assert summercart.partition_for(inventory, "/dev/sda2") == "/dev/sda2"
    with pytest.raises(ValueError, match="exactly one"):
        summercart.partition_for(inventory, "/dev/sda")


def test_internal_and_absent_disks_are_rejected():
    with pytest.raises(ValueError, match="removable"):
        summercart.partition_for([disk("/dev/sda1", removable=False)], "/dev/sda1")
    with pytest.raises(ValueError, match="absent"):
        summercart.partition_for([disk("/dev/sda1")], "/dev/sdb")


@pytest.mark.parametrize("filesystem", ["vfat", "exfat"])
@pytest.mark.parametrize("partitioned", [True, False])
def test_auto_selects_only_removable_fat_filesystem(filesystem, partitioned):
    card = disk("/dev/sda1") if partitioned else disk()
    target = card["children"][0] if partitioned else card
    target["fstype"] = filesystem
    internal = disk("/dev/nvme0n1p1", removable=False)
    internal["path"] = "/dev/nvme0n1"
    unsupported = disk("/dev/sdb1")
    unsupported["path"] = "/dev/sdb"
    unsupported["children"][0]["fstype"] = "ext4"
    assert summercart.partition_for([internal, unsupported, card]) == target["path"]


@pytest.mark.parametrize("inventory", [[], [disk("/dev/sda1", removable=False)], [disk()]])
def test_auto_rejects_absent_or_unsuitable_cards(inventory):
    with pytest.raises(ValueError, match="no removable FAT/exFAT partition"):
        summercart.partition_for(inventory)


@pytest.mark.parametrize("separate_disks", [True, False])
def test_auto_requires_explicit_selection_for_multiple_destinations(separate_disks):
    if separate_disks:
        other = disk("/dev/sdb1")
        other["path"] = "/dev/sdb"
        inventory = [disk("/dev/sda1"), other]
        alternative = "/dev/sdb1"
    else:
        inventory = [disk("/dev/sda1", "/dev/sda2")]
        alternative = "/dev/sda2"
    with pytest.raises(ValueError, match="multiple.*--device") as error:
        summercart.partition_for(inventory)
    assert "/dev/sda1" in str(error.value)
    assert alternative in str(error.value)
    assert summercart.partition_for(inventory, alternative) == alternative


@pytest.fixture
def copy_command(tmp_path, monkeypatch, rom_factory):
    game = replace(GameSpec.load("findus-workshop"), root=tmp_path)
    game.dist.mkdir(parents=True)
    source = game.dist / "findus-workshop.z64"
    source.write_bytes(rom_factory(controller_count=game.data["port"]["controller_count"]))
    monkeypatch.setattr(summercart, "selected_game", lambda: game)
    monkeypatch.setattr(summercart.signal, "signal", lambda *_: None)
    monkeypatch.setattr(Path, "is_block_device", lambda path: str(path) == "/dev/sda1")
    inventory = [disk("/dev/sda1")]

    def output(command, **_):
        assert command[0] == "lsblk"
        return json.dumps({"blockdevices": inventory})

    monkeypatch.setattr(summercart.subprocess, "check_output", output)
    transfer = MagicMock()
    monkeypatch.setattr(summercart, "transfer", transfer)
    monkeypatch.setattr(sys, "argv", ["director64 summercart"])
    return source, inventory, transfer


@pytest.mark.parametrize("dry_run", [False, True])
@pytest.mark.parametrize("override_rom", [False, True])
@pytest.mark.parametrize("wait_before_unmount", [False, True])
def test_command_defaults_to_local_build_and_auto_card(
    copy_command, dry_run, override_rom, wait_before_unmount, capsys
):
    source, _, transfer = copy_command
    if override_rom:
        alternate = source.with_name("alternate.z64")
        source.rename(alternate)
        source = alternate
        sys.argv.extend(["--rom", str(source)])
    if dry_run:
        sys.argv.append("--dry-run")
    if wait_before_unmount:
        sys.argv.append("--wait-before-unmount")
    summercart.main()
    assert f"ROM: {source}" in capsys.readouterr().out
    if dry_run:
        transfer.assert_not_called()
    else:
        transfer.assert_called_once_with(
            "/dev/sda1",
            source.read_bytes(),
            "findus-workshop.z64",
            artwork=None,
            title="Findus Workshop",
            wait_before_unmount=wait_before_unmount,
        )


@pytest.fixture
def all_copy_command(copy_command, tmp_path, monkeypatch, rom_factory):
    workshop_source, _, single_transfer = copy_command
    workshop = replace(GameSpec.load("findus-workshop"), root=tmp_path)
    mucklas = replace(GameSpec.load("findus-mucklas"), root=tmp_path)
    mucklas.dist.mkdir(parents=True)
    mucklas_source = mucklas.dist / "findus-mucklas.z64"
    mucklas_source.write_bytes(
        rom_factory(
            title="Findus Mucklas", controller_count=mucklas.data["port"]["controller_count"]
        )
    )
    monkeypatch.setattr(summercart, "all_games", lambda: [workshop, mucklas])
    batch_transfer = MagicMock()
    monkeypatch.setattr(summercart, "transfer_many", batch_transfer)
    sys.argv.append("--all")
    return workshop_source, mucklas_source, single_transfer, batch_transfer


def test_all_selects_every_executable_port():
    games = summercart.all_games()
    assert [game.slug for game in games] == sorted(game.slug for game in games)
    assert all(game.data["status"] in {"supported", "experimental"} for game in games)
    assert len(games) == 6


@pytest.mark.parametrize("dry_run", [False, True])
def test_all_uses_default_source_builds_and_one_batch(all_copy_command, dry_run, capsys):
    workshop, mucklas, single_transfer, batch_transfer = all_copy_command
    if dry_run:
        sys.argv.append("--dry-run")
    summercart.main()
    output = capsys.readouterr().out
    assert f"ROM: {workshop}" in output
    assert f"ROM: {mucklas}" in output
    assert output.count("SD partition: /dev/sda1") == 1
    single_transfer.assert_not_called()
    if dry_run:
        batch_transfer.assert_not_called()
    else:
        batch_transfer.assert_called_once()
        partition, roms = batch_transfer.call_args.args
        assert partition == "/dev/sda1"
        assert [rom.source for rom in roms] == [workshop, mucklas]
        assert [rom.filename for rom in roms] == ["findus-workshop.z64", "findus-mucklas.z64"]
        assert batch_transfer.call_args.kwargs == {"wait_before_unmount": False}


@pytest.mark.parametrize("failure", ["missing", "invalid"])
def test_all_preflights_every_rom_before_card_access(all_copy_command, monkeypatch, failure):
    _, mucklas, single_transfer, batch_transfer = all_copy_command
    if failure == "missing":
        mucklas.unlink()
        message = "No such file"
    else:
        mucklas.write_bytes(b"invalid ROM")
        message = "invalid z64 header"
    monkeypatch.setattr(
        summercart.subprocess,
        "check_output",
        lambda *_args, **_kwargs: pytest.fail("card accessed before all ROMs passed validation"),
    )
    with pytest.raises(SystemExit, match=message):
        summercart.main()
    single_transfer.assert_not_called()
    batch_transfer.assert_not_called()


@pytest.mark.parametrize("option", ["--rom", "--artwork"])
def test_all_rejects_single_game_overrides(all_copy_command, option):
    _, _, single_transfer, batch_transfer = all_copy_command
    sys.argv.extend([option, "other"])
    with pytest.raises(SystemExit, match="2"):
        summercart.main()
    single_transfer.assert_not_called()
    batch_transfer.assert_not_called()


def test_cli_all_invocation_and_source_conflict(monkeypatch):
    command = MagicMock()
    monkeypatch.setattr(summercart, "main", command)
    assert cli.main(["summercart", "--all", "--dry-run"]) == 0
    command.assert_called_once_with(["--all", "--dry-run"])
    with pytest.raises(SystemExit, match="2"):
        cli.main(["summercart", "--all", "--source", "different"])
    with pytest.raises(SystemExit, match="2"):
        cli.main(["summercart", "--all", "--game", "findus-workshop"])


@pytest.mark.parametrize("failure", ["missing_rom", "invalid_rom", "no_card", "multiple_cards"])
def test_command_selection_or_validation_error_never_transfers(copy_command, failure):
    source, inventory, transfer = copy_command
    if failure == "missing_rom":
        source.unlink()
        message = "No such file"
    elif failure == "invalid_rom":
        source.write_bytes(b"invalid ROM")
        message = "invalid z64 header"
    elif failure == "no_card":
        inventory.clear()
        message = "no removable FAT/exFAT partition"
    else:
        inventory[0]["children"].append({"path": "/dev/sda2", "fstype": "vfat"})
        message = "multiple.*--device"
    with pytest.raises(SystemExit, match=message):
        summercart.main()
    transfer.assert_not_called()


@pytest.mark.parametrize("slug", ["findus-workshop", "findus-mucklas"])
def test_selected_game_release_budget(copy_command, monkeypatch, rom_factory, slug):
    source, _, transfer = copy_command
    game = GameSpec.load(slug)
    source.write_bytes(
        rom_factory(
            title=game.data["port"]["rom_title"],
            controller_count=game.data["port"].get("controller_count", 1),
            code_bytes=60 * 1024 * 1024 - 16384,
        )
    )
    monkeypatch.setattr(summercart, "selected_game", lambda: game)
    sys.argv.extend(["--rom", str(source), "--dry-run"])
    # Every game shares the ceiling: nothing may be written past 56 MiB,
    # because the SDK's USB debug scratch starts overwriting the cart there.
    with pytest.raises(SystemExit, match="release 56 MiB"):
        summercart.main()
    transfer.assert_not_called()


@pytest.mark.parametrize("controller_count", [1, 4])
def test_command_uses_selected_game_controller_count(
    copy_command, tmp_path, monkeypatch, rom_factory, controller_count
):
    _, _, transfer = copy_command
    game = replace(GameSpec.load("findus-mucklas"), root=tmp_path)
    game.dist.mkdir(parents=True)
    source = game.dist / "findus-mucklas.z64"
    source.write_bytes(rom_factory(title="Findus Mucklas", controller_count=controller_count))
    monkeypatch.setattr(summercart, "selected_game", lambda: game)
    if controller_count == 1:
        monkeypatch.setattr(
            summercart.subprocess,
            "check_output",
            lambda *_args, **_kwargs: pytest.fail("card access"),
        )
        with pytest.raises(SystemExit, match="controller metadata must match"):
            summercart.main()
        transfer.assert_not_called()
    else:
        summercart.main()
        transfer.assert_called_once_with(
            "/dev/sda1",
            source.read_bytes(),
            "findus-mucklas.z64",
            artwork=None,
            title="Findus Mucklas",
            wait_before_unmount=False,
        )


@pytest.mark.parametrize(
    "size,message",
    [(56 * 1024 * 1024 + 1, "release 56 MiB"), (64 * 1024 * 1024 + 1, "hard 64 MiB")],
)
def test_oversized_mucklas_rom_is_rejected_before_read_or_card_access(
    copy_command, monkeypatch, size, message
):
    source, _, transfer = copy_command
    monkeypatch.setattr(summercart, "selected_game", lambda: GameSpec.load("findus-mucklas"))
    with source.open("r+b") as stream:
        stream.truncate(size)
    monkeypatch.setattr(Path, "read_bytes", lambda _: pytest.fail("oversized ROM read"))
    monkeypatch.setattr(
        summercart.subprocess, "check_output", lambda *_args, **_kwargs: pytest.fail("card access")
    )
    sys.argv.extend(["--rom", str(source)])
    with pytest.raises(SystemExit, match=message):
        summercart.main()
    transfer.assert_not_called()


@pytest.fixture
def artwork_png():
    def chunk(kind, content):
        return (
            struct.pack(">I", len(content))
            + kind
            + content
            + struct.pack(">I", zlib.crc32(kind + content))
        )

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 158, 112, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes((1 + 158 * 3) * 112)))
        + chunk(b"IEND", b"")
    )


@pytest.mark.parametrize("dry_run", [False, True])
def test_artwork_is_preflighted_and_uses_header_title(copy_command, artwork_png, dry_run, capsys):
    source, _, transfer = copy_command
    art = source.with_name("cover.png")
    art.write_bytes(artwork_png)
    sys.argv.extend(["--artwork", str(art)])
    if dry_run:
        sys.argv.append("--dry-run")
    summercart.main()
    assert "menu/metadata/homebrew/Findus Workshop/boxart_front.png" in capsys.readouterr().out
    if dry_run:
        transfer.assert_not_called()
    else:
        transfer.assert_called_once_with(
            "/dev/sda1",
            source.read_bytes(),
            "findus-workshop.z64",
            artwork=artwork_png,
            title="Findus Workshop",
            wait_before_unmount=False,
        )


def test_invalid_artwork_prevents_transfer(copy_command):
    source, _, transfer = copy_command
    art = source.with_name("cover.png")
    art.write_bytes(b"not an image")
    sys.argv.extend(["--artwork", str(art)])
    with pytest.raises(SystemExit, match="PNG"):
        summercart.main()
    transfer.assert_not_called()


def test_artwork_preserves_saves_and_other_images(tmp_path, artwork_png):
    summercart.validate_artwork(artwork_png)
    title = "Findus Workshop"
    folder = tmp_path / "menu/metadata/homebrew" / title
    folder.mkdir(parents=True)
    (folder / "boxart_back.png").write_bytes(b"existing image")
    (tmp_path / "findus-workshop.sav").write_bytes(b"save")
    summercart.copy_artwork(artwork_png, tmp_path, title)
    assert (folder / "boxart_front.png").read_bytes() == artwork_png
    assert (folder / "boxart_back.png").read_bytes() == b"existing image"
    assert (tmp_path / "findus-workshop.sav").read_bytes() == b"save"


def test_artwork_rejects_symlink_directories_and_destinations(tmp_path, artwork_png):
    target = tmp_path / "card"
    target.mkdir()
    outside = tmp_path / "outside"
    outside.mkdir()
    (target / "menu").symlink_to(outside, target_is_directory=True)
    with pytest.raises(ValueError, match="symlink"):
        summercart.copy_artwork(artwork_png, target, "Findus Workshop")
    assert not list(outside.iterdir())
    (target / "menu").unlink()
    folder = target / "menu/metadata/homebrew/Findus Workshop"
    folder.mkdir(parents=True)
    (folder / "boxart_front.png").symlink_to(outside / "image.png")
    with pytest.raises(ValueError, match="regular file"):
        summercart.copy_artwork(artwork_png, target, "Findus Workshop")
    assert not list(outside.iterdir())


def test_artwork_rejects_corruption_and_truncation(artwork_png):
    corrupt = bytearray(artwork_png)
    corrupt[30] ^= 1
    for data in (bytes(corrupt), artwork_png[:-12]):
        with pytest.raises(ValueError, match="PNG"):
            summercart.validate_artwork(data)


def test_artwork_failure_still_unmounts(monkeypatch, artwork_png):
    target = MagicMock(spec=Path)
    target.stat.return_value.st_dev = Path("/dev/null").stat().st_rdev
    monkeypatch.setattr(summercart, "mountpoint", lambda _: target)
    monkeypatch.setattr(summercart, "copy_rom", lambda *_: "checksum")
    monkeypatch.setattr(summercart, "copy_artwork", MagicMock(side_effect=OSError("card full")))
    unmount = MagicMock()
    monkeypatch.setattr(summercart, "sdcard", unmount)
    monkeypatch.setattr(sys.stdin, "isatty", lambda: True)
    prompt = MagicMock(side_effect=AssertionError("must not wait after a failed copy"))
    monkeypatch.setattr("builtins.input", prompt)
    with pytest.raises(OSError, match="card full"):
        summercart.transfer(
            "/dev/null",
            b"ROM",
            "findus-workshop.z64",
            artwork=artwork_png,
            title="Findus Workshop",
            wait_before_unmount=True,
        )
    unmount.assert_called_once_with("unmount", "/dev/null")
    prompt.assert_not_called()


@pytest.mark.parametrize("already_mounted", [False, True])
@pytest.mark.parametrize("completion", ["enter", "interrupt", "eof", "sigterm"])
def test_wait_keeps_card_mounted_until_input_and_always_cleans_up(
    monkeypatch, already_mounted, completion, capsys
):
    target = MagicMock(spec=Path)
    target.__str__.return_value = "/media/card"
    target.stat.return_value.st_dev = Path("/dev/null").stat().st_rdev
    points = iter([target if already_mounted else None, target])
    monkeypatch.setattr(summercart, "mountpoint", lambda _: next(points))
    monkeypatch.setattr(sys.stdin, "isatty", lambda: True)
    events = []
    monkeypatch.setattr(summercart, "sdcard", lambda action, _: events.append(action))

    def copy(*_):
        events.append("copy")
        return "checksum"

    monkeypatch.setattr(summercart, "copy_rom", copy)
    monkeypatch.setattr(summercart, "copy_artwork", lambda *_: events.append("artwork"))

    def enter(prompt):
        assert "press Enter to unmount" in prompt
        assert "/media/card" in capsys.readouterr().out
        assert events == ([] if already_mounted else ["mount"]) + ["copy", "artwork"]
        events.append("wait")
        if completion == "interrupt":
            raise KeyboardInterrupt
        if completion == "eof":
            raise EOFError
        if completion == "sigterm":
            summercart.interrupted(summercart.signal.SIGTERM, None)
        return ""

    monkeypatch.setattr("builtins.input", enter)

    def transfer():
        return summercart.transfer(
            "/dev/null",
            b"ROM",
            "findus-mucklas.z64",
            artwork=b"artwork",
            title="Findus Mucklas",
            wait_before_unmount=True,
        )

    if completion == "enter":
        assert transfer() == "checksum"
    else:
        error = {"interrupt": KeyboardInterrupt, "eof": ValueError, "sigterm": SystemExit}
        with pytest.raises(error[completion]):
            transfer()
    assert events == ([] if already_mounted else ["mount"]) + ["copy", "artwork", "wait", "unmount"]


def test_wait_rejects_noninteractive_input_before_touching_card(monkeypatch):
    monkeypatch.setattr(sys.stdin, "isatty", lambda: False)
    mountpoint = MagicMock(side_effect=AssertionError("must not touch card"))
    monkeypatch.setattr(summercart, "mountpoint", mountpoint)
    with pytest.raises(ValueError, match="requires an interactive terminal"):
        summercart.transfer("/dev/null", b"ROM", "findus-mucklas.z64", wait_before_unmount=True)
    mountpoint.assert_not_called()


def test_transfer_many_mounts_and_unmounts_once(monkeypatch):
    target = MagicMock(spec=Path)
    target.stat.return_value.st_dev = Path("/dev/null").stat().st_rdev
    monkeypatch.setattr(summercart, "mountpoint", MagicMock(side_effect=[None, target]))
    events = []
    monkeypatch.setattr(summercart, "sdcard", lambda action, _: events.append(action))

    def copy(_, __, filename):
        events.append(filename)
        return filename

    monkeypatch.setattr(summercart, "copy_rom", copy)
    roms = [
        summercart.RomCopy(Path(filename), b"ROM", filename, {"name": filename})
        for filename in ("first.z64", "second.z64")
    ]
    assert summercart.transfer_many("/dev/null", roms) == ["first.z64", "second.z64"]
    assert events == ["mount", "first.z64", "second.z64", "unmount"]


def test_copy_replaces_only_rom(tmp_path):
    destination = tmp_path / "findus-workshop.z64"
    destination.write_bytes(b"old ROM")
    save = tmp_path / "findus-workshop.fla"
    save.write_bytes(b"existing progress")
    summercart.copy_rom(b"new ROM", tmp_path, "findus-workshop.z64")
    assert destination.read_bytes() == b"new ROM"
    assert save.read_bytes() == b"existing progress"
    assert sorted(p.name for p in tmp_path.iterdir()) == [
        "findus-workshop.fla",
        "findus-workshop.z64",
    ]


def test_failed_copy_keeps_previous_rom_and_removes_temporary_file(tmp_path, monkeypatch):
    destination = tmp_path / "findus-workshop.z64"
    destination.write_bytes(b"old ROM")

    def fail(_):
        raise OSError("card write failed")

    monkeypatch.setattr(summercart.os, "fsync", fail)
    with pytest.raises(OSError, match="card write failed"):
        summercart.copy_rom(b"new ROM", tmp_path, "findus-workshop.z64")
    assert destination.read_bytes() == b"old ROM"
    assert list(tmp_path.iterdir()) == [destination]


@pytest.mark.parametrize("already_mounted", [True, False])
def test_copy_error_still_unmounts(already_mounted, monkeypatch):
    # The transfer stage receives a preselected device; no real mount calls here.
    target = MagicMock(spec=Path)
    target.stat.return_value.st_dev = Path("/dev/null").stat().st_rdev
    points = iter([target if already_mounted else None, target])
    monkeypatch.setattr(summercart, "mountpoint", lambda _: next(points))
    commands = []
    monkeypatch.setattr(summercart, "sdcard", lambda action, _: commands.append(action))

    def fail(*_):
        raise OSError("card full")

    monkeypatch.setattr(summercart, "copy_rom", fail)
    with pytest.raises(OSError, match="card full"):
        summercart.transfer("/dev/null", b"ROM", "findus-workshop.z64")
    assert commands == (["unmount"] if already_mounted else ["mount", "unmount"])


def test_failed_unmount_is_reported(monkeypatch):
    target = MagicMock(spec=Path)
    target.stat.return_value.st_dev = Path("/dev/null").stat().st_rdev
    monkeypatch.setattr(summercart, "mountpoint", lambda _: target)
    monkeypatch.setattr(summercart, "copy_rom", lambda *_: "checksum")

    def fail(*_, **__):
        raise OSError("device busy")

    monkeypatch.setattr(summercart, "sdcard", fail)
    with pytest.raises(OSError, match="device busy"):
        summercart.transfer("/dev/null", b"ROM", "findus-workshop.z64")


@pytest.mark.parametrize("action,command", [("mount", "mount-media"), ("unmount", "umount-media")])
def test_sdcard_commands_are_resolved_from_path(action, command, monkeypatch):
    start = MagicMock()
    process = start.return_value.__enter__.return_value
    process.wait.return_value = process.poll.return_value = 0
    monkeypatch.setattr(summercart.subprocess, "Popen", start)
    monkeypatch.setattr(summercart.shutil, "which", lambda name: f"/usr/bin/{name}")
    summercart.sdcard(action, "/dev/sda1")
    start.assert_called_once_with([command, "--device", "/dev/sda1"])


@pytest.mark.parametrize("action", ["mount", "unmount"])
def test_sdcard_falls_back_to_udisksctl(action, monkeypatch):
    start = MagicMock()
    process = start.return_value.__enter__.return_value
    process.wait.return_value = process.poll.return_value = 0
    monkeypatch.setattr(summercart.subprocess, "Popen", start)
    monkeypatch.setattr(summercart.shutil, "which", lambda name: None)
    summercart.sdcard(action, "/dev/sda1")
    start.assert_called_once_with(["udisksctl", action, "--block-device", "/dev/sda1"])


@pytest.mark.parametrize("failure", [KeyboardInterrupt, SystemExit, subprocess.CalledProcessError])
def test_sdcard_failure_propagates_and_interrupts_allow_cleanup(failure, monkeypatch):
    start = MagicMock()
    process = start.return_value.__enter__.return_value
    if failure is subprocess.CalledProcessError:
        process.wait.return_value = process.poll.return_value = 7
    else:
        process.wait.side_effect = [failure(), 143]
        process.poll.return_value = None
    monkeypatch.setattr(summercart.subprocess, "Popen", start)
    with pytest.raises(failure):
        summercart.sdcard("mount", "/dev/sda1")
    if failure is not subprocess.CalledProcessError:
        process.terminate.assert_called_once()
        assert process.wait.call_count == 2
    process.kill.assert_not_called()


@pytest.mark.parametrize("filename", ["../escape.z64", "progress.fla", "/escape.z64"])
def test_game_filename_cannot_overwrite_saves_or_escape_card(tmp_path, filename):
    with pytest.raises(ValueError, match="basename"):
        summercart.copy_rom(b"rom", tmp_path, filename)
    assert not list(tmp_path.iterdir())

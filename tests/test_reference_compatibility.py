"""Private-reference compatibility patches, tested on synthetic PE32 bytes."""

from __future__ import annotations

import hashlib
import shutil
import struct

import pytest

from director64.project import repository
from director64.reference import runner as load_runner

runner = load_runner(repository())


@pytest.mark.parametrize("full_script", [False, True])
def test_authored_experiments_preserve_explicit_handler_scope_and_completion(full_script):
    statements = 'out.writeString("measured")\nout.closeFile()\n'
    source = ("-- director64: full-script\non startup\n" + statements + "end\n"
              if full_script else statements)
    rendered = runner.experiment_lingo(source)
    assert rendered.count("on startup") == 1
    assert rendered.count("DIRECTOR64_PROBE_COMPLETE") == 1
    assert rendered.index("DIRECTOR64_PROBE_COMPLETE") < rendered.index("out.closeFile()")
    assert rendered.startswith("-- director64: full-script") == full_script


def test_legacy_fileio_experiment_records_explicit_interface_and_completion():
    body = ('-- director64: fileio-xobject\n'
            'out = FileIO(mNew, "write", "C:\\legacy-probe.txt")\n'
            'out(mWriteString, "measured")\nout(mDispose)\n')
    assert runner.experiment_output(body) == ("legacy-probe.txt", "xobject")
    rendered = runner.experiment_lingo(body)
    assert 'out(mWriteString, "DIRECTOR64_PROBE_COMPLETE" & RETURN)' in rendered
    assert rendered.index("DIRECTOR64_PROBE_COMPLETE") < rendered.index("out(mDispose)")
    assert "out.closeFile" not in rendered
    with pytest.raises(ValueError, match="write and close"):
        runner.experiment_output(body.replace('-- director64: fileio-xobject', '-- absent'))
    with pytest.raises(ValueError, match="write and close"):
        runner.experiment_output(body.replace('C:\\legacy-probe.txt', 'C:\\..\\escape.txt'))


def test_xtra_fileio_experiment_keeps_existing_result_contract():
    source = 'out.createFile("C:\\probe.txt")\nout.closeFile()\n'
    assert runner.experiment_output(source) == ("probe.txt", "xtra")


def test_director_six_fileio_function_notation_is_explicit_and_closed():
    body = ('-- director64: fileio-functions\n'
            'createFile(out, "C:\\function-probe.txt")\ncloseFile(out)\n')
    assert runner.experiment_output(body) == ("function-probe.txt", "xtra-functions")
    rendered = runner.experiment_lingo(body)
    assert 'writeString(out, "DIRECTOR64_PROBE_COMPLETE" & RETURN)' in rendered
    assert rendered.index("DIRECTOR64_PROBE_COMPLETE") < rendered.index("closeFile(out)")


def test_embedded_d5_d6_engines_are_retained_without_playback_dlls():
    assert runner.playback_process(
        "00400000-00500000 r-xp /session/wine/drive_c/Original/STARTW95.EXE")
    assert runner.playback_process(
        "00400000-00500000 r-xp /session/wine/drive_c/Original/MEDIA/WIN95.EXE")
    assert runner.playback_process("10000000-10500000 r-xp /temp/DIRAPI.DLL")
    assert not runner.playback_process("70000000-70500000 r-xp /usr/lib/wine/wineboot.exe")


@pytest.mark.parametrize("value", [None, True, 0, 15, 1025, "256", 256.0])
def test_legacy_memory_report_rejects_unbounded_or_ambiguous_values(value):
    with pytest.raises(ValueError, match="16..1024"):
        runner.legacy_meminfo(value)


def test_legacy_memory_report_is_explicit_and_does_not_alter_game_files(tmp_path):
    before = bytes(synthetic_pe32())
    path = tmp_path / "synthetic.exe"
    path.write_bytes(before)
    report = runner.legacy_meminfo(256)
    assert "MemTotal: 262144 kB\n" in report
    assert "MemFree: 131072 kB\n" in report
    assert "SwapTotal: 262144 kB\n" in report
    assert path.read_bytes() == before


PE = 0x80
COFF = PE + 4
OPTIONAL = COFF + 20
SECTIONS = OPTIONAL + 224
TEXT_RVA = 0x1000
TEXT_OFFSET = 0x200
EXPORT_RVA = 0x2000
EXPORT_OFFSET = 0x400
EXPECTED_RETURN = 13665984


def synthetic_pe32() -> bytearray:
    image = bytearray(0x600)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, PE)
    image[PE:PE + 4] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", image, COFF, 0x14C, 2, 0, 0, 0, 224, 0x2102)
    struct.pack_into("<H", image, OPTIONAL, 0x10B)
    struct.pack_into("<III", image, OPTIONAL + 16, TEXT_RVA, TEXT_RVA, EXPORT_RVA)
    struct.pack_into("<III", image, OPTIONAL + 28, 0x10000000, 0x1000, 0x200)
    struct.pack_into("<II", image, OPTIONAL + 56, 0x3000, 0x200)
    struct.pack_into("<H", image, OPTIONAL + 68, 2)
    struct.pack_into("<I", image, OPTIONAL + 92, 16)
    struct.pack_into("<II", image, OPTIONAL + 96, EXPORT_RVA, 0x100)
    for offset, name, rva, raw, flags in (
        (SECTIONS, b".text", TEXT_RVA, TEXT_OFFSET, 0x60000020),
        (SECTIONS + 40, b".edata", EXPORT_RVA, EXPORT_OFFSET, 0x40000040),
    ):
        image[offset:offset + len(name)] = name
        struct.pack_into("<IIII", image, offset + 8, 0x200, rva, 0x200, raw)
        struct.pack_into("<I", image, offset + 36, flags)
    image[TEXT_OFFSET:TEXT_OFFSET + 0x200] = b"\xcc" * 0x200
    image[TEXT_OFFSET:TEXT_OFFSET + 6] = b"\x55\x8b\xec\x83\xec\x10"
    struct.pack_into("<IIHHIIIIIII", image, EXPORT_OFFSET,
                     0, 0, 0, 0, EXPORT_RVA + 0x50, 1, 1, 1,
                     EXPORT_RVA + 0x28, EXPORT_RVA + 0x2C, EXPORT_RVA + 0x30)
    struct.pack_into("<I", image, EXPORT_OFFSET + 0x28, TEXT_RVA)
    struct.pack_into("<I", image, EXPORT_OFFSET + 0x2C, EXPORT_RVA + 0x60)
    struct.pack_into("<H", image, EXPORT_OFFSET + 0x30, 0)
    image[EXPORT_OFFSET + 0x50:EXPORT_OFFSET + 0x50 + 14] = b"synthetic.dll\0"
    image[EXPORT_OFFSET + 0x60:EXPORT_OFFSET + 0x60 + 12] = b"initdisplay\0"
    return image


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def test_private_reference_patch_changes_only_the_export_entry_and_records_hashes(tmp_path):
    original = bytes(synthetic_pe32())
    path = tmp_path / "synthetic.dll"
    path.write_bytes(original)
    record = runner.patch_pe32_return(path, sha256(original), "initdisplay", EXPECTED_RETURN)
    expected = (original[:TEXT_OFFSET] + b"\xb8" + struct.pack("<I", EXPECTED_RETURN)
                + b"\xc3" + original[TEXT_OFFSET + 6:])
    assert path.read_bytes() == expected and len(expected) == len(original)
    assert [i for i, (before, after) in enumerate(zip(original, expected, strict=True))
            if before != after] == list(range(TEXT_OFFSET, TEXT_OFFSET + 6))
    assert record["before_sha256"] == sha256(original)
    assert record["after_sha256"] == sha256(expected)
    assert record["before_sha256"] != record["after_sha256"]
    assert record["export"] == "initdisplay"
    assert record["rva"] == TEXT_RVA
    assert record["file_offset"] == TEXT_OFFSET
    assert record["return_u32"] == EXPECTED_RETURN
    # Reapplying to an already changed file cannot claim an original match.
    with pytest.raises(ValueError):
        runner.patch_pe32_return(path, sha256(original), "initdisplay", EXPECTED_RETURN)
    assert path.read_bytes() == expected


def test_wrong_source_hash_never_writes_the_reference_copy(tmp_path):
    path = tmp_path / "synthetic.dll"
    original = bytes(synthetic_pe32())
    path.write_bytes(original)
    with pytest.raises(ValueError):
        runner.patch_pe32_return(path, "0" * 64, "initdisplay", EXPECTED_RETURN)
    assert path.read_bytes() == original


@pytest.mark.parametrize("problem", [
    "wrong-machine", "pe32-plus", "not-dll", "forwarded-export", "unmapped-export",
    "nonexecutable-export", "cross-section-entry", "unbacked-entry", "missing-export",
])
def test_invalid_pe_exports_are_rejected_without_writing(tmp_path, problem):
    image = synthetic_pe32()
    if problem == "wrong-machine":
        struct.pack_into("<H", image, COFF, 0x8664)
    elif problem == "pe32-plus":
        struct.pack_into("<H", image, OPTIONAL, 0x20B)
    elif problem == "not-dll":
        struct.pack_into("<H", image, COFF + 18, 0x102)
    elif problem == "forwarded-export":
        struct.pack_into("<I", image, EXPORT_OFFSET + 0x28, EXPORT_RVA + 0x80)
        image[EXPORT_OFFSET + 0x80:EXPORT_OFFSET + 0x80 + 14] = b"other.forward\0"
        # A forwarded RVA remains invalid even in an executable export section.
        struct.pack_into("<I", image, SECTIONS + 40 + 36, 0x60000040)
    elif problem == "unmapped-export":
        struct.pack_into("<I", image, EXPORT_OFFSET + 0x28, 0x4000)
    elif problem == "nonexecutable-export":
        struct.pack_into("<I", image, SECTIONS + 36, 0x40000020)
    elif problem == "cross-section-entry":
        struct.pack_into("<I", image, EXPORT_OFFSET + 0x28, TEXT_RVA + 0x1FD)
    elif problem == "unbacked-entry":
        struct.pack_into("<I", image, SECTIONS + 20, 0x600)
    elif problem == "missing-export":
        image[EXPORT_OFFSET + 0x60:EXPORT_OFFSET + 0x60 + 12] = b"other_name\0\0"
    original = bytes(image)
    path = tmp_path / "synthetic.dll"
    path.write_bytes(original)
    with pytest.raises(ValueError):
        runner.patch_pe32_return(path, sha256(original), "initdisplay", EXPECTED_RETURN)
    assert path.read_bytes() == original


@pytest.mark.parametrize("length", [0, 2, 63, PE + 3, OPTIONAL + 100, SECTIONS + 79, 0x430])
def test_short_pe_files_are_rejected_without_writing(tmp_path, length):
    original = bytes(synthetic_pe32()[:length])
    path = tmp_path / "synthetic.dll"
    path.write_bytes(original)
    with pytest.raises(ValueError):
        runner.patch_pe32_return(path, sha256(original), "initdisplay", EXPECTED_RETURN)
    assert path.read_bytes() == original


def compatibility_plan(original: bytes, path="XTRAS/synthetic.dll") -> dict:
    return {"id": "synthetic-private-reference", "reason": "Synthetic environment limitation",
            "patches": [{"path": path, "source_sha256": sha256(original),
                         "export": "initdisplay", "return_u32": EXPECTED_RETURN}]}


def test_apply_compatibility_changes_only_the_copied_installation_and_records_patch(tmp_path):
    original = bytes(synthetic_pe32())
    source = tmp_path / "source"
    source.mkdir()
    (source / "XTRAS").mkdir()
    original_dll = source / "XTRAS/synthetic.dll"
    original_dll.write_bytes(original)
    installed = tmp_path / "private-copy"
    shutil.copytree(source, installed)
    plan = compatibility_plan(original)
    record = runner.apply_compatibility(installed, plan)
    private_dll = installed / "XTRAS/synthetic.dll"
    assert original_dll.read_bytes() == original
    assert private_dll.read_bytes() != original
    assert record["id"] == plan["id"] and record["reason"] == plan["reason"]
    assert record["original_game_unmodified"] is False
    assert len(record["patches"]) == 1
    patch = record["patches"][0]
    assert patch["path"] == "XTRAS/synthetic.dll"
    assert patch["before_sha256"] == sha256(original)
    assert patch["after_sha256"] == sha256(private_dll.read_bytes())
    assert patch["export"] == "initdisplay" and patch["return_u32"] == EXPECTED_RETURN
    assert patch["rva"] == TEXT_RVA and patch["file_offset"] == TEXT_OFFSET


@pytest.mark.parametrize("path_kind", ["absolute", "parent", "file-symlink", "directory-symlink"])
def test_apply_compatibility_refuses_paths_and_symlinks_outside_private_copy(tmp_path, path_kind):
    original = bytes(synthetic_pe32())
    outside = tmp_path / "outside"
    outside.mkdir()
    outside_dll = outside / "synthetic.dll"
    outside_dll.write_bytes(original)
    installed = tmp_path / "private-copy"
    installed.mkdir()
    if path_kind == "absolute":
        name = str(outside_dll)
    elif path_kind == "parent":
        name = "../outside/synthetic.dll"
    elif path_kind == "file-symlink":
        (installed / "synthetic.dll").symlink_to(outside_dll)
        name = "synthetic.dll"
    else:
        (installed / "XTRAS").symlink_to(outside, target_is_directory=True)
        name = "XTRAS/synthetic.dll"
    with pytest.raises(ValueError, match="escapes private installation"):
        runner.apply_compatibility(installed, compatibility_plan(original, name))
    assert outside_dll.read_bytes() == original


def test_apply_compatibility_can_follow_an_alias_within_the_private_copy(tmp_path):
    original = bytes(synthetic_pe32())
    installed = tmp_path / "private-copy"
    installed.mkdir()
    private_dll = installed / "synthetic.dll"
    private_dll.write_bytes(original)
    alias = installed / "alias.dll"
    alias.symlink_to("synthetic.dll")
    record = runner.apply_compatibility(installed, compatibility_plan(original, "alias.dll"))
    assert alias.is_symlink()
    assert private_dll.read_bytes() == alias.read_bytes() != original
    assert record["patches"][0]["path"] == "alias.dll"
    assert record["patches"][0]["after_sha256"] == sha256(private_dll.read_bytes())

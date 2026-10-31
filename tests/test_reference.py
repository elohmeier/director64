"""Original capture layouts preserve the selected disc and stage boundaries."""

import hashlib
import json
from dataclasses import replace
from itertools import product

import pytest

from director64.project import GameSpec
from director64.reference import layout_for, optical_metadata, parse_precision, relative, runner


def test_birthday_original_layout_uses_actual_launcher_and_debug_ini(tmp_path):
    (tmp_path / "SPIEL").mkdir()
    (tmp_path / "SPIEL/PF3.EXE").write_bytes(b"synthetic launcher")
    layout = layout_for(GameSpec.load("findus-birthday"), tmp_path)
    assert layout["stage"] == [800, 600]
    assert layout["launcher"] == "SPIEL/PF3.EXE"
    assert layout["cwd"] == "SPIEL"
    assert "CheckCopyProtection=0\r\n" in layout["files"]["pf3.ini"]
    assert layout["files"]["projector.ini"] == layout["files"]["pf3.ini"]


def test_original_inputs_allow_large_stage_and_normal_typing_without_shell_commands(tmp_path):
    module = runner(GameSpec.load("findus-birthday").root)
    path = tmp_path / "input.json"
    data = {"schema_version": 1, "id": "name-entry", "clock": "host_monotonic_ms",
            "duration_ms": 1000, "actions": [
                {"at_ms": 1, "type": "pointer", "x": 799, "y": 599},
                {"at_ms": 2, "type": "key", "key": "A"},
                {"at_ms": 999, "type": "checkpoint", "id": "entry"}]}
    path.write_text(json.dumps(data))
    assert module.read_replay(path, (800, 600)) == data
    with pytest.raises(ValueError, match="640x480"):
        module.read_replay(path)
    data["actions"][1]["key"] = "ctrl+alt+Delete"
    path.write_text(json.dumps(data))
    with pytest.raises(ValueError, match="unsupported key"):
        module.read_replay(path, (800, 600))


@pytest.mark.parametrize("value", ["../game.exe", "/game.exe", "C:game.exe", "a\\b.exe", ""])
def test_layout_does_not_escape_private_install(value):
    with pytest.raises(ValueError, match="disc-relative"):
        relative(value)


def test_original_install_reconstructs_joliet_names_without_case_duplicate_folders(tmp_path):
    module = runner(GameSpec.load("findus-birthday").root)
    (tmp_path / "SPIEL/DATA/LOCATION").mkdir(parents=True)
    (tmp_path / "SPIEL/UTILITIE.DXR").write_bytes(b"synthetic utility")
    (tmp_path / "SPIEL/DATA/LOCATION/R0011ANI.CXT").write_bytes(b"synthetic cast")
    module.install_aliases(tmp_path, {"Spiel/Utilities.dxr": "SPIEL/UTILITIE.DXR",
                                    "Spiel/Data/Locations/R0011_Animation.cxt":
                                        "SPIEL/DATA/LOCATION/R0011ANI.CXT"})
    assert not (tmp_path / "Spiel").exists()
    assert (tmp_path / "SPIEL/Utilities.dxr").read_bytes() == b"synthetic utility"
    assert (tmp_path / "SPIEL/DATA/Locations/R0011_Animation.cxt").read_bytes() == b"synthetic cast"
    (tmp_path / "SPIEL/PF3.INI").write_text("original")
    target = module.installed_file(tmp_path / "SPIEL", "pf3.ini")
    target.write_text("debug settings")
    assert (tmp_path / "SPIEL/PF3.INI").read_text() == "debug settings"
    assert not (tmp_path / "SPIEL/pf3.ini").exists()


def test_original_install_exposes_verified_mixed_directories_and_filenames(tmp_path):
    module = runner(GameSpec.load("findus-birthday").root)
    source = tmp_path / "WIMMELBI/SOUNDS/INTROOUT/S2_INTRO.MP3"
    source.parent.mkdir(parents=True)
    source.write_bytes(b"unchanged original sound")
    aliases = {"Wimmelbild/sounds/IntroOutro/S2_Intro_Long.mp3":
               "WIMMELBI/SOUNDS/INTROOUT/S2_INTRO.MP3"}
    module.install_aliases(tmp_path, aliases)
    module.install_aliases(tmp_path, aliases)
    for parts in product(["Wimmelbild", "WIMMELBI"], ["sounds"],
                         ["IntroOutro", "INTROOUT"], ["S2_Intro_Long.mp3", "S2_INTRO.MP3"]):
        target = tmp_path
        for part in parts:
            matches = [p for p in target.iterdir() if p.name.casefold() == part.casefold()]
            assert len(matches) == 1
            target = matches[0]
        assert target.read_bytes() == b"unchanged original sound"
        assert target.resolve() == source
    files = [p for p in tmp_path.rglob("*") if p.is_file()]
    assert len(files) == 8
    assert sum(not p.is_symlink() for p in files) == 1


def test_original_install_unequal_depth_alias_keeps_complete_verified_path(tmp_path):
    module = runner(GameSpec.load("findus-birthday").root)
    (tmp_path / "SOURCE.DXR").write_bytes(b"unchanged source")
    module.install_aliases(tmp_path, {"Long/Directory/Original.dxr": "SOURCE.DXR"})
    assert (tmp_path / "Long/Directory/Original.dxr").resolve() == tmp_path / "SOURCE.DXR"
    assert not (tmp_path / "Long/Directory/SOURCE.DXR").exists()


@pytest.mark.parametrize("existing", [b"retail", None])
def test_original_install_rejects_conflicting_existing_alias_target(tmp_path, existing):
    module = runner(GameSpec.load("findus-birthday").root)
    (tmp_path / "SOURCE.DXR").write_bytes(b"source")
    target = tmp_path / "Long.dxr"
    if existing is None:
        target.mkdir()
    else:
        target.write_bytes(existing)
    with pytest.raises(ValueError, match="conflicting disc alias target"):
        module.install_aliases(tmp_path, {"LONG.DXR": "SOURCE.DXR"})
    if existing is not None:
        assert target.read_bytes() == existing


def test_original_install_accepts_an_existing_identical_alias_file(tmp_path):
    module = runner(GameSpec.load("findus-birthday").root)
    (tmp_path / "SOURCE.DXR").write_bytes(b"source")
    (tmp_path / "Long.dxr").write_bytes(b"source")
    module.install_aliases(tmp_path, {"LONG.DXR": "SOURCE.DXR"})
    assert not (tmp_path / "Long.dxr").is_symlink()


def test_original_install_rejects_conflicting_parent_or_alias_source(tmp_path):
    module = runner(GameSpec.load("findus-birthday").root)
    (tmp_path / "SOURCE.DXR").write_bytes(b"source")
    (tmp_path / "OTHER.DXR").write_bytes(b"other!")
    (tmp_path / "Long").write_bytes(b"file, not a directory")
    with pytest.raises(ValueError, match="conflicting disc alias parent"):
        module.install_aliases(tmp_path, {"Long/File.dxr": "SOURCE.DXR"})
    with pytest.raises(ValueError, match="conflicting disc alias target"):
        module.install_aliases(tmp_path, {"Alias.dxr": "SOURCE.DXR", "ALIAS.DXR": "OTHER.DXR"})
    with pytest.raises(ValueError, match="source is absent"):
        module.install_aliases(tmp_path, {"Missing.dxr": "ABSENT.DXR"})


def test_original_install_bounds_variants_after_casefold_deduplication(tmp_path):
    module = runner(GameSpec.load("findus-birthday").root)
    parts = ["folder"] * 12
    source = tmp_path.joinpath(*parts, "SOURCE.DXR")
    source.parent.mkdir(parents=True)
    source.write_bytes(b"source")
    module.install_aliases(tmp_path, {"/".join([p.upper() for p in parts] + ["source.dxr"]):
                                      "/".join(parts + ["SOURCE.DXR"])})
    short = "/".join(["short"] * 9 + ["SOURCE.DXR"])
    long = "/".join(["Long"] * 9 + ["source.dxr"])
    source = tmp_path / short
    source.parent.mkdir(parents=True)
    source.write_bytes(b"source")
    with pytest.raises(ValueError, match="more than 256 mixed-name variants"):
        module.install_aliases(tmp_path, {long: short})
    assert not (tmp_path / "Long").exists()


def test_original_observations_require_complete_independent_results():
    for text in ("", "[40, 0, 0, 0, rgb( 255, 255, 255 )]", "arbitrary output"):
        with pytest.raises(ValueError):
            parse_precision(text)
    grid = list(product((40, 41), (0, 1, 63, 64, 127, 128, 254, 255), (0, 50),
                        (0, 1, 2, 63, 64, 65, 127, 128, 129, 191, 192, 253, 254, 255)))
    def output(rows):
        return "\n".join(f"[{i}, {s}, {f}, {b}, rgb( 0, 0, 0 )]" for i, s, f, b in rows)
    assert len(parse_precision(output(grid))) == 448
    grid[0] = (40, 0, 3, 0)
    with pytest.raises(ValueError, match="exact 448-input grid"):
        parse_precision(output(grid))


def test_installed_launcher_recipe_is_pinned_to_the_original_disc(tmp_path):
    game = replace(GameSpec.load("loewenzahn-4"), root=tmp_path)
    host = game.directory / "host"
    host.mkdir(parents=True)
    (host / "source-policy.json").write_text('{"native_stage":true}')
    (tmp_path / "SETUP/WIN95").mkdir(parents=True)
    (tmp_path / "SETUP/WIN95/LOEWE4.EXE").write_bytes(b"synthetic installed launcher")
    recipe = {"schema_version": 1, "source_sha256": game.source["sha256"],
              "launcher": "SETUP/WIN95/LOEWE4.EXE", "cwd": "SETUP/WIN95"}
    (host / "reference.json").write_text(json.dumps(recipe))
    layout = layout_for(game, tmp_path)
    assert layout["launcher"] == recipe["launcher"] and layout["cwd"] == recipe["cwd"]
    recipe["source_sha256"] = "0" * 64
    (host / "reference.json").write_text(json.dumps(recipe))
    with pytest.raises(ValueError, match="selected source"):
        layout_for(game, tmp_path)


def synthetic_iso(path):
    data = bytearray(20 * 2048)
    for sector, kind, label in ((16, 1, "PRIMARY"), (17, 2, "ORIGINAL CD")):
        block = memoryview(data)[sector * 2048:(sector + 1) * 2048]
        block[0], block[1:7] = kind, b"CD001\x01"
        block[40:72] = (label.ljust(16).encode("utf-16-be") if kind == 2
                        else label.encode().ljust(32, b" "))
        if kind == 2:
            block[88:91] = b"%/E"
    data[18 * 2048:18 * 2048 + 7] = b"\xffCD001\x01"
    path.write_bytes(data)
    return path


def test_optical_metadata_uses_source_joliet_label_and_windows_version_byte_order(tmp_path):
    image = synthetic_iso(tmp_path / "disc.iso")
    win95 = optical_metadata(image)
    win7 = optical_metadata(image, windows_version="win7")
    assert win95["label"] == "ORIGINAL CD" and win95["namespace"] == "joliet"
    assert win95["descriptor_sector"] == 17
    assert win95["serial_hex"] == "068a6957"
    assert win7["serial_hex"] == "57698a06"
    assert win95["descriptor_sha256"] == win7["descriptor_sha256"]


@pytest.mark.parametrize("letter", ["C", "Z", "d:", "../D", "D:", ""])
def test_optical_drive_cannot_replace_system_or_host_namespace(tmp_path, letter):
    with pytest.raises(ValueError, match="letter"):
        optical_metadata(synthetic_iso(tmp_path / "disc.iso"), letter=letter)


def test_optical_raw_backing_refuses_non_cooked_images(tmp_path):
    with pytest.raises(ValueError, match="cooked"):
        optical_metadata(synthetic_iso(tmp_path / "disc.iso"), layout=(2352, 24), raw_image=True)


def test_optical_recipe_has_no_serial_override_and_requires_matching_disc(tmp_path):
    game = replace(GameSpec.load("loewenzahn-4"), root=tmp_path)
    host = game.directory / "host"
    host.mkdir(parents=True)
    (host / "source-policy.json").write_text('{"native_stage":true}')
    (tmp_path / "SETUP/WIN95").mkdir(parents=True)
    (tmp_path / "SETUP/WIN95/LOEWE4.EXE").write_bytes(b"synthetic unchanged projector")
    image = synthetic_iso(tmp_path / "disc.iso")
    game = replace(game, source=game.source | {
        "sha256": hashlib.sha256(image.read_bytes()).hexdigest()})
    recipe = {"schema_version": 1, "source_sha256": game.source["sha256"],
              "launcher": "SETUP/WIN95/LOEWE4.EXE", "cwd": "SETUP/WIN95",
              "optical_drive": {"letter": "D", "raw_image": True}}
    path = host / "reference.json"
    path.write_text(json.dumps(recipe))
    assert layout_for(game, tmp_path, iso=image)["optical_drive"]["label"] == "ORIGINAL CD"
    recipe["optical_drive"]["serial_hex"] = "00d086c0"
    path.write_text(json.dumps(recipe))
    with pytest.raises(ValueError, match="letter/raw_image only"):
        layout_for(game, tmp_path, iso=image)
    del recipe["optical_drive"]["serial_hex"]
    path.write_text(json.dumps(recipe))
    image.write_bytes(image.read_bytes() + b"different disc")
    with pytest.raises(ValueError, match="selected source"):
        layout_for(game, tmp_path, iso=image)


def test_private_optical_copy_keeps_payloads_and_aliases_and_volume_metadata(tmp_path):
    module = runner(GameSpec.load("loewenzahn-4").root)
    source = tmp_path / "source"
    (source / "DATA").mkdir(parents=True)
    payload = b"synthetic unchanged original movie"
    (source / "DATA/LONGNA~1.DXR").write_bytes(payload)
    metadata = optical_metadata(synthetic_iso(tmp_path / "disc.iso"), raw_image=True)
    output = tmp_path / "disc"
    module.prepare_optical_source(source, output, {
        "optical_drive": metadata, "aliases": {"Data/Long Name.dxr": "DATA/LONGNA~1.DXR"}})
    assert (output / "DATA/Long Name.dxr").read_bytes() == payload
    assert (source / "DATA/LONGNA~1.DXR").read_bytes() == payload
    assert not (source / ".windows-label").exists()
    assert (output / ".windows-label").read_text() == "ORIGINAL CD\n"
    assert (output / ".windows-serial").read_text() == metadata["serial_hex"] + "\n"
    devices = tmp_path / "dosdevices"
    devices.mkdir()
    module.map_optical_drive(devices, metadata, mount=output, image=tmp_path / "disc.iso")
    assert (devices / "d:").resolve() == output
    assert (devices / "d::").resolve() == tmp_path / "disc.iso"
    assert not (devices / "c:").exists() and not (devices / "z:").exists()


@pytest.mark.parametrize("name", ["../escape", "C:/escape", "dir\\escape", "/escape"])
@pytest.mark.parametrize("position", ["long", "short"])
def test_optical_aliases_cannot_escape_namespace(tmp_path, name, position):
    module = runner(GameSpec.load("loewenzahn-4").root)
    (tmp_path / "SOURCE.DXR").write_bytes(b"source")
    with pytest.raises(ValueError, match="alias escapes"):
        module.install_aliases(tmp_path, {name: "SOURCE.DXR"} if position == "long"
                               else {"Long.dxr": name})


@pytest.mark.parametrize("position", ["source", "parent", "target"])
def test_optical_aliases_reject_symlinks_outside_private_root(tmp_path, position):
    module = runner(GameSpec.load("loewenzahn-4").root)
    root, outside = tmp_path / "private", tmp_path / "outside"
    root.mkdir()
    outside.mkdir()
    (outside / "Original.dxr").write_bytes(b"source")
    (root / "SOURCE.DXR").write_bytes(b"source")
    if position == "source":
        (root / "SOURCE.DXR").unlink()
        (root / "SOURCE.DXR").symlink_to(outside / "Original.dxr")
    elif position == "parent":
        (root / "Long").symlink_to(outside, target_is_directory=True)
    else:
        (root / "Long").mkdir()
        (root / "Long/Original.dxr").symlink_to(outside / "Original.dxr")
    with pytest.raises(ValueError, match="escapes installed source"):
        module.install_aliases(root, {"Long/Original.dxr": "SOURCE.DXR"})
    assert list(outside.iterdir()) == [outside / "Original.dxr"]
    assert (outside / "Original.dxr").read_bytes() == b"source"

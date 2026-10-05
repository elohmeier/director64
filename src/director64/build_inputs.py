"""Reject stale generated assets after source, compiler, schema or SDK changes."""

from __future__ import annotations

import json

from .iso import file_sha256


def identity(game):
    paths = [
        game.directory / "game.toml",
        game.root / "package-lock.json",
        game.root / "config/provenance.toml",
    ]
    paths += sorted((game.directory / "host").glob("*.json"))
    if postprocessor := game.data.get("port", {}).get("asset_postprocessor"):
        paths.append(game.directory / "host" / (postprocessor + ".py"))
    if game.data.get("port", {}).get("native_compatibility"):
        paths.append(game.directory / "host/compatibility.py")
    for relative in game.data.get("port", {}).get("asset_inputs", []):
        paths.append(game.directory / relative)
    # Ignored local overrides: adding, changing or removing one invalidates
    # the receipt like any other input.
    optional = [
        game.directory / r for r in game.data.get("port", {}).get("optional_asset_inputs", [])
    ]
    paths += [
        game.root / "src/director64" / name
        for name in (
            "lingo.py",
            "aot.py",
            "director.py",
            "iso.py",
            "full_assets.py",
            "image_pack.py",
            "build_inputs.py",
        )
    ]
    # The Rust compiler stage is the bytecode generator; tests' synthetic
    # roots have no crate, and their receipts cover what they have.
    paths += [
        p
        for p in (game.root / "compiler/Cargo.toml", game.root / "compiler/Cargo.lock")
        if p.is_file()
    ]
    paths += sorted((game.root / "compiler/src").rglob("*.rs"))
    paths += sorted((game.root / "tools/director").glob("*.mjs"))
    paths += [p for p in sorted((game.root / "tools/projectorrays").glob("*")) if p.is_file()]
    paths += [
        p
        for p in sorted((game.root / "tools/fonts").glob("*"))
        if p.is_file() and p.suffix in {".py", ".cpp", ".json"}
    ]
    # The compiler/runtime ABI is expressed by these headers.
    paths += [
        game.root / "runtime/director/director.h",
        game.root / "runtime/director/cursor.h",
        game.root / "runtime/lingo/lingo_runtime.h",
    ]
    return {
        "schema_version": 1,
        "game": game.slug,
        "source": game.source["sha256"],
        "inputs": {str(p.relative_to(game.root)): file_sha256(p) for p in paths}
        | {
            str(p.relative_to(game.root)): file_sha256(p) if p.is_file() else None for p in optional
        },
    }


def generated(game):
    paths = [
        game.work / "aot/program.json",
        game.work / "aot/c/manifest.json",
        game.work / "director/model.json",
        game.work / "director/packed.json",
    ]
    if (game.work / "aot/native-program.json").exists():
        paths.append(game.work / "aot/native-program.json")
    font_dir = game.work / "director/fonts"
    if font_dir.exists():
        paths += [p for p in sorted(font_dir.iterdir()) if p.suffix in {".otf", ".pfr", ".json"}]
    paths += [
        p
        for directory in ("aot/c", "director/c")
        for p in sorted((game.work / directory).iterdir())
        if p.suffix in {".c", ".h", ".inc"} and not p.name.startswith("replay.")
    ]
    return {str(p.relative_to(game.work)): file_sha256(p) for p in paths}


def record(game):
    value = identity(game)
    value["generated"] = generated(game)
    (game.work / "build-inputs.json").write_text(json.dumps(value, indent=2) + "\n")


def verify(game):
    path = game.work / "build-inputs.json"
    if not path.exists():
        raise ValueError("missing build inputs receipt; run assets for the selected game")
    receipt = json.loads(path.read_text())
    expected = identity(game)
    if any(receipt.get(k) != v for k, v in expected.items()):
        raise ValueError("source/compiler/ABI inputs changed; rebuild assets for the selected game")
    if receipt.get("generated") != generated(game):
        raise ValueError("generated inputs changed; rebuild assets for the selected game")
    if file_sha256(game.media) != game.source["sha256"]:
        raise ValueError("source container hash changed")

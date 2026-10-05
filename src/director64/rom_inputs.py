"""Reject a ROM that was not built from the sources in the checkout.

The asset receipt (build_inputs) keeps generated code honest, but nothing tied
a compiled ROM to the runtime it was compiled from: a capture copies whatever
ROM the profile directory holds. Lernerfolg's probe ROM was not rebuilt after a
runtime change, its capture ran the previous ROM byte for byte, and the
identical pacing it reported was taken as proof that the change cost nothing.
A build now records what went into each ROM, and every capture verifies it.
"""

from __future__ import annotations

import json
from pathlib import Path

from .iso import file_sha256

SOURCE_SUFFIXES = {".c", ".h", ".inc", ".S", ".s", ".ld", ".mk", ""}


def _sources(game) -> list[Path]:
    paths = [
        p
        for p in sorted((game.root / "runtime").rglob("*"))
        if p.is_file() and p.suffix in SOURCE_SUFFIXES
    ]
    paths += [p for p in sorted((game.root / "platforms/n64").rglob("*")) if p.is_file()]
    game_runtime = game.directory / "runtime"
    if game_runtime.exists():
        paths += [p for p in sorted(game_runtime.rglob("*")) if p.is_file()]
    receipt = game.work / "build-inputs.json"
    if receipt.exists():
        paths.append(receipt)
    return paths


def inputs(game) -> dict[str, str]:
    return {str(p.relative_to(game.root)): file_sha256(p) for p in _sources(game)}


def _receipt(game, profile: str) -> Path:
    return game.work / "n64" / profile / "rom-inputs.json"


def record(game, profile: str, rom: Path) -> None:
    path = _receipt(game, profile)
    path.parent.mkdir(parents=True, exist_ok=True)
    value = {"schema_version": 1, "rom_sha256": file_sha256(rom), "inputs": inputs(game)}
    path.write_text(json.dumps(value, indent=2) + "\n")


def verify(game, profile: str, rom: Path) -> None:
    """Raise unless `rom` is the ROM the last build of `profile` produced from
    the sources now in the checkout."""
    path = _receipt(game, profile)
    command = "probe" if profile == "probe" else "build"
    if not path.exists():
        raise ValueError(
            f"{profile} ROM has no build receipt; run `director64 {command} --game {game.slug}`"
        )
    receipt = json.loads(path.read_text())
    if receipt.get("rom_sha256") != file_sha256(rom):
        raise ValueError(f"{rom.name} is not the ROM its last {profile} build produced")
    recorded, current = receipt.get("inputs", {}), inputs(game)
    changed = sorted(
        key for key in recorded.keys() | current.keys() if recorded.get(key) != current.get(key)
    )
    if changed:
        shown = ", ".join(changed[:5])
        if len(changed) > 5:
            shown += f" and {len(changed) - 5} more"
        raise ValueError(
            f"{profile} ROM is stale ({shown} changed since it was built); "
            f"run `director64 {command} --game {game.slug}`"
        )

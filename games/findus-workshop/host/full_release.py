"""Assemble a functional release receipt only from passing, matching local evidence."""

from __future__ import annotations

import json
from pathlib import Path

from director64.project import work_dir

from .full_validation import digest, validate_rom

CORE = [
    str(p)
    for directory in ("runtime", "platforms/n64", "games/findus-workshop/runtime")
    for p in sorted(Path(directory).rglob("*"))
    if p.suffix in {".c", ".h"}
]
CORE += [f"{work_dir()}/director/model.json", f"{work_dir()}/aot/c/manifest.json"]


def matches_release(snapshot: dict, source: dict, rom: dict) -> bool:
    """Require the same native engine, complete DFS and pinned asset toolchain."""
    return (
        snapshot.get("status") == "passing"
        and snapshot.get("dfs_sha256") == rom["dfs_sha256"]
        and snapshot.get("toolchain") == rom["toolchain"]
        and all(snapshot.get("source_sha256", {}).get(name) == sha for name, sha in source.items())
    )


def main():
    root = Path.cwd()
    rom = validate_rom(root, "release")
    current = {p: digest((root / p).read_bytes()) for p in CORE}
    host_path = root / f"{work_dir()}/director/host-sanitized/validation.json"
    host = json.loads(host_path.read_text())
    if host["status"] != "passing" or not host["sanitizers"] or host["scenarios"] < 75:
        raise ValueError("complete sanitized host suite is required")
    for name in (
        "runtime/director/director.c",
        "runtime/director/director.h",
        "runtime/lingo/lingo_runtime.c",
        "runtime/lingo/lingo_runtime.h",
        f"{work_dir()}/director/model.json",
        f"{work_dir()}/aot/c/manifest.json",
        "games/findus-workshop/tests/full_probe.py",
        "games/findus-workshop/tests/director_probe.c",
    ):
        if host["source_sha256"].get(name) != digest((root / name).read_bytes()):
            raise ValueError(f"host evidence predates current source: {name}")
    for name, sha in host["source_sha256"].items():
        if digest((root / name).read_bytes()) != sha:
            raise ValueError(f"host evidence predates compiled input: {name}")
    required = {"SNIKBOD", "VEMORY", "START", "HALLTYST", *(f"KONSTR{i:02}" for i in range(1, 13))}
    required |= {
        e["target"] for e in json.loads((host_path.parent / "activities-START.json").read_text())
    }
    selected = {}
    for path in sorted(
        (root / f"{work_dir()}/captures").glob("full.*/validation.json"),
        key=lambda p: p.stat().st_mtime,
    ):
        result = json.loads(path.read_text())
        snapshot = path.parent / "inputs/validation.json"
        if result.get("status") != "passing" or not snapshot.exists():
            continue
        if not matches_release(json.loads(snapshot.read_text()), current, rom):
            continue
        target = result["checkpoint"][0]
        if target in required:
            selected[target] = {
                "validation": str(path.relative_to(root)),
                "sha256": digest(path.read_bytes()),
                "rom_sha256": result["rom_sha256"],
                "controller": result.get("controller", "recorded-mouse"),
            }
    missing = required - selected.keys()
    if missing:
        raise ValueError(f"missing matching N64 evidence: {sorted(missing)}")
    for target in ("SNIKBOD", "VERKORK", "BREDHOGN"):
        evidence = json.loads((root / selected[target]["validation"]).read_text())
        if evidence.get("reload", {}).get("status") != "passing":
            raise ValueError(f"independent emulator save reboot required: {target}")
    boots = []
    for path in (root / f"{work_dir()}/captures").glob("manual-boot.*/validation.json"):
        value = json.loads(path.read_text())
        if value.get("rom_sha256") == rom["rom_sha256"] and all(
            value.get(case, {}).get("status") == "passing" for case in ("cold", "corrupt")
        ):
            boots.append(path)
    if not boots:
        raise ValueError("matching manual boot and corrupt-save capture required")
    boot = max(boots, key=lambda p: p.stat().st_mtime)
    receipt = {
        "version": 1,
        "status": "functional-emulator-validated",
        "rom": rom,
        "hardware_qualified": False,
        "original_presentation_certified": False,
        "host": {
            "validation": str(host_path.relative_to(root)),
            "sha256": digest(host_path.read_bytes()),
            "scenarios": host["scenarios"],
        },
        "manual_boot": {
            "validation": str(boot.relative_to(root)),
            "sha256": digest(boot.read_bytes()),
        },
        "source_sha256": current,
        "accountability": {
            "path": f"{work_dir()}/director/accountability.json",
            "sha256": digest((root / f"{work_dir()}/director/accountability.json").read_bytes()),
        },
        "n64_scenarios": selected,
    }
    output = root / f"{work_dir()}/director/release.json"
    output.write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"FULL_RELEASE={output} scenarios={len(selected)} rom_sha256={rom['rom_sha256']}")


if __name__ == "__main__":
    main()

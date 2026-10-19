"""Shared compiler rendering preflight for a recovered, normalized model.

This inventories authored requirements, not reachability. Strict mode rejects
score features the selected renderer cannot draw. It does not certify visuals.
"""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path


def audit_model(model: dict, target: str) -> dict:
    """Use the same capability implementation as native and Wasm packaging."""
    from .project import converter

    result = subprocess.run(
        [*converter(), "capabilities", "-", target],
        input=json.dumps(model), text=True, capture_output=True, check=False,
    )
    if result.returncode:
        raise ValueError(result.stderr.strip())
    return json.loads(result.stdout)


def require_capabilities(model: dict, target: str, identity: str) -> dict:
    report = audit_model(model, target)
    if report["problems"]:
        raise ValueError(f"{identity}: resource conversion is incomplete: {report['problems']}")
    if report["findings"]:
        finding = report["findings"][0]
        at = finding["examples"][0]
        feature = finding["code"] + (f" {finding['ink']}" if "ink" in finding else "")
        if "kind" in finding:
            feature += f" {finding['kind']}"
        location = "".join(
            f" {key} {at[key]}"
            for key in (
                "film_cast", "film_member", "frame", "sprite", "cast", "member", "font", "size"
            )
            if key in at
        )
        raise ValueError(
            f"{identity}: renderer {target} capability {feature} "
            f"at {at.get('movie', '?')}{location}"
        )
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("--target", choices=("web", "android"), required=True)
    parser.add_argument(
        "--strict", action="store_true", help="exit 1 on findings or conversion problems"
    )
    args = parser.parse_args(argv)
    try:
        report = audit_model(json.loads(args.model.read_text()), args.target)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, f"audit input error: {error}\n")
    print(json.dumps(report, indent=2))
    return int(args.strict and bool(report["findings"] or report["problems"]))


if __name__ == "__main__":
    raise SystemExit(main())

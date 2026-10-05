"""Capture every activity and construction in independent N64 probe sandboxes."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from pathlib import Path

from director64.project import work_dir

from .full_capture import capture, prepare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("targets", nargs="*")
    parser.add_argument(
        "--reports", type=Path, default=Path(f"{work_dir()}/director/host-sanitized")
    )
    parser.add_argument("--jobs", type=int, choices=range(1, 5), default=3)
    args = parser.parse_args()
    Path(f"{work_dir()}/captures").mkdir(parents=True, exist_ok=True)
    suite = Path(tempfile.mkdtemp(prefix="full-suite.", dir=f"{work_dir()}/captures"))
    results = []
    cases = [
        (path, e["target"])
        for path in (
            args.reports / "activities-START.json",
            args.reports / "construction-START.json",
        )
        for e in json.loads(path.read_text())
    ]
    # Exercise actual START/profile initialization on the target as well as
    # direct-activity fixtures. This target is distinct from VEMORY:1..3.
    cases.append((args.reports / "routes-START.json", "VEMORY"))
    cases.append((args.reports / "garden-START.json", "START"))
    cases.append((args.reports / "credits-HALLTYST.json", "HALLTYST"))
    if not args.targets or "SNIKBOD" in args.targets:
        # This shorter input-only fixture exercises the same map collector as
        # the complete boot-to-treasure host journey, without replaying reboots.
        report = suite / "map.json"
        executable = f"{work_dir()}/native/director-probe"
        if args.reports.name.endswith("-sanitized"):
            executable += "-sanitized"
        # CheckMagicPlace chooses 34 or 35 with random(2). Obtain both legal
        # save artifacts by real host input, never by editing a saved value.
        outcomes = {}
        variants = [(0, False), (0, True)] + [(d, False) for d in (60, 300, 600, 1200, 2400, 4800)]
        for delay, animation in variants:
            oracle = suite / f"map-delay-{delay}-animation-{int(animation)}.json"
            subprocess.run(
                [
                    sys.executable,
                    "games/findus-workshop/tests/full_probe.py",
                    "map",
                    "SNIKBOD",
                    "--executable",
                    executable,
                    "--output",
                    str(oracle),
                    "--magic-delay",
                    str(delay),
                    *(["--magic-animation"] if animation else []),
                ],
                check=True,
            )
            entry = json.loads(oracle.read_text())[0]
            outcomes.setdefault(entry["state"]["save_contents"][0], entry)
            if len(outcomes) == 2:
                break
        if len(outcomes) != 2:
            raise ValueError("input-only host map oracles did not cover both random treasures")
        report.write_text(json.dumps(list(outcomes.values()), indent=2) + "\n")
        cases.append((report, "SNIKBOD"))
    unknown = set(args.targets) - {t for _, t in cases}
    if unknown:
        raise SystemExit(f"unknown targets: {sorted(unknown)}")

    def record(target, log_path, run, error=None):
        results.append(
            {
                "target": target,
                "status": "failed" if error else "passing",
                "log": str(log_path),
                "capture": str(run) if run else None,
                "error": str(error) if error else None,
            }
        )
        (suite / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"{target}: {'FAIL' if error else 'PASS'} {run or log_path}", flush=True)

    pending = {}

    def collect(done):
        for future in done:
            target, log_path, run = pending.pop(future)
            try:
                future.result()
            except Exception as error:
                record(target, log_path, run, error)
            else:
                record(target, log_path, run)

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for report, target in cases:
            if args.targets and target not in args.targets:
                continue
            if len(pending) >= args.jobs:
                collect(wait(pending, return_when=FIRST_COMPLETED).done)
            log_path = suite / f"{target.replace(':', '-')}.log"
            run = None
            try:
                with log_path.open("x") as log:
                    command = [
                        "director64",
                        "probe",
                        "--game",
                        "findus-workshop",
                        str(report),
                        target,
                    ]
                    if target in {"MOSSEN", "FINNDUNK", "SNIKBOD"} or target.startswith(
                        ("PLOCKSPL:", "VEMORY:")
                    ):
                        command += ["--adaptive"]
                    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
                    # All shared build inputs are frozen before another build
                    # can start. Only isolated emulator processes run in parallel.
                    run = prepare(Path.cwd())
                    log.write(f"FULL_CAPTURE={run}\n")
            except Exception as error:
                record(target, log_path, run, error)
            else:
                pending[pool.submit(capture, run)] = (target, log_path, run)
        collect(wait(pending).done)
    print(f"FULL_SUITE={suite}")
    if not all(r["status"] == "passing" for r in results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()

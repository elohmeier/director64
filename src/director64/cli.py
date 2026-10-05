"""Director64 checkout command line. Select a game for every source-dependent action."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys

from .project import GameSpec, repository


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def director_stage(game, *args):
    """One stage of the Rust converter (`director64-aot director ...`)."""
    from .aot import RUST_GENERATOR

    binary = game.root / RUST_GENERATOR
    if not binary.is_file():
        raise ValueError(f"{binary} is not built; run mise run compiler")
    run(binary, "director", *args)


def recover(game):
    from .iso import file_sha256
    from .media import extract_source

    recovery = game.data.get("recovery", game.data.get("port"))
    if not recovery:
        raise ValueError(f"{game.slug} has no audited recovery policy")
    policy = json.loads((game.directory / "host/source-policy.json").read_text())
    if policy.get("director_version", recovery["director_version"]) != recovery["director_version"]:
        raise ValueError("manifest and recovery policy Director versions differ")
    game.work.mkdir(parents=True, exist_ok=True)
    extract_source(game)
    analysis = game.work / "analysis"
    media = game.work / "extracted" / recovery["media_root"]
    policy = game.directory / "host/source-policy.json"
    dumps = analysis / "dumps"
    # ProjectorRays parses every Director file once (tools/director/dump.mjs);
    # every stage after it is the Rust converter reading those dumps.
    director_stage(game, "plan", media, policy, dumps)
    run("node", "tools/director/dump.mjs", dumps / "plan.json")
    director_stage(game, "analyze", media, dumps, analysis, policy)
    parser = file_sha256(game.root / "node_modules/projectorrays/dist/projectorrays.wasm")
    director_stage(game, "audit", media, dumps, analysis / "source", policy, parser)
    director_stage(game, "recover", analysis / "source/manifest.json", analysis / "score-recovery")
    run(
        sys.executable, "-m", "director64.lingo", analysis / "lingo", game.work / "aot/program.json"
    )
    from .recovery import report

    report(game)


def assets(game):
    from .toolchain import selected_image

    with selected_image(game.root) as image:
        _assets(game, image["Id"])


def _assets(game, image):
    from .full_assets import pack

    game.require_port()
    recover(game)
    analysis = game.work / "analysis"
    media = game.work / "extracted" / game.data["port"]["media_root"]
    director_stage(
        game,
        "compile",
        game.work / "director",
        analysis / "score-recovery",
        media,
        game.directory / "host/source-policy.json",
        analysis / "dumps",
        *(["--defer-video"] if game.data["port"].get("asset_postprocessor") else []),
    )
    if module := game.data["port"].get("asset_postprocessor"):
        game.host(module).main()
    from .aot import executable_program, generator_command

    program = executable_program(game)
    run(*generator_command(game.root), program, game.work / "aot/c")
    run(
        sys.executable,
        "-m",
        "director64.director",
        game.work / "director/model.json",
        game.work / "director/c",
        "--program",
        program,
    )
    game.host("full_accountability").main()
    pack(game.root, image)
    from .build_inputs import record

    record(game)


def build(game, profile="release", replay_scenario=1):
    from .toolchain import selected_image

    with selected_image(game.root) as image:
        _build(game, image, profile, replay_scenario)


def _build(game, sdk, profile, replay_scenario):
    from .build_inputs import verify
    from .rom_metadata import prepare
    from .toolchain import container_engine, record_build_context

    verify(game)
    game.require_port()
    image = sdk["Id"]
    packed = json.loads((game.work / "director/packed.json").read_text())
    if packed["toolchain"]["image"] != image:
        raise ValueError("asset toolchain image changed; rebuild assets for the selected game")
    port = game.data["port"]
    prepare(game, profile)
    (game.work / "n64").mkdir(parents=True, exist_ok=True)
    arguments = [
        "make",
        "-f",
        "/workdir/platforms/n64/Makefile",
        f"GAME={game.slug}",
        f"WORK=/workdir/{game.work.relative_to(game.root)}",
        f"PROFILE={profile}",
        f"REPLAY_SCENARIO={replay_scenario}",
        f"ROM_TITLE={port['rom_title']}",
        f"ENTRY_MOVIE={port['entry_movie']}",
        f"DIRECTOR_VERSION={port['director_version'] // 100}",
        f"EXTENDED_D6={int(port.get('extended_d6', False))}",
        f"CONTROLLER_COUNT={port.get('controller_count', 1)}",
        f"-j{min(24, os.cpu_count() or 8)}",
    ]
    record_build_context(game.work, image, arguments)
    run(
        container_engine(),
        "run",
        "--rm",
        "--network=none",
        "--volume",
        f"{game.root}:/workdir",
        "--workdir",
        f"/workdir/{game.work.relative_to(game.root)}/n64",
        image,
        *arguments,
    )
    name = game.slug + ("-probe" if profile == "probe" else "")
    from .rom_inputs import record as record_rom_inputs

    record_rom_inputs(game, profile, game.work / "n64" / f"{name}.z64")
    result = game.host("full_validation").validate_rom(game.root, profile)
    game.dist.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(game.work / "n64" / f"{name}.z64", game.dist / f"{name}.z64")
    (game.dist / f"{name}.json").write_text(json.dumps(result, indent=2) + "\n")
    print(game.dist / f"{name}.z64")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    help_text = {
        "native": "Adapter options: --sanitizers --build-only; Workshop: --soak; "
        "Mucklas: --journey --activity --regressions",
        "fuzz": "Options: --duration SECONDS --episodes N --actions N --jobs N --seed N "
        "--movie NAME --replay CASE --no-minimize --output DIR",
        "perf": "Options: --actions N --dwell TICKS --budget-us MEAN --output DIR",
        "working-sets": "Options: --episodes N --actions N --seed S --no-journey --output PATH",
        "parity": "Options: --against PROBE --probe PROBE --episodes N --actions N --seed N "
        "--movie NAME --output DIR (side A defaults to parity/probe-a)",
        "capture": "Adapter options: [SCENARIO ...] --jobs 1..4 --reports PATH",
        "probe": "Adapter arguments: REPORT SCENARIO [--adaptive] [--controller]",
        "summercart": "Use --game SLUG or --all (every executable game's default-source "
        "build). Adapter options: --device PATH (default: only removable FAT/exFAT "
        "partition) --rom PATH (single game only; default: selected build in dist) "
        "--artwork PNG --dry-run --wait-before-unmount (press Enter after manual SD changes)",
        "compatibility": "Adapter options: report|validate --inventory-mode required|optional "
        "--format markdown|json --output PATH",
        "reference": "Adapter options: --replay PATH --output PATH --windows-version VERSION",
        "web": "Options: --serve --port N (default 18064) --no-build (browser player, docs/web.md)",
    }
    sub.add_parser("games", help="list ports and catalogued sources")
    sub.add_parser("doctor", help="report host prerequisites, SDK state and image compatibility")
    for name in (
        "assets",
        "build",
        "native",
        "probe",
        "capture",
        "boot",
        "release",
        "compatibility",
        "summercart",
        "extract",
        "recover",
        "reference",
        "fuzz",
        "perf",
        "parity",
        "working-sets",
        "web",
    ):
        p = sub.add_parser(name, epilog=help_text.get(name))
        if name == "summercart":
            selection = p.add_mutually_exclusive_group(required=True)
            selection.add_argument("--game")
            selection.add_argument("--all", action="store_true", help="copy all executable games")
        else:
            p.add_argument("--game", required=True)
        p.add_argument("--source")
        if name == "build":
            p.add_argument(
                "--reuse-assets", action="store_true", help="skip conversion of verified inputs"
            )
    args, remaining = parser.parse_known_args(argv)
    try:
        os.chdir(repository())
        if args.command == "doctor":
            if remaining:
                parser.error("unexpected arguments")
            from .doctor import main as doctor

            return doctor(repository())
        if args.command == "games":
            if remaining:
                parser.error("unexpected arguments")
            for path in sorted(repository().glob("games/*/game.toml")):
                game = GameSpec.load(path.parent.name)
                print(f"{game.slug:22} {game.data['status']:10} {game.data['title']}")
            return 0
        if args.command in {"assets", "extract", "recover", "boot", "release"} and remaining:
            parser.error("unexpected arguments: " + " ".join(remaining))
        if args.command == "summercart" and args.all:
            if args.source is not None:
                parser.error("--source cannot be used with --all")
            from .summercart import main as summercart

            summercart(["--all", *remaining])
            return 0
        game = GameSpec.load(args.game, args.source)
        game.activate()
        if args.command == "extract":
            from .media import extract_source

            print(extract_source(game))
            return 0
        if args.command == "recover":
            recover(game)
            return 0
        game.require_port()
        sys.argv = [f"director64 {args.command}", *remaining]
        if args.command == "assets":
            assets(game)
        elif args.command == "build":
            if remaining:
                parser.error("unexpected arguments")
            if not args.reuse_assets:
                assets(game)
            build(game)
        elif args.command == "probe":
            game.host("full_replay").main()
            build(game, "probe")
        elif args.command == "fuzz":
            sys.argv = ["native", "--sanitizers", "--build-only"]
            game.host("full_host").main()
            from .fuzz import main as fuzz

            return fuzz(game, remaining)
        elif args.command == "perf":
            sys.argv = ["native", "--build-only"]
            game.host("full_host").main()
            from .perf import main as perf

            return perf(game, remaining)
        elif args.command == "working-sets":
            sys.argv = ["native", "--build-only"]
            game.host("full_host").main()
            from .working_sets import main as working_sets

            return working_sets(game, remaining)
        elif args.command == "parity":
            # An explicit side-B probe is used as built; otherwise the
            # working tree's sanitized probe is rebuilt first.
            if "--probe" not in remaining:
                sys.argv = ["native", "--sanitizers", "--build-only"]
                game.host("full_host").main()
            from .parity import main as parity

            return parity(game, remaining)
        elif args.command == "web":
            from .web import main as web

            return web(game, remaining)
        elif args.command == "compatibility":
            from .compatibility import main as compatibility

            defaults = [
                "--inventory",
                str(game.work / "analysis/inventory.json"),
                "--lingo-dir",
                str(game.work / "analysis/lingo"),
            ]
            for name in ("capabilities", "scenes", "scenarios"):
                defaults += ["--" + name, str(game.directory / "compatibility" / f"{name}.toml")]
            return compatibility([*defaults, *remaining])
        elif args.command == "reference":
            run(sys.executable, game.directory / "host/reference_runner.py", "capture", *remaining)
        elif args.command == "summercart":
            from .summercart import main as summercart

            summercart()
        else:
            module = {
                "native": "full_host",
                "capture": "full_target",
                "boot": "full_boot",
                "release": "full_release",
            }[args.command]
            game.host(module).main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"director64: {error}\n")
    return 0

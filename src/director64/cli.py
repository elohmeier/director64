"""Director64 checkout command line. Select a game for every source-dependent action."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

from .project import GameSpec, repository


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


# The workshop's journeys (games/findus-workshop/tests/full_probe.py) that
# start at its entry movie, which is where the web probe starts.
WORKSHOP_JOURNEYS = ("routes", "hubs", "treasure", "exit", "garden", "profiles")


def journey(game, argv):
    """A game's scripted journeys (games/<slug>/host/*_journey.py,
    full_regressions.py) over its web build's probe."""
    from .web import probe_path

    names = sorted(
        p.stem
        for pattern in ("*journey.py", "full_regressions.py")
        for p in (game.directory / "host").glob(pattern)
    )
    workshop = game.directory / "tests/full_probe.py"
    if workshop.is_file():
        names += WORKSHOP_JOURNEYS
    if not names:
        raise ValueError(f"{game.slug} has no scripted journeys")
    parser = argparse.ArgumentParser(prog="director64 journey")
    default = "full_journey" if "full_journey" in names else names[0]
    parser.add_argument("name", nargs="?", default=default, choices=names)
    parser.add_argument("--activity", action="store_true", help="Mucklas' activity journey")
    args = parser.parse_args(argv)
    probe = probe_path(game)
    if args.name in WORKSHOP_JOURNEYS:
        run(sys.executable, workshop, args.name, "--executable", probe)
        return 0
    output = game.work / "journeys" / args.name
    module = game.host(args.name)
    extra = [True] if args.activity else []
    report = module.run(probe, output, *extra)
    print(json.dumps(report))
    if report.get("status", "passing") != "passing":
        raise ValueError(f"{args.name} failed; inspect {output}")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    help_text = {
        "fuzz": "Options: --duration SECONDS --episodes N --actions N --jobs N --seed N "
        "--movie NAME --replay CASE --no-minimize --output DIR --probe EXECUTABLE "
        "--prelude FILE",
        "parity": "Options: --against PROBE --probe PROBE --episodes N --actions N --seed N "
        "--movie NAME --output DIR (side A defaults to the web build kept at parity/site-a)",
        "journey": "Arguments: [NAME] --activity",
        "reference": "Adapter options: --replay PATH --output PATH --windows-version VERSION",
        "convert": "Options: --force --output DIR (default build/<slug>/<source>/converted)",
        "web": "Options: --reconvert --serve --port N (default 18064) --no-build (docs/web.md)",
    }
    sub.add_parser("games", help="list ports and catalogued sources")
    sub.add_parser(
        "android",
        help="the Android TV player, one APK per game: build | install --game SLUG",
        add_help=False,
    )
    for name in (
        "convert",
        "web",
        "extract",
        "repair-media",
        "fuzz",
        "parity",
        "journey",
        "reference",
    ):
        p = sub.add_parser(name, epilog=help_text.get(name))
        p.add_argument("--game", required=True)
        p.add_argument("--source")
    args, remaining = parser.parse_known_args(argv)
    try:
        os.chdir(repository())
        if args.command == "android":
            from .android import main as android

            return android(remaining)
        if args.command == "games":
            if remaining:
                parser.error("unexpected arguments")
            for path in sorted(repository().glob("games/*/game.toml")):
                game = GameSpec.load(path.parent.name)
                print(f"{game.slug:22} {game.data['status']:10} {game.data['title']}")
            return 0
        if args.command == "extract" and remaining:
            parser.error("unexpected arguments: " + " ".join(remaining))
        game = GameSpec.load(args.game, args.source)
        game.activate()
        if args.command == "extract":
            from .media import extract_source

            print(extract_source(game))
            return 0
        if args.command == "repair-media":
            from .media_repair import main as repair_media

            return repair_media(game, remaining)
        game.require_port()
        sys.argv = [f"director64 {args.command}", *remaining]
        if args.command == "convert":
            from .convert import main as convert

            return convert(remaining)
        if args.command == "web":
            from .web import main as web

            return web(game, remaining)
        if args.command == "fuzz":
            from .fuzz import main as fuzz

            return fuzz(game, remaining)
        if args.command == "parity":
            from .parity import main as parity

            return parity(game, remaining)
        if args.command == "journey":
            return journey(game, remaining)
        if args.command == "reference":
            run(sys.executable, game.directory / "host/reference_runner.py", "capture", *remaining)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"director64: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

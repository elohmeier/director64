# Director64

Director64 plays Macromedia/Adobe Director CD-ROM games on Android TV. It
converts a game's movies, Lingo scripts, score and media from the original
disc into a game package, and plays that package on a small C runtime: one
APK per game, played with a gamepad, the remote or a mouse. The same runtime
also builds as a standalone web app for one game.

**No game data is included.** You need your own copy of a supported disc;
everything is converted locally. Director64 is an independent fan project,
not affiliated with the games' publishers or rights holders. APKs and web
builds contain the converted game and must not be redistributed.

## Supported games

| Game | Director | Status |
| --- | --- | --- |
| [Pettersson und Findus](games/findus-workshop/README.md) (Findus' workshop) | 6 | Supported |
| [Autos bauen mit Willy Werkel](games/willy-werkel-cars/README.md) | 6 | Experimental |
| [Findus wartet auf Weihnachten](games/findus-christmas/README.md) | 7 | Experimental |
| [Findus bei den Mucklas](games/findus-mucklas/README.md) | 8 | Experimental |
| [Die Gespenster-Erschreckmaschine](games/findus-ghosts/README.md) | 8.5 | Experimental |
| [Hühnertanz und Katerzirkus](games/findus-circus/README.md) | 8.5 | Experimental |
| [Der Torten werfende Geburtstagskater](games/findus-birthday/README.md) | 8.5 | Experimental |
| [Löwenzahn 1](games/loewenzahn-1/README.md) | 5 | Experimental |
| [Löwenzahn 6](games/loewenzahn-6/README.md) | 8.5 | Experimental |
| [Löwenzahn 7](games/loewenzahn-7/README.md) | 8.5 | Experimental |
| [Löwenzahn 8](games/loewenzahn-8/README.md) | 8.5 | Experimental |
| [Löwenzahn Adventskalender](games/loewenzahn-advent/README.md) | 8.5 | Experimental |
| [25 Jahre Löwenzahn: Optik](games/loewenzahn-optik/README.md) | 8.5 | Experimental |
| [Lernerfolg Grundschule Deutsch 1-2](games/lernerfolg-deutsch-1-2/README.md) | 10 | Experimental |

Each port supports one edition, identified by its disc image's SHA-256
(alternate rips of the same pressing may be listed too).
[docs/games.md](docs/games.md) lists every catalogued source.

## Setup

Requirements: Linux (x86-64), Docker or a compatible engine,
[mise](https://mise.jdx.dev/), a Rust toolchain, a C and C++ compiler, and
FFmpeg with libvpx and libopus.

```sh
git clone --recurse-submodules https://github.com/elohmeier/director64.git
cd director64
mise trust && mise install
mise run setup          # Python and Node dependencies, the Rust converter
uv run --locked director64 games
```

Place your disc image at `media/<game>/<source-id>/<filename>` (ignored by
git); `games/<game>/game.toml` names the source id, filename and SHA-256 it
expects.

## Commands

Every game command takes `--game <slug>` and optionally `--source <id>`.

| Command | What it does |
| --- | --- |
| `director64 games` | List ports and catalogued sources |
| `director64 convert` | Convert the disc into `build/<game>/<source>/converted/` |
| `director64 android build` | Build the game's APK ([docs/android.md](docs/android.md)) |
| `director64 android install` | `adb install -r` the built APK (`--serial host:port`) |
| `director64 web` | Build the standalone web app; `--serve` serves it ([docs/web.md](docs/web.md)) |
| `director64 extract` | Extract the disc image into `build/<game>/<source>/extracted/` |
| `director64 repair-media` | Make a separately pinned repaired disc image ([architecture](docs/architecture.md#media-repairs)) |
| `director64 fuzz` | Randomized input against the web build's probe ([docs/fuzzing.md](docs/fuzzing.md)) |
| `director64 parity` | Compare two builds state by state |
| `director64 journey` | Run a game's scripted journeys |
| `director64 reference` | Capture the original projector under Wine (workshop only) |

Run them as `uv run --locked director64 ...`. Development checks:

```sh
mise run check                 # converter build, lint, Python, Node and C tests
mise run test-native           # runtime and game C tests
mise run test-android-host     # Android keyboard and JNI checks (JDK 11+)
```

## Outputs

| Path | Content |
| --- | --- |
| `build/<game>/<source>/converted/` | The converted game: `game.d64p` and its asset packs, shared by APK and web app |
| `build/<game>/<source>/conversion/analysis/` | Recovered Lingo, score records and source audit |
| `build/<game>/<source>/{fuzz,parity,journeys}/` | Test runs |
| `build/tools/` | Host tools the converter builds on first use |
| `dist/<game>/<source>/director64-<game>.apk` | The signed APK |
| `dist/<game>/<source>/web/` | The web app, with its probe beside it |

## Documentation

- [Architecture, conversion and `game.toml` reference](docs/architecture.md)
- [Android TV player](docs/android.md) and [standalone web app](docs/web.md)
- [Fuzzing, parity and journeys](docs/fuzzing.md)
- [Game package format](docs/package-format.md)
- [Catalogued sources](docs/games.md)

## Contributing

Issues and pull requests are welcome. Never attach or commit game media,
files extracted or converted from it, APKs, web builds or captures; describe
a problem with the edition's SHA-256, the command and its output.

## License

[MIT](LICENSE). A few files carry MPL-2.0 or LGPL-2.1-or-later headers; they
and the third-party components are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

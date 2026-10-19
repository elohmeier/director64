# Director64

Director64 plays Macromedia/Adobe Director games, the CD-ROM titles of the
1990s and 2000s, on Android TV. It recovers a game's Director movies, Lingo
scripts and score from the original disc, compiles the scripts ahead of time,
converts the media, and packages the result with a small C runtime as one
APK per game, played with a gamepad, the remote or a mouse. The same runtime
also builds as a standalone web app for one game.

**No game data is included.** Director64 is a converter and engine; you need
your own copy of a supported game's disc. Everything is converted locally on
your machine; nothing is uploaded. Director64 is an independent fan project,
not affiliated with or endorsed by the games' publishers or rights holders.
Game titles are trademarks of their respective owners. APKs and web builds
made with Director64 contain the converted game and must not be
redistributed.

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

Each port supports one specific edition, identified by the disc image's
SHA-256. An edition may also list alternate rips of the same pressing, such
as a raw CloneCD `.img`, each pinned by its own SHA-256;
[docs/games.md](docs/games.md) lists the catalogued sources.

## Setup

Requirements: Linux (x86-64), Docker or a compatible engine,
[mise](https://mise.jdx.dev/), a Rust toolchain, a C and C++ compiler, and
FFmpeg with libvpx and libopus. Place
your disc image under the ignored `media/` directory at
`media/<game>/<source-id>/<filename>`; each `games/<game>/game.toml` names
the source id, original filename and SHA-256 it expects.

```sh
git clone --recurse-submodules https://github.com/elohmeier/director64.git
cd director64
mise trust
mise install
mise run setup          # Python and Node dependencies, and the Rust converter
uv run --locked director64 games
```

Mise pins Python, uv and Node. Use `mise run` for tasks and
`uv run --locked director64` for the CLI; update dependency locks
deliberately rather than during setup. Every game-dependent command takes
`--game <slug>`; `--source <source-id>` selects a catalogued edition
explicitly.

## Android TV

```sh
docker build --target sdk -t director64-android:local docker/android
uv run --locked director64 android build --game findus-workshop
uv run --locked director64 android install --game findus-workshop --serial <tv-ip>:<port>
```

The build converts the disc into `build/<game>/<source-id>/converted/`,
compiles the runtime with the NDK, and writes a signed APK to
`dist/<game>/<source-id>/director64-<game>.apk`; `--reconvert` converts
again. Installs are updates, so saves survive as long as the APK is signed
with the same key. See [docs/android.md](docs/android.md) for requirements,
controls and device notes.

## Standalone web app

```sh
uv run --locked director64 web --game findus-workshop --serve
```

This builds one game, converted the same way, as a static site in
`dist/<game>/<source-id>/web/` and serves it on `http://127.0.0.1:18064/`.
It needs a current desktop Chrome or Edge; saves stay in the browser's
origin-private file system. See [docs/web.md](docs/web.md).

## Development

```sh
mise run check                 # converter build, lint, Python, Node and C tests
mise run test-native           # portable runtime and adapter C contracts
mise run test-android-host     # Android keyboard and JNI contracts (JDK 11+)
uv run --locked director64 fuzz --game loewenzahn-1 --duration 900
uv run --locked director64 journey --game findus-workshop
uv run --locked director64 convert --game findus-mucklas
```

`fuzz` floods the web build's probe with randomized input aimed at live
sprites and deduplicates Lingo failures, crashes and hangs into minimized
repros; `parity` compares two probes state by state; `journey` runs a game's
scripted journeys ([docs/fuzzing.md](docs/fuzzing.md)). `convert` converts
the game on its own and keeps the recovered Lingo, scores and source audit
under `build/<game>/<source>/conversion/analysis/` for inspection. `extract`, `repair-media` and `reference` cover disc
extraction, audited media repairs ([docs/video.md](docs/video.md)) and
original-projector reference captures under Wine
([docs/reference-capture.md](docs/reference-capture.md)).

## Documentation

- [Architecture and adding a game](docs/architecture.md) and the
  [roadmap](docs/roadmap.md)
- [Android TV player](docs/android.md) and [standalone web app](docs/web.md)
- [Source catalog](docs/games.md) and [source schema](docs/source-schema.md)
- [Package format](docs/package-format.md)
- [Audio](docs/audio.md), [digital video](docs/video.md) and [Flash playback](docs/flash.md)
- [Fuzzing, parity and journeys](docs/fuzzing.md)
- [Implementation gaps and diagnostic checks](docs/implementation-audit.md)
  and [performance follow-up](docs/performance-audit.md)
- Per-game notes: see the table above.

## Contributing

Issues and pull requests are welcome. Never attach or commit game media,
files extracted or converted from it, APKs, web builds or captures; describe
problems with the edition's SHA-256, the command you ran and its output
instead.

## License

Director64 is released under the [MIT License](LICENSE). A few files carry
their own MPL-2.0 or LGPL-2.1-or-later headers; they and the third-party
components the converter and players use are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

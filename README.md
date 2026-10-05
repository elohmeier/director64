# Director64

Director64 runs Macromedia/Adobe Director games, the CD-ROM titles of the
1990s and 2000s, on the Nintendo 64 and in the browser. It recovers a game's
Director movies, Lingo scripts and score from the original disc, compiles the
scripts ahead of time, and converts the media for a small native runtime. On
the console the mouse becomes up to four controller-driven cursors.

**No game data is included.** Director64 is a converter and engine; you need
your own copy of a supported game's disc. Everything is converted locally on
your machine or in your browser; nothing is uploaded. Director64 is an
independent fan project, not affiliated with or endorsed by the games'
publishers or rights holders. Game titles are trademarks of their respective
owners. ROMs built with Director64 contain the converted game and must not be
redistributed.

## Play in the browser

**<https://elohmeier.github.io/director64/>**

Choose a game, select your disc image (`.iso`), and the page converts and
caches it in the browser, then plays it. Saves stay in browser storage. It
needs a current desktop Chrome or Edge (WebCodecs audio/video encoding and the
origin-private file system); see [docs/web.md](docs/web.md) for details and for
building the site locally.

## Supported games

| Game | Director | N64 | Browser |
| --- | --- | --- | --- |
| [Pettersson und Findus](games/findus-workshop/README.md) (Findus' workshop) | 6 | Supported | Yes |
| [Findus bei den Mucklas](games/findus-mucklas/README.md) | 8 | Experimental | Yes |
| [Findus wartet auf Weihnachten](games/findus-christmas/README.md) | 7 | Experimental | Yes |
| [Autos bauen mit Willy Werkel](games/willy-werkel-cars/README.md) | 6 | Experimental | Yes |
| [Löwenzahn 1](games/loewenzahn-1/README.md) | 5 | Experimental | Yes |
| [Lernerfolg Grundschule Deutsch 1-2](games/lernerfolg-deutsch-1-2/README.md) | 10 | Experimental | Yes |

Each port supports one specific edition, identified by the disc image's
SHA-256; [docs/games.md](docs/games.md) lists the catalogued sources. The
engine's design is in [docs/architecture.md](docs/architecture.md), the plans
in [docs/roadmap.md](docs/roadmap.md) and [docs/web-roadmap.md](docs/web-roadmap.md),
and the history of the ports and their optimization in
[docs/performance-history.md](docs/performance-history.md).

## Building N64 ROMs

Requirements: Linux (x86-64), Docker or a compatible engine for the pinned
libdragon toolchain image, [mise](https://mise.jdx.dev/), and a Rust toolchain
(`cargo`). Place your disc image under the ignored `media/` directory at
`media/<game>/<source-id>/<filename>`; each `games/<game>/game.toml` names the
source id, original filename and SHA-256 it expects. `director64 games` lists
the catalogued games.

```sh
git clone --recurse-submodules https://github.com/elohmeier/director64.git
cd director64
mise trust
mise install
mise run setup          # also builds compiler/, the bytecode compiler (cargo required)
mise run toolchain
mise run doctor
mise run check
uv run --locked director64 games
uv run --locked director64 build --game findus-workshop
```

The existing `.mise-tasks/`, `platforms/n64/`, and packaged Python CLI follow the
recommended N64 project setup. Mise pins Python, uv, and Node and activates the
uv environment; setup consumes `uv.lock` and `package-lock.json`. Use `mise run`
for tasks so their environment is active. Routine Python commands use
`uv run --locked`; update dependency locks deliberately rather than during setup.

`mise run doctor` uses the existing Python environment without synchronizing it.
It reports host prerequisites, the libdragon gitlink, and image compatibility.
FFmpeg/FFprobe and emulators are reported separately because host checks do not
require media or emulator execution.

The toolchain task archives the clean SDK at its recorded commit and fingerprints
the compiler base digest, Dockerfile, builder, build arguments, and platform.
Conversion and compilation reject images built from another recipe and resolve
the selected tag to an immutable image ID. Image or compile-option changes also
invalidate target objects. After a recipe change, run `mise run toolchain`, then
rebuild assets before using `build --reuse-assets`.

Put personal overrides in ignored `mise.local.toml` or export `DOCKER` (one
Docker-compatible executable), `DIRECTOR64_TOOLCHAIN_IMAGE` (default
`director64-toolchain:local`), or `DIRECTOR64_TOOLCHAIN_PLATFORM` (default
`linux/amd64`; the pinned base must support the selected platform). Exported
`DOCKER` takes precedence. Container mounts and offline build behavior retain
the existing Docker contract; an alternative engine must support that contract.
Ordinary setup never advances SDK refs. For an intentional SDK update, review
the fork commit, update the gitlink and `config/provenance.toml` together, then
rebuild the toolchain, assets, and ROM and run the relevant target checks.

The build verifies the selected source hash, recovers Lingo and versioned Director scores,
compiles resumable C handlers and scene metadata, converts assets with the pinned
vendored libdragon, and checks the resulting ROM and DragonFS contents.
Images pack at maximum compression by default; a game may spend ROM bytes on
faster decompression where they pay best through `image_codec_tiers` in its
source policy, which selects a compression level by decompressed size (large
images dominate the bytes a scene decompresses at runtime while holding a
minority of the ROM). A ROM may not exceed 56 MiB, because the SDK's USB
debug channel overwrites the cartridge from there upward; the same source
policy can buy those megabytes back with `image_color_bits` (store four bits
per colour channel instead of five, which packs about 30% smaller) and
`wav_resample_hz`.
Handlers lower to compact stack ops over the frame's collector-rooted temp
array (`lx_*` in `runtime/lingo/lingo_runtime.c`) with per-movie interned
name and float pools, so call sites carry two-byte indices instead of
16-byte struct plumbing — on the MIPS o64 ABI this halves generated
overlay code (Deutsch `scripts_cxt.dso`: 1,087,091 → 443,891 bytes).
Source media remains local and ignored. The default workshop source is
`media/findus-workshop/sha256-fc24ffe284ec/FINDUS.iso`.

Output: `dist/findus-workshop/sha256-fc24ffe284ec/findus-workshop.z64`.
The ROM uses FlashRAM and requires 8 MiB RDRAM.

### Copying to a SummerCart64

To copy the built ROM to a connected SummerCart64 SD card:

```sh
director64 summercart --game findus-workshop
```

This uses the selected game/source build in `dist` and automatically selects the
only removable FAT/exFAT partition. If several match, select one with
`--device /dev/sdX1`. Use `--rom PATH` to copy a different build or `--dry-run` to
check the ROM and card without mounting or copying. The copy is verified and the
card unmounted afterward; existing saves are preserved. Use `uv run --locked director64`
if the project's environment is not active.

To copy every supported or experimental port's default-source build in one card
mount, use:

```sh
director64 summercart --all
```

Every ROM must already exist in `dist`. The command validates all of them before
mounting, then copies each ROM once and unmounts after the set. `--dry-run` checks
the full set and device without mounting. `--rom`, `--artwork`, and `--source`
apply to a single game only. Catalogued games without an executable port are
excluded. A card write failure may leave earlier ROMs from the set updated;
each ROM is verified before it replaces the previous copy.

On Linux the card is mounted with `udisksctl` (UDisks2), which may ask for
polkit authorization. If `mount-media` and `umount-media` commands are on
`PATH`, they are used instead; they take `--device PARTITION` and can wrap
`udisksctl` with a terminal polkit agent, e.g. for use over `ssh -t`.

To make manual changes on the mounted SD card after the copy, use:

```sh
director64 summercart --game findus-mucklas --wait-before-unmount
```

This prints the mount location and waits for Enter before unmounting. The flag
requires an interactive terminal; `--dry-run` still exits without mounting or
waiting. Ctrl-C also attempts to unmount the card.

The ROM embeds a description, release date, port credits, website, and player
count. Its description includes the Director64 version and Git revision, with
`-dirty` for uncommitted changes, plus the 8 MiB RAM requirement. Metadata is
configured per game; [packaging and menu details](docs/rom-metadata.md) describe
the fields and validation.

To also install a front-cover PNG (158×112 or 112×158 pixels):

```sh
director64 summercart --game findus-workshop --artwork /path/to/cover.png
```

Artwork is copied separately to the menu's directory for the ROM's internal
title. Without `--artwork`, existing artwork is preserved.

## Development and validation

```sh
mise run check
uv run --locked director64 native --game findus-workshop --sanitizers --soak
uv run --locked director64 boot --game findus-workshop
uv run --locked director64 capture --game findus-workshop VEMORY
# All emulator scenarios, followed by the strict release receipt:
uv run --locked director64 capture --game findus-workshop
uv run --locked director64 release --game findus-workshop
```

`--source <source-id>` selects a catalogued edition explicitly. Every game-dependent
command requires `--game`; there is no implicit workshop build. `extract` supports
catalogued ISO and ZIP sources independently of port support. `build --reuse-assets`
skips source conversion when working only on runtime code. It verifies the source
hash, compiler/ABI inputs and generated-file receipt before using the cache.

```sh
uv run --locked director64 fuzz --game loewenzahn-1 --duration 900
```

`fuzz` builds the sanitized native probe and floods it with randomized but
hardware-plausible controller input: clicks and drags aimed at live sprite
bounds, raw pad samples, key events where the probe supports them, and
console reboots. Lingo failures, sanitizer reports, crashes and hangs are
deduplicated by signature; each unique case gets a minimized exact-input repro
under `build/<game>/<source-id>/fuzz/<run>/`. Replay a case with
`--replay <case-dir>`. `perf` walks reachable states deterministically on the
plain native probe — the scripted journey, then a systematic click crawl —
and reports per-movie service cost against a host tick budget.
[Details and triage workflow](docs/fuzzing.md).

```sh
uv run --locked director64 recover --game findus-mucklas
```

`recover` verifies and extracts the selected media, audits nested casts and the
embedded launcher, recovers versioned score records, and parses Lingo into a source
AST. It checks handler identities against bytecode and writes a recovery report.
This is a host recovery step; it does not build or qualify a ROM.

## Documentation

- [Architecture and adding a game](docs/architecture.md)
- [Source catalog](docs/games.md) and [source schema](docs/source-schema.md)
- [Browser player](docs/web.md) and its [roadmap](docs/web-roadmap.md)
- [Fuzzing and performance tooling](docs/fuzzing.md)
- [Package format](docs/package-format.md) and [ROM metadata](docs/rom-metadata.md)
- Per-game ports, controls, validation and limits: see the table above.

## Contributing

Issues and pull requests are welcome. Never attach or commit game media,
files extracted or converted from it, ROMs, or captures; describe problems
with the edition's SHA-256, the command you ran and its output instead.

## License

Director64 is released under the [MIT License](LICENSE). A few files carry
their own MPL-2.0 or LGPL-2.1-or-later headers; they and the third-party
components the browser player ships are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

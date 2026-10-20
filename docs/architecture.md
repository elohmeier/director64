# Architecture

Director64 converts a Director game from its original disc into a game
package plus asset packs, and plays that package on one C runtime. The
runtime runs on Android TV ([android.md](android.md)), the product, and in a
standalone per-game web app ([web.md](web.md)).

| Directory | Responsibility |
| --- | --- |
| `src/director64/` | CLI, game manifests, disc extraction, the conversion driver (`convert.py`), media repair, the Android and web builds, fuzz/parity/journey drivers |
| `compiler/` | The Rust converter `director64-aot`: ISO/ZIP reading, analysis and source audit, score recovery, media conversion, the Lingo compiler, the package writer, per-port corrections (`ports.rs`) |
| `tools/director/` | ProjectorRays dump driver (Node) and the package validator |
| `tools/convert/` | Host tools the converter builds: the WebP packer and the font measurer |
| `runtime/lingo/` | Values, heap, bytecode VM; `family.h` names each Director family's capabilities |
| `runtime/director/` | Score, sprites, inks, film loops, Flash, shapes, events, sound and video services |
| `runtime/interaction/` | Controller input and the per-player pointer |
| `runtime/storage/saves.c` | The game's files and save image, configured by `[player]` |
| `runtime/package/` | Package loader ([package-format.md](package-format.md)) |
| `platforms/web/` | Runtime host, software compositor, the web page, the Node probe |
| `platforms/android/` | Android host (`jni/`), activity and controllers (`java/`) |
| `games/<slug>/` | `game.toml`, source policy, optional runtime code, probe hooks, journeys, tests |

## Conversion

`director64 convert`, `android build` and `web` all convert the same way
(`src/director64/convert.py`) into `build/<slug>/<source>/converted/`:

1. The disc image (cooked ISO, raw 2352-byte rip, or ZIP) is verified
   against its SHA-256 and extracted; the policy's system fonts are
   installed beside it.
2. ProjectorRays parses each Director file into a dump
   (`tools/director/dump.mjs` under Node).
3. `director64-aot` runs every later stage: analysis and source audit,
   score recovery, conversion of bitmaps, sounds, film loops, Flash, shapes,
   text and fonts (including embedded PFR1 fonts), the port's `[convert]`
   corrections, the Lingo compiler and packaging into `game.d64p`.
4. The driver packs assets: images as lossless WebP where smaller
   (`tools/convert/webp_tool.c`, vendored libwebp), every PCM sound as
   Ogg Opus and video as VP9 (FFmpeg). Already compressed MP3 streams retain
   their encoding. Images that compress better with deflate keep that form
   in both APKs and web builds. `tools/convert/font_metrics.c`
   (vendored FreeType) measures fonts where a port needs it.
5. The C package loader validates the package against the runtime ABI.

Opus uses 64 kbit/s VBR for mono and 96 kbit/s VBR for stereo. Converter tags
retain the original sample rate and frame count; players preserve authored
loop and Flash sample bounds when decoding. Short one-shot member cues start
from their beginning after first-use decoding, so codec startup cannot consume
the whole effect. Decoded sounds are cached within each player's memory budget.

Host tools build on first use under `build/tools/`. A conversion is reused
while the converter, runtime ABI, source image and game profile match;
`--force` (or `--reconvert` on `android build`/`web`) converts again.
Recovered Lingo, score records and the source audit stay under
`build/<slug>/<source>/conversion/analysis/`.

### Media repairs

A few discs carry damaged MPEG data that strict decoding rejects. Such an
edition lists a `[[sources.repairs]]` image in `game.toml` with a recipe in
`games/<slug>/host/media-repair.json`. `director64 repair-media --game <slug>
[--replacement FILE]` verifies the original image, applies the recipe's
same-length file changes (a file replaced from an independently recovered
copy, matched by SHA-256, or damaged pictures held and audio silenced), and
writes a separately pinned image plus receipt under `media/`. The original
is never modified; once present, the repaired image is selected
automatically. Reconstructed content is not original content.

## Runtime

Android and the web compile the same sources with the same flags: the
engine, `platforms/web/web_runtime.c`, the software compositor and the
game's own `games/<slug>/runtime/*.c`, for the game's Director family. The
runtime only loads game packages; no game-specific code is generated. The
score runs on a 60 Hz service clock (at most four ticks per pass, twelve
ticks of backlog) and presents a 640×480 stage, or an 800×600 stage
unscaled.

Families (`runtime/lingo/family.h`): D5, D6, extended D6, D7, D8 and D10.
A Director 8.5 game (`director_version = 850`) builds the D10 family plus
`DIRECTOR64_D85`. Handlers compile to resumable bytecode
(`runtime/lingo/lingo_bytecode.h`) bound to the runtime by an ABI digest
over `names.txt` and `lingo_bytecode.h`.

Script errors (type errors, missing handlers, bounds, division by zero)
reproduce the projector's alert-and-continue: the Lingo call is discarded,
the message is recorded (`script_errors`/`last_script_error` in the probe
state) and the score keeps running. Integrity failures halt.

Up to four players each own a cursor. Director has one mouse: a press, or
failing that movement, claims it; the holder keeps it until its click
finishes. Idle cursors hide after five seconds, except the holder's.

### Saves and files

`runtime/storage/saves.c` gives the game's Lingo its files: the saves it may
write and the read-only files an installed game would find beside it. Saves
live in one 128 KiB save image of two 64 KiB generations; each generation is
a `D64S` header (version, generation, size, CRC-32, entry count) and named
entries, and a commit writes the slot not in use, so a failed write keeps
the previous generation. The host stores the whole image after every
commit.

## `game.toml`

Top level: `schema_version`, `slug`, `title`, `status` (`supported`,
`experimental`, `catalogued`), `default_source`, and `[[sources]]` (`id` =
`sha256-` plus the first 12 hex digits, `file`, `sha256`, `bytes`, `kind` =
`iso9660` or `zip`, optional `[[sources.alternates]]` and
`[[sources.repairs]]` with `recipe`).

`[port]`: `director_version` (500, 600, 700, 800, 850, 1000),
`media_root`, `entry_movie`, `launcher_movie` (the embedded projector's
movie), `extended_d6`, `defer_video`, `host_package` (Python package of the
game's `host/` journeys).

`[player]` (generated into `game_profile.h`):

| Key | Meaning |
| --- | --- |
| `saves` | Files the game may write. Patterns: `?` any character, `#` a digit, `*` any run; `a\|b` aliases one file |
| `files` | Read-only installation files, name to contents (`"a\|b"` aliases) |
| `folders` | Directory listings: `[folder, file...]`, `"*"` for any folder |
| `long_date` | Fixed `the long date` |
| `movie_path` | What `the moviePath` answers (D10) |
| `data_folder` | Shipped read-only folder FileIO reads (D10) |
| `key_field` | `{movie, member, frames}`: a field that takes typed keys instead of a text dialog |
| `switches` | `DIRECTOR64_<SWITCH>` defines enabling the game's code in `games/<slug>/runtime/` |

`[convert]` (applied by `compiler/src/ports.rs`; every patch and fix is
pinned to the reviewed source and fails on anything else):

| Key | Meaning |
| --- | --- |
| `streams` | Disc folders played with `playFile`; `NAME=FOLDER` maps a folder into a namespace |
| `data` | Disc folders shipped as read-only data |
| `print` | Folder with print artwork (Löwenzahn 1) |
| `external_media` | `{root, extensions, bind}` with `bind` = `linked-movies` (QuickTime/AIFF, `sounds_movie`), `linked-sounds` or `directmedia` |
| `substitute_font` | `{asset, sizes, names, controller}`: Droid Sans for unembedded system fonts |
| `measure_fonts` | Measure every authored font size with FreeType for text layout |
| `[[convert.patch]]` | `movie`, `member`, `cast`, `handler` (or `all = true`), pinned by `source_sha256` or `calls`; then `body` (Lingo), `parameters`, or `rename_call = [from, to]` with `count` |
| `[[convert.text_fix]]` | `movie`, `member`, `text_sha256`, `trim` trailing characters |
| `[[convert.tempo_fix]]` | `movie`, `frame`, `from`, `to` |

`games/<slug>/host/source-policy.json` holds the audited source decisions:
which Director files belong to the game, aliases, palettes, system-font
resources, `native_stage` (800×600) and pinned `deferred_conversions`.

## Adding a port

Write `game.toml` and the source policy, run `director64 convert` until it
reports no problems, add `[player]` entries for the files the scripts touch
and `[convert]` corrections for source defects, and add runtime code only
where a game needs its own input or output (`switches`). Check it with a
journey, the fuzzer ([fuzzing.md](fuzzing.md)) and a device run. Use
[ScummVM](https://github.com/scummvm/scummvm) as a behavioral reference
only; never copy its code.

## Original-projector reference

`director64 reference --game findus-workshop` captures the workshop's
original projector in an isolated Wine container
(`mise run reference-toolchain` builds it). Options: `--replay`, `--output`,
`--iso`, `--windows-version`. Output goes under
`build/<game>/<source>/reference/`. `tools/reference/` generates synthetic
probe movies for engine experiments.

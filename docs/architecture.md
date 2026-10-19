# Architecture

Director64 converts a Director game from its original disc into a portable
game package plus asset packs, and plays that package on one C runtime. The
runtime runs on Android TV ([android.md](android.md)), its product, and in a
standalone per-game web app ([web.md](web.md)).

| Directory | Responsibility |
| --- | --- |
| `src/director64/` | CLI, game and source manifests, disc extraction, the conversion driver (`convert.py`), media repair, the Android and web builds, fuzz/parity/journey drivers |
| `compiler/` | The Rust converter (`director64-aot`): ISO and ZIP reading, the Director stages (plan, analyze, audit, score recovery, conversion), the Lingo parser and bytecode compiler, the package writer, per-port corrections (`ports.rs`) |
| `tools/convert/` | The converter's C host tools: the lossless WebP image packer, mksprite's build stub |
| `tools/director/` | ProjectorRays dump driver and format, the package validator |
| `tools/projectorrays/` | Pinned versioned Director structural score recovery extension |
| `tools/fonts/` | Isolated host PFR1 recovery and original-outline OpenType export |
| `runtime/lingo/` | Values, globals, handler calls, garbage collection and resumable bytecode execution; `family.h` names each Director family's capabilities |
| `runtime/director/` | Score scheduling, sprites, stage ownership, inks, film loops, Flash, shapes, events and Lingo services |
| `runtime/interaction/` | Controller input and the shared per-player pointer controller |
| `runtime/storage/` | Storage backend callbacks and status types |
| `runtime/package/` | Loader of the game package ([package-format.md](package-format.md)) |
| `runtime/game.h` | Compile-time adapter contract for saves and virtual directories |
| `platforms/web/` | Runtime host, software compositor, image decoding, the web page and the Node probe |
| `platforms/android/` | Android host (`jni/`), activity and controllers (`java/`), resources |
| `games/<slug>/` | Manifest, source policy, browser profile, save adapter, probe state and journeys |
| `tests/` | Source-free shared contracts and synthetic inputs |

The direction of travel is in [roadmap.md](roadmap.md).

## Conversion

`director64 convert`, `director64 android build` and `director64 web` convert
a game the same way (`src/director64/convert.py`) into
`build/<slug>/<source>/converted/`:

1. The disc image (a cooked ISO, a raw 2352-byte rip, or a ZIP) is verified
   against the edition's SHA-256 and extracted into the conversion's work
   directory, where the policy's original system fonts are installed.
2. ProjectorRays parses each Director file once into a dump
   (`tools/director/dump.mjs` under Node, the files spread over parallel
   processes).
3. The Rust converter (`compiler/target/release/director64-aot`) runs every
   later stage over those dumps: analysis and source audit, score recovery,
   conversion of bitmaps, sounds, film loops, Flash and vector shapes, text
   and fonts (including embedded PFR1 fonts), the port's corrections
   (`compiler/src/ports.rs`, with libdragon's mkfont measuring a substitute
   font where a port needs it), the Lingo compiler, and packaging into
   `game.d64p`.
4. The driver packs the assets: images as lossless WebP where that is
   smaller than deflate (`tools/convert/webp_tool.c`), sounds of eight
   seconds or more as Ogg Opus and video as VP9 with FFmpeg.
5. The production C loader validates the package against the runtime ABI.

The host tools build on first use under `build/tools/`. A conversion is
reused while the converter's sources and toolchain, the runtime ABI, the
source image and the game's profile match. The recovered Lingo, score
records and source audit stay under `build/<slug>/<source>/conversion/analysis/`
for inspection.

## Runtime hosts

Android and the web compile the same sources with the same flags: the engine,
the game's adapter (`games/<slug>/runtime/`), `platforms/web/web_runtime.c`
and the software compositor, built for the game's Director family. The web
build adds the Emscripten glue; the Android build adds `platforms/android/jni/`.
Both run the score at a 60 Hz service clock with the same catch-up rule (at
most four ticks per pass, twelve ticks of backlog), and present a 640×480
stage, or an authored 800×600 stage unscaled.

Saves go through the game's adapter (`games/<slug>/runtime/archive.c`) into
a 128 KiB save image: two independently written 64 KiB slots, each with a
header, a generation counter, a CRC and the game's fixed list of save files,
so a commit that is cut off leaves the previous generation intact. Each host
writes the whole image after every commit; the web app's menu exports and
imports it as a file.

## Script failures and coercions

Script-class failures (type errors, missing handlers or properties, list
bounds, division by zero, exhausted call stacks) reproduce the original
projector's alert-and-continue: `dg_service` discards the Lingo call
contexts, records the message (probes expose
`script_errors`/`last_script_error`), and the score keeps running. Integrity
failures (stale heap references, resource exhaustion, invalid transitions)
still halt. Lingo coercions follow the original: numeric text participates in
arithmetic, numbers coerce in string operators, `float` of a point or rect
maps per coordinate, arithmetic maps element-wise over linear and property
lists (property lists keep their keys), and `inside` of a non-rect operand
returns FALSE.

One game adapter is linked into each runtime build. The shared runtime has no
Findus global names or virtual save filenames; its file enumeration service
delegates to the game. The workshop adapter retains its save journal and
migration logic locally.

## Players and cursors

Up to four players each own a cursor, and each draws in its own colour (red,
blue, gold and green by player) once more than one is connected. The colour
tints the glyph's black ink only; its white contrast pixels stay white, so a
cursor still reads against the art under it.

Numeric cursors include Director's standard tool roles: finger 280, open hand
260, closed hand 290, no-drop hand 291, drawing tools, resize arrows and zoom
tools. The shared provider preserves the numeric ID and documented role;
its independently drawn 16-pixel target glyphs are not copies of the engine's
cursor artwork. The 37 tool IDs and hotspots are corroborated by the selected
Birthday 8.5.1 engine's cursor resource directory and the
[Macromedia MX 2004 Scripting Reference, pages 720–722](https://www.manualslib.com/manual/389808/Macromedia-Director-Mx-2004-Director-Scripting.html?page=720).
Unrecognized resource IDs fail with the actual ID in the error. Cast bitmap
and Cursor Xtra artwork continues to come from the user's converted media.

Director has one mouse, so one player drives it at a time and the rest point
alongside. A press claims the mouse; failing that, the player who is actually
moving takes it, which keeps rollover under the cursor that is pointing. The
holder wins every tie and cannot lose the mouse mid-click: a held button or a
queued source event keeps the mouse until that click finishes on both sides. A
single connected controller therefore never hands over. The players who are
not driving still move their own cursor, but only the driver's press becomes a
Director event. The on-screen keyboard and Willy's notices each cover the
whole screen and answer to the player who opened them. A cursor left idle for
five seconds hides, except the holder's.

## Director versions

Structural score recovery supports the verified Director 5, 6, 7, 8, 8.5 and
10 layouts. The runtime is built for a Director 5, 6, 7, 8 or 10 family
selected by the game manifest (`runtime/lingo/family.h`); a Director 8.5 game
(manifest version 850) builds the D10 family, whose 1006-channel score layout
and services it shares, and its package header names the D10 layout.

- **D5** (Löwenzahn 1) keeps direct score/cast script references, versioned
  tempo operands, mouse-event propagation, actorList callbacks and
  cross-movie labels, with 48 sprite channels and four logical sound
  channels. Its linked QuickTime movies (Cinepak video) are decoded by the
  converter and play as digital video on a 600-unit source clock
  ([video.md](video.md)). D5 STXT retains its Mac Roman bytes and style
  metrics; this game explicitly substitutes pinned Droid Sans for
  unavailable system fonts. The runtime supports the one source DIALOG
  window, which suspends the stage and renders a centered movie window, not
  a general desktop window manager.
- **D6** (Workshop) has two sound channels. The **extended D6** profile
  (Willy Werkel) renders puppet wipe and edges-in transition requests as
  timed reveals sourced from the previous frame; the score keeps playing
  during the reveal.
- **D7 and D8** add 800 sprites, twelve external cast libraries, eight sound
  channels, extended score fields and persistent script instances. The
  Christmas calendar selects Director 7 from its recovered media: the D7
  48-byte score, 999 FPS tempo and larger bounds with the shared extended
  services; its clock adapter supplies a fixed Christmas Eve date. D7 bitmap
  conversion also handles two-bit pixels and the modern Windows palette.
  The embedded Mucklas projector is converted as a separate startup movie
  with source provenance; its original embedded font is recovered through a
  pinned, isolated host parser.
- **D8.5** files differ in their text Xtra payload and XMED structure (the
  D10 forms, with the character set read from each document's Paige table),
  zero-padded sound samples, and source quirks the converter repairs and
  records as approximations: palette references stored with castlib 0 or
  stale castlib numbers, film loops nesting loops or carrying comment
  fields, and castlibs sharing a library id and script context.
- **D10** shares D7's score semantics, hover cache and cast-preserving member
  arithmetic, extends the bounds to 1,000 sprites and sixteen linked cast
  files, and adds the bounded controller-window services (resident window
  code, request-based attachment, sprite timeline tells); the window's own
  score tick and panel rendering remain explicit feature work.

Object methods, collection loops, cases, indexed access and chunks lower to
resumable bytecode (`runtime/lingo/lingo_bytecode.h`) that `lv_vm_step` runs
one state at a time; each movie's compiled unit also carries a `.lst` listing
that maps a reported offset back to its statement. Script instances keep
stable member IDs and heap properties across movie changes.

Paige styles retain source metrics, while missing system-font and layout
limits remain explicit in the asset model. FDIA images retain RGB555 plus A8
coverage. The selected static SWF vector-shape conversion is also explicit.

Converted FDI images carry optional conservative coverage bounds in reserved
header bytes: byte 27 is `0xa0 | shift`, and bytes 28–31 are left, top, right
and bottom in units of `2^shift` source pixels. Conversion rounds outward;
four zero coordinates mean empty coverage. Original pixels, dimensions and
registration remain intact, including tiled and indexed images. Older images
fall back to scanning decoded pixels. The compositor uses this hint only when
the selected ink follows source alpha; rectangle inks, palette brushes,
opaque rich text and derived paired-mask coverage retain their own rules.

Additional Director versions, multiple parameterized behaviors, arbitrary
QuickTime codecs, general Flash playback and further Xtras remain feature
work. Director64 is a reusable ahead-of-time porting framework, not a
universal Director emulator.

## Games, sources and adding a port

`GameSpec` validates manifest identities and selects isolated
`media/<game>/<source-id>/`, `build/<game>/<source-id>/` and
`dist/<game>/<source-id>/` trees. Game host code (journeys, reference
runners) is a checkout plugin loaded from `host/__init__.py` under the
package name declared in the manifest.

Original archives are retained byte-for-byte. ISO associated streams are
extracted under `__associated__/`; ordinary data keeps its original paths.
ZIP import keeps its directory roots, verifies CRC while reading, rejects
traversal, symlinks, encryption and duplicate names, and bounds total
expansion. Import does not run projector executables or adopt archived user
progress.

To add a port, create a game manifest and audited source policy
([source-schema.md](source-schema.md)), a browser profile
(`games/<slug>/web.toml`), then implement `runtime/game.h`, any
source-specific conversion decisions and game journeys. Catalogue status is
separate from executable support. Add synthetic contracts for new shared
behavior and check the game with its journeys, the fuzzer
([fuzzing.md](fuzzing.md)) and a device run.

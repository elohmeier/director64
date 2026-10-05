# Browser player

The browser player imports a supported game from the user's own disc image
and plays it, entirely in the browser: milestones W1 to W4 of
[web-roadmap.md](web-roadmap.md), and W6 for all six ports. The site
serves the engine, the converter and the game profiles, and no game data.
Choosing a disc image (or, for Willy Werkel, its ZIP) converts it locally, in
a worker, with the native pipeline's own stages. The result is kept in the
browser's origin-private file system for later visits. A Content Security
Policy confines the page to its own origin.

| Game | Family | Source | Import | Status |
| --- | --- | --- | --- | --- |
| Pettersson und Findus (Workshop) | D6 | `FINDUS.iso` | 6 s | plays |
| Autos bauen mit Willy Werkel | extended D6 | the edition's ZIP | 7 s | plays |
| Findus: Christmas calendar | D7 | `FINDUS3.ISO` | 6 s | plays |
| Findus bei den Mucklas | D8 | `Findus4.iso` | 14 s | plays |
| Lernerfolg Deutsch 1/2 | D10 | `Deutsch12.iso` | 27 s | plays |
| Löwenzahn 1 | D5 | `LOEWENZA.iso` | 14 s | plays, with video and printing |

Import times are headless Chrome on a 16-core machine, with the worker pool
described under [Importing a disc](#importing-a-disc). In one thread the
same imports took 12 s (Workshop), 26 s (Mucklas), 60 s (Lernerfolg) and
41 s (Löwenzahn).

A game is offered once it has a browser profile, `games/<slug>/web.toml`.

## Build and serve

```sh
uv run director64 web --game findus-workshop --serve
```

This writes the source-free site to `build/web/site/` and serves it at
<http://127.0.0.1:18064/> (`--port` changes the port, `--no-build` serves the
existing build). Open it, choose `FINDUS.iso`, and play once the import
finishes: about 6 s in Chrome. The build needs Docker (Emscripten 6.0.10,
pinned by digest in `src/director64/web.py`, run without network), Rust with
the `wasm32-unknown-unknown` target, and Node with the locked dev
dependencies (`npm ci --ignore-scripts`).

When the game has also been converted by the native pipeline
(`director64 assets`), the build writes that package and its asset packs to
`build/<game>/<source>/web/` as well. It checks them against the generated C
and writes the probe harness. The development server offers them as
"Play the local build" under `/local/`, which is not part of the site.
`--check-import` then reruns the browser importer from the local disc under
Node and compares every output with the native pipeline's. `--linked` builds
the W1 runtime with the generated tables linked in, for the probe only.

The page asks for a click before a game starts, because browsers unlock audio
only on a user gesture.

`director64 web --game <slug>` builds that game's runtime and local build;
the site lists every game with a profile and a built runtime.

## Importing a disc

| Stage | What runs | Where it came from |
| --- | --- | --- |
| Verify | Size, then full SHA-256 of the image against the profile's pinned edition, streamed from the `File` in 8 MiB reads | `compiler/src/iso.rs` / `sha2` in `compiler/wasm` |
| Extract | ISO 9660 inventory over bounded reads (`FileReaderSync` in the worker), every file into the in-memory file system | Rust port of `iso.py` |
| Parse | ProjectorRays parses each Director file once; its chunks, records and decompiled scripts become a dump | `director plan` in `compiler/src/convert/plan.rs`, then `tools/director/dump-format.mjs` |
| Analyze, audit | Recovery of scripts, casts and score chunks; the source policy's pinned denominators | `compiler/src/convert/analyze.rs`, `audit.rs` |
| Scores | The structural score decoder | `compiler/src/convert/score.rs` (port of `tools/projectorrays/score_recovery.cpp`) |
| Convert | Bitmaps, sounds, film loops, Flash and vector shapes, text, and the scene model | `compiler/src/convert/compile.rs`, `movie.rs` and the model modules beside them |
| Compile | Lingo to `program.json` | Rust port of `lingo.py` |
| Package | Bytecode and scene tables into a D64P package; images and sounds into asset packs | `compiler/src/package.rs`, `scene.rs` |

Every stage after the parse is the converter's Rust code, the same code
`director64 recover` and `director64 assets` run as `director64-aot director
<stage>` on the host. In the browser it runs as `compiler/wasm`'s
`d64c_director`, which reaches the in-memory file system of `vfs.mjs` through
host imports: the extracted disc stays in JavaScript memory, and a stage
copies in one file at a time. ProjectorRays is the one stage that is not Rust;
its dumps (`compiler/src/convert/dump.rs`) are what the stages read. Embedded
PFR1 fonts recover in the converter too (`compiler/src/convert/pfr.rs`,
`cff.rs`), so games with original fonts convert in the browser as well.
Because it is one codebase, parity is byte-level.

The independent parts of an import run on a pool of workers
(`platforms/web/import/pool.mjs`, one per spare core up to twelve), each
with its own converter and parser instance: parsing each Director file,
converting each movie, the 4/5 stage prescale of each image, and encoding
each sound and each linked movie's video. A pool needs no shared memory, so
the site needs no cross-origin isolation. A movie job converts against
`ProxyFiles` (`jobs.mjs`): what it writes stays on its worker, and it is sent
the files it is known to read (`Setup::job_inputs`); anything else it asks
for, and it runs again with the answers, so a result is only made from
complete inputs. The import worker merges the movies' files and records in
movie order (`compile-merge`) and finishes the stage, so the output is the
one-thread output byte for byte; `--check-import` runs the same pool under
Node with `worker_threads` (`D64_POOL=0` runs it in one thread).
`platforms/web/import/parity.mjs` drives the built bundle from the disc under
Node and compares its outputs with the native pipeline's.

Only a finished import is stored. An entry under
`director64/<slug>/packages/<edition>-<converter revision>/` counts once its
`manifest.json`, written last, matches the file sizes. On load, entries from
another converter revision and directories without a valid manifest are
removed. Removing the converted game never touches the saves under
`director64/<slug>/saves/`. When storage is full, the game plays from memory
and the page says so. A cancelled import terminates the worker and leaves
nothing behind.

## What runs where

| Piece | Responsibility |
| --- | --- |
| `platforms/web/site/` | The page: import flow and cache (`app.js`, `cache.js`), the player (`player.js`: the 60 Hz loop, input, Web Audio through `audio.js`, OPFS saves through `saves.js`) |
| `platforms/web/import/` | The importer: worker entry, pipeline, in-memory file system, the converter's JS wrapper (`convert.mjs`) and the bundle script |
| `compiler/wasm/` | The Rust converter stages as WebAssembly: ISO inventory, SHA-256, the Director stages over the embedder's files, Lingo parser, packager |
| `runtime/package/package.c` | Validates a game package (header, ABI digest, bounds of every string, index, table and bytecode op) and builds the engine's movie tables from it |
| `compiler/src/package.rs`, `scene.rs` | Write the package: the compiler's bytecode units and a Rust port of `director.py`'s plain-D6 scene tables ([package-format.md](package-format.md)) |
| `platforms/web/web_runtime.c` | Engine instance, platform callbacks, save device, the service step and the native probes' line protocol |
| `platforms/web/compositor.c` | Software compositor into a 640×480 RGBA framebuffer: draw order, copy/matte/background-transparent inks, blend, stretching, shapes, text, film loops and the Director cursor |
| `platforms/web/web_main.c`, `library.js` | Emscripten glue: the runtime's imports forward to the embedder's `director64Host` object |
| `platforms/web/node/probe.mjs` | The probe protocol over the same Wasm module, for parity and journeys |
| `games/<slug>/web.toml` | Per-game browser profile (the save archive's file names); not a conversion input, so editing it keeps the asset receipt |

Behaviour follows the console backend (`platforms/n64/director_main.c`)
unless noted:

- **Clock.** Service ticks at 60 Hz from `requestAnimationFrame` time, at most
  four per animation frame, with twelve ticks of backlog; the console's rule.
  Longer stalls drop time rather than rushing the score.
- **Pause.** A hidden page stops game time and suspends audio together; on
  return neither replays the time away. Window blur releases a held button.
- **Input.** The mouse maps through the letterboxed canvas to stage pixels,
  left button as the controller's A and right as B. Pointer capture keeps a
  drag's release. A click shorter than one tick is still delivered for one
  tick.
- **Keyboard.** From D7 on, where scripts receive keyDown and keyUp, every
  key goes to the runtime as a desktop projector delivers it: the Macintosh
  virtual key code of its physical position and the character the layout
  types. Keys go nowhere in D5 and D6, whose scripts declare no key handler.
- **Text entry.** A press on an editable field opens a text box over it, where
  the console opens its on-screen keyboard. The game pauses while the box is
  open, and Enter commits the text through the same `dg_edit_text` as the
  console's Start. The box accepts what that function accepts: 20 characters,
  printable ASCII except the double quote, and German letters on D10. D8
  takes names through keys, so Mucklas types into its field directly.
- **Rendering.** The CPU composites what the RDP draws on the console, at
  8 bits per channel instead of the console's 5. An unchanged stage and
  pointer are not recomposited. A styled text member (D7 and later, extended
  D6) is set in its own font, recovered from the disc or the port's pinned
  substitute. That font is delivered as a pack beside the images and sounds,
  and drawn at the member's size, alignment, ascent and line height.
  Unstyled text uses the console's font: monogram (CC0), libdragon's
  builtin, at the ascent and line height of its atlas.
- **Sound storage.** Imported sounds and WAV speech streams are stored as
  Opus, which the import worker encodes with WebCodecs. The converter first
  resamples them to 48 kHz (`compiler/src/convert/resample.rs`) so they stay
  sample-aligned. Each is decoded on first play (4 ms at the median) and
  cached. A sound still decoding starts at the offset the service clock has
  reached. This makes sound 5 to 9 times smaller than the PCM it replaces:
  Mucklas's is 25 MB instead of 216 MB. Local builds and the Node parity
  importer keep WAV.
- **Sound.** WAV sources (which the console converts to wav64) decode
  synchronously into Web Audio buffers, with the console's loop-bound rule and
  gain from `the soundLevel`. *Channel lifetime* (when a script sees a sound
  as finished) follows the service clock, as it does on the native probe. That
  keeps score waits tick-exact with the probe; the audio plays alongside that
  clock and pauses with it.
- **Saves.** The runtime's save device is the console's 128 KiB FlashRAM
  image, and the game's own archive code commits to it. After each commit the
  page writes the whole image to the origin-private file system at
  `director64/<slug>/saves/flash.bin`. It shows *Saved* only once that write
  has closed. A failed write shows an error and keeps the previous generation.
  Export downloads the image. Import keeps the current save as `flash.bak`
  until the imported one boots; if it fails to load, *Restore previous save*
  puts the old one back. An unreadable save stops the game without changing
  it, as the console does.
- **Failures.** Integrity failures stop the runtime with the console's
  message; recovered script alerts are counted in the footer.

`window.director64.rpc("state")` returns the probe state from the running
page, and `rpc("point N")` a hittable point of sprite N. Both are read-only;
the protocol's `step`/`pad`/`reboot` commands advance the game and are for
debugging only.

## W6 evidence: Willy, Christmas, Mucklas, Lernerfolg, Löwenzahn (2026-09-27)

Each game passes the same four gates as Workshop. Loader parity is run by
every build. Import parity, run with `--check-import`, compares the browser
importer, driven from the disc under Node, with the native pipeline. Fuzzer
parity runs the package build under `director64 parity --probe` against the
native probe. The last check is by hand in Chrome.

| Game | Loader parity | Import parity (Node) | Fuzzer parity | Chrome |
| --- | --- | --- | --- | --- |
| Willy Werkel | 30 movies, 0 differences | program, model, package identical; 1,573/1,573 images, 579/579 sounds; 6.1 s | 16 episodes, 6,929 states, 0 divergences | converts in 6 s; typing a name on the sign draws it in the game's font |
| Christmas | 32 movies, 0 differences | identical; 1,704/1,704 images, 495/495 sounds; 5.2 s | 16 episodes, 6,879 states over 26 movies, 0 divergences | converts in 5 s; the calendar plays |
| Mucklas | 49 movies, 0 differences | identical; 5,284/5,284 images, 1,235/1,235 sounds; 14.5 s, 1.08 GB peak | 16 episodes, 7,059 states over 14 movies, 0 divergences | converts in 15 s; the keyboard types a new player's name |

| Lernerfolg | 77 movies, 0 differences | identical; 22,026/22,026 images, 301/301 sounds, 3,714/3,714 speech streams, 266/266 data files; 46 s, 754 MB peak | 16 episodes, 7,125 states over 13 movies, 0 divergences | converts in 47 s; login, class choice and the castle play with speech |

| Löwenzahn | 23 movies, 0 differences | identical; 1,107/1,107 images, 390/390 sounds, print documents; 6 s (video needs WebCodecs, absent in Node) | 16 episodes, 6,584 states over 19 movies, 0 divergences | converts in 38 s including video; the intro video plays; a print stamp opens the print dialog |

Löwenzahn's movies are QuickTime with Cinepak video. The Rust converter
decodes them, and its sound is sample-identical to ffmpeg's. The import
worker encodes the video to VP8 with WebCodecs, and the page decodes it
again with WebCodecs at the runtime's video time. Printing uses the
browser's print dialog: the page lays out the chapter's recovered text and
original artwork on an A4 page (`print.html`, after the console's
`print.typ`) and prints it. It then dismisses the print pause as the
console's B button does.

Lernerfolg needed the most, and all of it holds for any later port:
- **Stage prescale.** D10's authored 800x600 stage draws at the console's
  4/5 prescale: images at the stored size, geometry, quads, hit tests and
  font sizes scaled, and the page's pointer scaled back into authored
  space.
- **Masks and Flash.** Mask ink takes the `_mask` partner, by the
  console's luminance rule. Flash prompt text is drawn over its card.
- **Speech streams.** `sound playFile` speech arrives as a streams pack.
  AIFF is rewritten as WAV, and WAV and MP3 are kept, since the browser
  decodes both. Each stream records its duration, and the runtime holds
  the channel busy that long.
- **Data files.** The exercise databases ship as a data pack.
- **Deflated images.** Converted images are stored deflated from the
  moment the convert stage writes them (`D64Z`), and the page inflates
  one when the runtime loads it. Lernerfolg's 1.7 GB of converted images
  would otherwise have held the import at 2.3 GB.

Each port's host corrections run in Rust as well (`compiler/src/ports.rs`):
Willy's, Mucklas's and Lernerfolg's compatibility fixes, Willy's and
Christmas's system-font substitute, and Lernerfolg's measured metrics. The substitute is measured by libdragon's own mkfont,
built as WebAssembly. A game's probe step and state line live in
`games/<slug>/probe/*.inc`, included unchanged by the native probe and the
browser runtime, so both report one schema.

Loader parity also checks two properties the runtime depends on:
- **Behavior-block sharing.** The director tells a continuing sprite span
  from a new one by the address of its behavior block. The loader therefore
  shares a movie's identical blocks, as the generated C does. Without that,
  every re-written channel restarted its sprite.
- **Name and double pools.** Parity compares their contents.

## Import evidence (W3/W4, 2026-09-27)

| Check | Result |
| --- | --- |
| Rust Lingo parser against `lingo.py` | Byte-identical `program.json` for all six recovered corpora; identical output or failure on synthetic, non-ASCII and property-based inputs (`tests/test_lingo_rust.py`) |
| Rust ISO reader against `iso.py` | Identical inventories and file digests on all six local discs (32 to 4,160 files) |
| Score decoder as WebAssembly against the native `score-probe` | All 563 score chunks of the six corpora byte-identical |
| Rust converter stages against the JavaScript stages they replace | Analysis, source audit, score recovery and conversion (`model.json`, `font-audit.json`, images, sounds, alpha planes, fonts, film-loop evidence) byte-identical for all six corpora |
| Refactored JS stages, run through their CLIs | Workshop's Lingo, source audit, score recovery, `model.json`, 3,976 images and 783 sounds byte-identical to the existing build |
| Browser importer from the ISO, under Node (`--check-import`) | `program.json`, `model.json`, the game package, all 3,976 images and 783 sounds byte-identical to the native pipeline; 9.6 s; the in-memory file system peaks at 342 MB |
| Chrome, fresh profile: choose the ISO, play | Import in 9 s. The Vemory journey (hub, 10 pairs, feather, return) saves, and after a page reload the chest shows the feather |
| Warm reopen | The page offers the converted game; play starts in about a second without importing |
| Wrong discs | Another game's ISO is refused by size; a same-size copy of the Workshop ISO with one byte changed is refused by its SHA-256 |
| Cancel mid-import | Nothing stored; the next visit asks for the disc |
| Converter update | Entries of another converter revision and an interrupted import are pruned on load; saves untouched |
| No upload | 219 requests over the test session, all GETs of site files; the page runs under `connect-src 'self'` without violations |
| Browser importer on the Rust stages, under Node (`--check-import`) | Same byte-identical outputs; 6.1 s; the in-memory file system peaks at 476 MB, the parser's dumps included |
| Chrome, fresh profile, Rust stages | Import in 6 s; the game plays |
| Cache commit, pruning and failed writes (`tests/node/web-import.test.mjs`) | Round trip, manifest-last commit, truncation detection, quota failure leaves no entry; the in-memory file system keeps a disk's semantics |

Not yet shown: quota exhaustion in a real browser (unit-tested only), save
export and import in the browser (the W1 unit tests cover the store), memory
on lower-end machines (the import holds the extracted disc in memory), and a
deployed Pages URL (W5).

## Package evidence (W2, 2026-09-27)

| Check | Result |
| --- | --- |
| Loader parity: `tests/package/compare.c` loads Workshop's package and compares every field of every table with the generated C (also under ASan/UBSan) | 38 movies, 0 differences; the build runs it every time |
| Refactored compiler still writes the console's C | Byte-identical units for all six ports |
| Package runtime against the native probe: fuzzer parity and every START journey | 6,859 states, 0 divergences; 33 of 33 journeys pass with identical final states and commands |
| Malformed and incompatible packages (`tests/native/test_package.c`) | Wrong ABI, profile, format and magic are rejected with a reason. Every truncation is rejected with nothing left allocated. None of 4,287 single-bit corruptions crashes under ASan: 712 are rejected and the rest load into tables that stay in bounds |
| Browser | Workshop plays from the 1.39 MB package; the runtime module is the same for any Workshop package |

## W1 evidence (2026-09-27)

Runtime revision: the commit that adds this document. Source:
`sha256-fc24ffe284ec`. Browser: headless Chromium through agent-browser.

| Check | Result |
| --- | --- |
| Native/Wasm state parity: `director64 parity --against native/director-probe --probe web/director-web-probe --episodes 16 --actions 200` | 16 episodes, 6,859 states compared, 0 divergences (6 movies visited) |
| Workshop journeys from START on the Wasm runtime (`full_probe.py <mode> --executable web/director-web-probe`): routes, hubs (incl. 12 constructions), treasure, exit, garden, profiles | All 33 scenarios pass; every final state and command stream identical to the native probe's |
| W1 journey in the browser with real mouse input: boot, choose a profile, reach the hub, complete Vemory (10 pairs), return to the hub, reload the page, reach the chest | Passed: save generation 1 written and shown saved, the reloaded page finds the save, and the chest shows the awarded feather for that profile |
| Focus/pause: page hidden for 3 s, then shown | Ticks frozen while hidden; resumed at 60/s with no replayed backlog |
| Compositor contracts (`tests/native/test_web_compositor.c`, also under ASan/UBSan) | Inks, tiled and FDIA decoding, blend, stretch, shapes, text, pointer, hit test, cache, missing-asset failure |
| Page helpers (`tests/node/web-player.test.mjs`) | WAV decoding, loop bounds, save commit ordering, failed writes, import backup/restore, no-storage reporting |

Baseline performance:

| Measurement | Value |
| --- | --- |
| `profiles` journey, 79,920 service ticks, native probe | 0.34 s |
| Same journey, Wasm under Node (includes module start) | 0.57 s |
| Browser delivery over the whole session above | 60 ticks/s, 0.0 s dropped |
| Full composite (KISTA, median / p95 of 60) | 0.80 / 1.10 ms |

Not yet shown: visual comparison against console or original-projector
frames, and audio checkpoints (the headless session plays audio but nothing
listened to it). Not measured yet: peak memory and scene-entry stall
distribution. Of the W1 gates, these remain open.

# Architecture

| Directory | Responsibility |
| --- | --- |
| `src/director64/` | Game/source manifests, CLI, Lingo parser and AOT compiler, metadata generation, import, asset packing, toolchain and evidence utilities |
| `compiler/` | Rust bytecode compiler stage (`director64-aot`): program.json to bytecode units, listings and the per-movie handler index; the pipeline's only generator (`src/director64/aot.py` runs it) |
| `tools/director/` | Director recovery models, bitmap decoding and conversion; source-specific assumptions supplied as policy |
| `tools/projectorrays/` | Pinned versioned Director structural score recovery extension |
| `tools/fonts/` | Isolated host PFR1 recovery and original-outline OpenType export |
| `runtime/lingo/` | Values, globals, handler calls, garbage collection and resumable execution |
| `runtime/director/` | Score scheduling, sprites, stage ownership, shapes, events and Lingo services |
| `runtime/interaction/` | Controller input and the shared per-port pointer controller |
| `runtime/storage/` | Storage backend callbacks and status types |
| `runtime/game.h` | Compile-time adapter contract for saves and virtual directories |
| `platforms/n64/` | Display, audio, image cache, DSO loading, FlashRAM I/O and main loop |
| `platforms/web/` | Browser player: software compositor, runtime host, Wasm glue, the page, the in-browser importer and the Node probe harness ([web.md](web.md)) |
| `runtime/package/` | Loader of the portable game package ([package-format.md](package-format.md)) |
| `compiler/wasm/` | The Rust converter stages (ISO, Lingo parser, packager) as WebAssembly |
| `games/<slug>/` | Manifest, source policy, save codec, journeys and qualification |
| `tests/` | Source-free shared contracts and synthetic inputs |

The direction of travel is in [roadmap.md](roadmap.md): the converter takes
over the semantic binding the runtime does by name today.
The planned browser backend and local ISO pipeline are in
[web-roadmap.md](web-roadmap.md), with a portable game package shared by the
native and browser conversion paths and a static GitHub Pages deployment.

Script-class failures — type errors, missing handlers or properties, list
bounds, division by zero, exhausted call stacks — reproduce the original
projector's alert-and-continue: `dg_service` discards the Lingo call contexts,
records the message (probes expose `script_errors`/`last_script_error`), and
the score keeps running. Integrity failures (stale heap references, resource
exhaustion, invalid transitions) still halt. Lingo coercions follow the
original: numeric text participates in arithmetic, numbers coerce in string
operators, `float` of a point or rect maps per coordinate, arithmetic maps
element-wise over linear and property lists (property lists keep their keys),
and `inside` of a non-rect operand returns FALSE.

One game adapter is linked into each ROM. The shared runtime has no Findus global
names or virtual save filenames; its file enumeration service delegates to the
game. The workshop adapter retains its proven save journal and migration logic
locally; it is not a compatibility layer for the old project interfaces.

## Players and cursors

Every console port carries a player, up to the `controller_count` its source
policy declares; the build passes that count to both the ROM header and the
runtime, so the ports the menu advertises are the ports the game reads. Each
player owns a cursor, and each draws in its own colour — red, blue, gold and
green by port. The colour tints the glyph's black ink only; its white
contrast pixels stay white, so a cursor still reads against the art under it.

Director has one mouse, so one player drives it at a time and the rest point
alongside. A press claims the mouse; failing that, the player who is actually
moving takes it, which keeps rollover under the cursor that is pointing. The
holder wins every tie and cannot lose the mouse mid-click: a held button or a
queued source event keeps the mouse until that click finishes on both sides. A
single connected controller therefore never hands over and behaves exactly as
it did before. The players who are not driving still move their own cursor, but
only the driver's press becomes a Director event. The on-screen keyboard, print
dialogue and Willy's notices each cover the whole screen and answer to the
player who opened them.

The N64 backend uses resumable Lingo state machines, a 3 MiB image cache and a
640×480 stage. The Director 6 profile retains the workshop's four-overlay limit
and two audio channels; Director 7 and 8 permit thirteen overlays and eight channels.
The extended D6 profile renders puppet wipe and edges-in transition requests as
timed reveals sourced from the previously queued framebuffer; the score keeps
playing during the reveal.
Director 5 uses 48 sprite channels, four overlays, four logical audio channels,
and two bounded H.264/ULC video slots. Its source dialog can suspend the stage
and render a centered movie window while keeping its overlays alive.
A game needing different stage dimensions or further external libraries requires
extensions.

`GameSpec` validates manifest identities and selects isolated
`media/<game>/<source-id>/`, `build/<game>/<source-id>/` and
`dist/<game>/<source-id>/` trees. Game host code is a checkout plugin loaded from
`host/__init__.py` under the package name declared in the manifest. The Python
wheel contains shared tools; complete builds run from this repository checkout.

Original archives are retained byte-for-byte. ISO associated streams are extracted
under `__associated__/`; ordinary data keeps its original paths. ZIP import keeps
its directory roots, verifies CRC while reading, rejects traversal, symlinks,
encryption and duplicate names, and bounds total expansion. Import does not run
projector executables or adopt archived user progress.

To add a port, create a game manifest and audited source policy, then implement
`runtime/game.h`, source-specific recovery decisions and game journeys. Catalogue
status is separate from executable support. Add synthetic contracts for new shared
behavior and qualify the game through native journeys and target captures.

Structural score recovery supports the verified Director 5, 6, 7, 8 and 10 layouts.
The native runtime uses a Director 5, 6, 7, 8 or 10 build profile selected by the
game manifest. D7 and D8 add 800 sprites, twelve external cast libraries, eight sound
channels, extended score fields, and persistent script instances. The D10 profile
shares D7's score semantics, hover cache and cast-preserving member arithmetic,
extends the bounds to 1,000 sprites and sixteen linked cast files, and adds the
bounded controller-window services (resident window code, request-based
overlay attachment, sprite timeline tells); the window's own score tick,
panel rendering and boot qualification remain explicit feature work. Object methods,
collection loops, cases, indexed access, and chunks lower to resumable bytecode
(`runtime/lingo/lingo_bytecode.h`) that `lv_vm_step` runs one state at a time; each
movie's generated unit also carries a `.lst` listing that maps a reported offset back
to its statement.
Script instances keep stable member IDs and heap properties across overlay reloads;
no pointer into an unloaded native overlay remains in their property storage.
The embedded Mucklas projector is compiled as a separate startup unit with source
provenance. Its original embedded font is recovered through a pinned, isolated
host parser; the ROM contains only converted font data and the libdragon renderer.
Paige styles retain source metrics, while missing system-font and layout limits
remain explicit in the asset model. FDIA images retain RGB555 plus A8 coverage
in paired textures within the existing bounded image cache. The selected static
SWF vector-shape conversion is also explicit.

The Löwenzahn D5 profile keeps direct score/cast script references, versioned
tempo operands, mouse-event propagation, actorList callbacks, and cross-movie
labels. Its selected host postprocessor converts all linked QuickTime media
to H.264 and ULC, with a 600-unit source playback clock. D5 STXT retains its
Mac Roman bytes and style metrics; this game explicitly substitutes pinned
Droid Sans for unavailable system fonts. The target supports the one source
DIALOG window, not a general desktop window manager. Additional Director
versions, multiple parameterized behaviors, arbitrary QuickTime codecs,
general Flash playback and further Xtras remain feature work.
Director64 is a reusable AOT porting framework, not a universal Director emulator.

libdragon remains the clean pinned submodule. The upstream fork branch still has
its original name: it is dependency provenance, not an old Director64 interface.
Release and probe builds use the same DSO input ELF paths so their DragonFS payloads
can be compared exactly. Emulator evidence is distinct from hardware validation.

The Christmas calendar selects Director 7 from its recovered media. It combines
the D7 48-byte score, 999 FPS tempo and larger runtime bounds with the shared
extended behavior services. Its clock adapter supplies a fixed Christmas Eve
date. D7 bitmap conversion also handles two-bit pixels and the modern Windows
palette. Source policy pins the multi-archive launcher selection and records
external-copy differences and visual substitutions.

# Lernerfolg Grundschule Deutsch 1-2

Experimental Director 10 (MX 2004) port. The full authored chain plays:
launcher, controller window, environment check, Tivola intro, transition,
login with name entry, class pick, the castle map and its rooms, and the
exercises, with prompt cards rendered from their Flash fields and the answers'
feedback states. Umlauts survive in runtime strings ("Wie klingen Wörter?").
Legacy STXT field members — the login name among them — bind their authored
typography through the movie's `Fmap` font map (font, 24 pt, centered); 739 of
871 text members carry a bound style, and the remainder (Arial and other system
fonts not embedded in the media) are recorded as `unavailable-source-font`
approximations.

```sh
uv run --locked director64 android build --game lernerfolg-deutsch-1-2
uv run --locked director64 android install --game lernerfolg-deutsch-1-2 --serial <device>
uv run --locked director64 web --game lernerfolg-deutsch-1-2 --serve
uv run --locked director64 journey --game lernerfolg-deutsch-1-2
```

The scripted journey (`host/full_journey.py`, over the web build's probe)
creates a profile on the login screen, types the name through the keyboard
commit path, confirms, picks a class, skips the map demo, enters a room and
plays a spelling task (prompt cards rendered, an answer picked, the `Selected`
state read back) — ending with zero script alerts and the profile persisted.

## Presentation and input

The Android TV and web players draw the authored 800x600 stage unscaled (the
native stage, see [docs/web.md](../../docs/web.md)). Mask ink renders from the
authored `<name>_mask` member that follows each masked image, baked into the
image's alpha at load; mask ink over any other neighbour copies, as the original
does with an unsuitable mask member (the class panel's full-stage base is
authored that way). Recorded approximation: the deferred Flash intro members
draw nothing per their policy pins.

Clicking an editable field (the login name line, write-in exercise boxes) opens
text entry — the TV keyboard on Android, a text box with an on-screen keyboard
for controllers in the browser — which types windows-1252 characters, including
Ä Ö Ü ß, one keyDown at a time through the same replacement machinery desktop
typing used, so the authored Enter/backspace/character filters all run. Up to
four controllers point with their own coloured cursors
([shared pointer](../../docs/architecture.md#players-and-cursors)).

Speech and effects stream case-insensitively: the converted `voc/` and `sfx/`
set canonicalizes stems to lowercase, as the authored Windows paths were
resolved (`sfx\silence.aif` names the on-disk `Silence.aif`). The disc stores
853 of the `.aif` files as MP3 (one with an ID3 tag) and three as WAV;
conversion selects the codec by magic bytes, not extension.

## Exercises

The exercise task databases ship verbatim under `tests_db/` and are read through
the Buddy API `baReadBinFile`, which answers the file's bytes as the list the
authored parser walks; the shipped path is resolved from the authored Windows
path by its folder tail, and the bytes are stored one per element rather than a
full value each. A sprite answers `the castLibNum` of the member it shows and
`the frame` of its Flash timeline, a sound channel answers `the endTime` of what
is playing, `the keyboardFocusSprite` follows the field an exercise activates,
`bitOr` and its siblings compute the UTF-8 encoding the exercises use, and
`debugAlert` traces instead of failing. Settings the conversion already baked —
Flash playback flags, vector outlines, per-character colour and spacing — are
accepted and dropped rather than stopping the movie.

The exercise prompts' text lives in named edit fields inside Flash card assets,
which the conversion flattens to film timelines while recording the dynamic
surface: the PlaceObject2 instance names (`my_txt`, `my2_txt`, `Selected`…),
each field's bounds, font, size, color, alignment, wrap and margins, and the
frame-label table (`normal`/`red`/`green`/…). A script addressing
`sprite(n).my_txt` gets a property bag seeded from that field's descriptor
(`FLASH_FIELD_OBJECT`); `setText` and `setVariable("text1", …)` decode the
authored percent-escapes (the recovered `int2hex` path) into UTF-8 — the card
SWFs' own DoInitAction literal pools show `setText` passing its argument through
`unescape`, which pins that decode — lay the text out with the font's measured
advances, and answer `textHeight` so the authored vertical centering runs;
`getTextFormat`/`setTextFormat` carry the whole-field size and color (a
character-range format renders in the base style, traced
`FLASH_FORMAT_RANGE_PENDING`). The platform draws the wrapped lines above the
flattened frame in the sprite's own draw order. `goToFrame("red")` and
`findLabel` resolve through the recorded label table onto the flattened poses,
so the cards' feedback states switch as authored. A named non-field child still
answers a geometry-seeded bag (`FLASH_OBJECT_PENDING`) — its visibility toggles
read back and the label frames carry the authored art for the states the
exercises use. `setFlashProperty`/`getFlashProperty` address those bags; only a
field's `_visible` changes what renders.

Text metrics are measured, not guessed: the converter measures every authored
(font, size) variant with the pinned `mkfont` and records advance tables
covering codepoints 32–255, so `charPosOf` highlight boxes and the prompt
overlays lay out with the same glyph widths the renderer draws. A member whose
style still has no table keeps the half-em fallback, traced
`TEXT_METRICS_PENDING`.

The two MX 2004 JavaScript-dialect handlers the pinned decompiler reports as
`unk26` are recovered from their compiled literal pools and lowered by a
source-pinned compatibility transform: `int2hex` is
`function int2hex(num) { return num.toString(16) }` (the exercises'
percent-encoder) and `clearGarbage` is `function clearGarbage() { _system.gc() }`.

The Lingo value heap has a fixed budget (384 KiB for this family); live data
after a collection is about 128 KiB of it. D10 collects at half the heap, and a
list that cannot double near the ceiling grows exactly instead of failing. When
a single interpreter step still outgrows the remainder, an emergency pass frees
garbage from earlier steps in place: nothing moves and everything born during
the current step survives. The behavior lifecycle events (`new`, `beginSprite`)
dispatch through a collector-visible runner instead of one atomic span, because
a thousand-channel begin fan-out otherwise fills the heap with garbage no pass
may touch. One known gap from fuzzing: `TASKSCRIPTS.CXT initFields` in
`TEST2CB1.DXR` asks for character metrics of a font the recorded metrics table
does not carry yet ("text character metrics: unavailable").

Saves follow the authored mechanism: the "save" cast is a mutable text-member
archive whose `castLib("save").fileName` retargets between the shared "save"
file and per-player "save01".."save08" files; `save()` serializes the records
into the two-generation save image, `fx_FileDelete` removes a profile as a
zero-length write, and `the timeoutList`, `the globals` and `the result` behave
as authored so the movie-transition cleanup sweeps run unchanged.

## The controller window

The original projector opens `control.dir` as one invisible movie-in-a-window
("control"), navigates it between controller movies, and stage code calls the
window movie's interface handlers whose instances live in shared globals; the
window also opens as a visible statistics panel. The port models this as a
bounded controller-window service, not a desktop window manager:

- Every recovered `tell` form lowers: `tell the stage` inline, `tell window(…)`
  through window services, and sprite tells (references or sprite values) onto
  the flattened film timelines (`sprite_frame`, `sprite_lastframe`,
  `sprite_go`, with handler sends through `sendSprite`).
- The window movie's code attaches as a resident shared movie
  (`dg_window_attach`) so its handlers resolve from stage code across every
  movie change; `fileName`/`gotoMovie` requests record the movie stem for the
  platform to resolve. `visible`, `rect`, `open`, `forget`, `moveToFront` and
  `windowPresent` are served with the authored semantics.
- The platform keeps the window movies, their cast archives and any
  `castLib fileName` swap target resident across stage transitions, and
  excludes that resident set from other movies' event broadcasts. Explicitly
  pending, traced at runtime: the window's own score tick and rendering the
  visible panel (`WINDOW_MOVIE_SCORE_PENDING`, `WINDOW_PANEL_PENDING_RENDER`) —
  the core play loop never shows the panel.
- `put … into member(…)` assigns member text, and `delete <variable>` disposes
  by assigning VOID.
- Saves: the save image holds the six authored preference/progress files plus
  eight numbered player profiles; the read-only `tests_db` exercise databases
  ship as data.

## Source

The manifest pins ISO SHA-256
`4c87e10dcc99dfb68882b51c9daceb7ce4761b482a2a43bf620093289353e0f4`.

The image is a local rip of the pressed CD, cut to the declared ISO 9660 volume
(265,350 sectors). The disc is an ISO 9660 / UDF 1.02 bridge whose two views
expose the same 4,160 files and 529,164,042 bytes; the importer reads the
ISO 9660 side, which retains the original mixed-case names. Identification comes
from `install.exe` (`Lernerfolg Grundschule Deutsch 1-2`, `Tivola Publishing
GmbH, Hamburg`), the `version` file (`LernerfolgGerman 12 v2013`) and
`hilfe/Hilfe.htm`. `Deutsch.ini` carries a stale `Project Name = Mathe Premium 1`
from a sibling title and was not used for identification. The local ScummVM
checkout has no detection entry for this game.

Disc-level inspection limits are recorded in
[media-inventory.json](../../docs/media-inventory.json): sectors at and beyond
297,144 are unreadable on the local drive, and `Tivola.dat` returns a short read
through the UDF mount even though its ISO 9660 extent re-reads from the disc and
matches the image exactly.

## Contents

| Directory | Files | Content |
| --- | ---: | --- |
| `global/` | 9 | `control.dxr` startup movie, `errmsg.dxr`, shared casts, `save.cst` |
| `data/` | 67 | 43 movies and 24 casts: rooms, games, tests, demos, toolbar |
| `voc/` | 3,660 | AIFF speech |
| `sfx/` | 54 | 51 AIFF plus 3 WAV effects and music |
| `tests_db/` | 266 | `*.test.db` exercise records in 39 directories |
| `hilfe/` | 64 | HTML help |
| `xtras/` | 26 | Windows `.x32` Xtras |

`autorun.exe` launches `Deutsch.dat`, a Director MX 2004 projector whose embedded
movie references `global:control.dir`; `proj.dll`, `dirapi.dll` and `iml32.dll`
are its runtime. `Tivola.dat` is installer payload, not Director media.

The 76 Director files hold 165 cast libraries, 11,656 cast members, 631 script
blocks and 4,542 bytecode handlers. The pinned ProjectorRays 1.1.1 parser reads
all of them and decompiles their Lingo.

## Recovery

Every file reports Director version 1000 (MX 2004). `recover` audits all 76
Director files plus the `Deutsch.dat` projector against the pinned source
policy, decodes every versioned score, and verifies the 4,542 parsed handlers
against their bytecode identities.

D10 recovery findings, from the audited corpus:

- Every `VWSC` and film-loop `SCVW` uses the verified D7/D8 layout: the
  `0xfffffffd` indexed envelope, format 13, 48-byte channel records and 1,006
  allocated channels. `VWLB` and `VWFI` parse with the version-independent
  decoders. The 138 audited score resources recover 18,113 frames, 783 labels
  and 69,062 deltas with no unindexed tail bytes.
- The D10 script context chunk is spelled `LctX`; the source model accepts both
  spellings. Internal cast-library IDs are wide (for example `0x10400`), and
  all external `MCsL` entries share ID 1024.
- The 518 `SCRF` chunks are not routed anywhere; sampled chunks show a 20-byte
  header plus 6-byte entries whose semantics remain unverified.
- Two handlers (`int2hex`, `clearGarbage`) contain bytecode the pinned
  decompiler cannot translate (opcode `unk26`); both have live call sites (see
  above). There are 68 `tell-window-context` sites: the projector drives
  `control.dxr` as a movie-in-a-window.
- Tempo commands use opcode 246 only, with rates 8–30.

The movies request Text Asset, TextXtra, Font Asset, Mix Services and Script
Agent; the disc additionally ships Flash Asset, QT6Asset, ActiveX,
INetURL/NetLingo/NetFile, ShockTalk, Sound Control, FileIO/FileXtra4/BudFile,
PNG Import Export, SharpExport, PMatic, Cursor Asset, DirectSound and the SWA
streaming pair.

## Asset conversion

The shared converter compiles the corpus with 14 explicit residual problems;
`model.json` records every unconverted member. Converted:

- All 9,290 bitmaps, dominated by 8,916 32-bit members. The corpus stores each
  authored alpha channel twice: inline in the `BITD` plane and as a packed
  even-width-row `ALFA` plane. Conversion verifies the planes agree
  (`alfaVerified`) before writing the A8 asset; four `ALFA` planes attach to
  bitmaps without the FollowAlpha flag, are ignored by Director at render
  time, and are retained as `unusedAlfa` evidence.
- All 714 text Xtra members. D10 keeps the D8 Paige record encoding inside a
  432-byte cast payload, splits long text into continuation records whose
  count field is the cumulative character offset, closes documents with an
  `FFFE` marker, and may append an `FFFE`-headed binary layout section
  (`TXcl`), retained undecoded in the evidence. Text decodes as windows-1252;
  the corpus umlauts are wrong under D8's Mac Roman.
- All 308 embedded sounds. 131 D10 sounds retain their imported file's blanked
  458-byte header as a leading zero prefix before the declared sample frames;
  conversion requires provable silence and trims it. Six sound members have no
  media and convert as empty.
- All five embedded PFR1 fonts. Two of them (`Grund5`,
  `GrundSchulGroteskBQ-RNew`) come from the MX 2004 composer, which writes a
  positive logical Y matrix around identically oriented physical contours;
  the OpenType export accepts both signs. The converter decodes PFR1 itself
  (`compiler/src/convert/pfr.rs`), implemented from the original Bitstream
  TrueDoc Character Shape Player in the disc's own `Font Xtra.x32`: an
  orus-table lookup with direction 0 — the default for a curve command's
  unencoded control coordinates — infers its search direction from the travel
  of the current point relative to the previous one instead of returning the
  coordinate unchanged, and the y-control table is preprocessed before any
  command runs (glyph flag bit 2 duplicates its first entry, and an odd-length
  table is padded with its last). Without those, `e`, `s`, `3`, `8`, `S` and
  several counters came out as filled blobs. Emulating the player's outline
  loop (`tools/fonts/pfr1-verify.py`, see
  [tools/fonts/README.md](../../tools/fonts/README.md)) over every simple glyph
  of all six recovered fonts across the ports gives 688 of 688 identical.
- All 42 vectorShape members and 104 of the 111 Flash members, through the
  extended SWF rasterizer: multiple solid and gradient fills under per-style
  nonzero winding, solid line styles (including LineStyle2 cap/join/miter
  profiles) with minimum one-pixel stroke coverage, deterministic
  quadratic-curve flattening, partial-alpha fills, style replacement, nested
  sprite timelines, color transforms, clip-depth masking, embedded SWF font
  glyphs (`DefineFont` 1–3) with static `DefineText` and initial edit-text
  rendering through the corpus HTML subset, and full `PlaceObject2`/`3`
  timelines. Multi-frame timelines (85) flatten to per-frame assets played
  through the film mechanism (`flash-timeline-flattened`). The dynamic surface
  is recorded, never guessed: variable-bound edit texts become `flashFields`
  descriptors that the runtime renders as prompt overlays
  (`flash-dynamic-text`), frame labels and top-level `stop()` frames persist as
  `flashTimeline`, named non-field children persist as `flashNamed`, and every
  other ActionScript sequence, filter list, blend mode and clip action is
  retained as classified evidence (`flash-actions-ignored`,
  `flash-effects-ignored`). SWF 6+ strings decode as UTF-8.
- 354 of 357 film loops flatten offline. The extended flattener draws
  unfilled shape children with the runtime's exact span geometry, stencils
  mask ink through the neighbouring mask member (the corpus masks are 8-bit
  with anti-aliased edges; grays threshold at half luminance under a recorded
  `film-loop-mask-antialiased` limit), captures D6+ sound-channel references
  (recorded as `film-loop-sounds-pending-native`), resolves external-cast
  children, and treats behaviors on children as inert render-only channels per
  the reference. Cast-library-zero and stale absent-member references draw
  nothing, as in the reference, each recorded per channel.

The 14 residual conversions — 7 long full-stage timelines (the Game 2 track
displays at 133–155 frames of 800×600; the 128-frame flatten bound is a
deliberate budget limit), 2 Flash members with streaming sound, 1 with a button,
3 film loops (a text child, a nested loop child and an unconverted Xtra child),
and 1 cursor Xtra member — are pinned per member as `deferred_conversions` in
the source policy with their dispositions. Each becomes an explicit
`deferred-conversion` approximation; a pin without a matching residual fails
the conversion as stale.

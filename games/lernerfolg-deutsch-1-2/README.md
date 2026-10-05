# Lernerfolg Grundschule Deutsch 1-2

Experimental Director 10 (MX 2004) port. Recovery, asset conversion, ROM
packing, the port scaffolding and the boot milestone are complete; the ROM
builds and validates against the 56 MiB ceiling, and a validated Gopher64
capture boots the full authored chain — launcher, controller window,
environment check, Tivola intro, transition, login, class pick — and
visits a castle room and enters a spelling task. The exercises are
playable: the sanitized scripted journey plays the task end to end
(prompt cards rendered from their Flash fields, an answer picked, the
Selected state read back) on the host probe, and the console capture
follows it into TEST2A and picks the first answer card. The compact
interned-op code generator (`src/director64/aot.py`) shrank the task's
overlay set by ~56–59% — `scripts_cxt.dso` 1,087,091 → 443,891 bytes,
the always-resident `control_dxr.dso` 681,328 → 288,247 — closing the
measured ~1.1 MiB arena deficit with margin. The exercise screen now
renders faithfully on console: script-addressable Flash edit fields no
longer bake their authored placeholder into the flattened frame (the
overlay draws the live or initial text instead), rotated sprites
prescale their quads onto the 640x480 screen like everything else, and
the rdpq draw boundary transcodes windows-1252 runtime strings to
UTF-8, so umlauts survive ("Wie klingen Wörter?"). The PFR font
recovery gap is closed: the glyph-program decoder's curve-command
semantics were corrected against the original Bitstream player
extracted from the disc's own Font Xtra (see "Original fonts" below),
so `e`, `s`, `3`, `8`, `S` and the other formerly blob-rendered glyphs
now decode with exactly the original outlines.

**The ROM boots and plays on real hardware** (SummerCart64, Expansion
Pak): the login keyboard, profile creation and room navigation work on
the console. Legacy STXT field members — the login name among them —
now bind their authored typography through the movie's `Fmap` font map
(font, 24 pt, centered), so the typed name renders in the school font
instead of the 8 px debug fallback; 739 of 871 text members carry a
bound style, and the remainder (Arial and other system fonts not
embedded in the media) are recorded as `unavailable-source-font`
approximations. The remaining gap is performance, and two optimization
rounds have closed most of it. The first cached handler resolution (the
profile's dominant cost) and cut the 220 s showcase from 1,348 timing
overruns and 21.7 s of lost game time to 198 and 15.2 s.

The second fixed rendering. The image cache's headroom sweep ran on every
load and, with free heap pinned just under its reserve, bought that reserve
out of the images the next frame would draw again — a treadmill that showed
up as evictions running one-for-one with loads while two thirds of the cache
budget sat unused. TEST2A's entire working set is ten images totalling
194 KB against a 979 KB cache, yet each was reloaded every frame. The sweep
now reclaims what a scene has stopped drawing and only takes the live set
when the heap is genuinely short, so a scene whose set does not fit (the map
cycles full-stage planes) still degrades to plain LRU. Large images also
moved to faster compression levels through `image_codec_tiers`, and the
per-pixel ink pass is skipped for the inks that leave pixels as authored.
Steady scenes now load nothing: TEST2A went from 280 loads and 2.84 s of
image work per five seconds of game time to zero and 13 ms, its render cost
from 3.5 s to 0.9 s, and it holds full tick delivery. Across the showcase,
dropped game time fell to 10.0 s with worst-case delivery at 0.67, and the
release boot to 1.3 s at 0.83. Rendering is no longer the limit.

A third round measured scene entry, which `NATIVE_ENTER_COST` shows is not
script execution but loading: across the showcase, 2.2 s of `dlopen` and
1.5 s of full-stage plane decompression against 0.5 s in `dg_enter`. Each
scene used to load its own overlay twice — once to read its cast list, then
again in size order — so the generated `movie_casts` table now supplies that
list and the load happens once (57 overlay loads to 44). The draw-order
build, which runs for the mouse hit test as well as each render, skips empty
channel words through the `active_sprites` bitmap instead of striding all
1001 channels. Dropped time fell to 9.3 s at 0.68 and the release boot to
1.3 s at 0.83 with 35 overruns.

Half of all overlay bytes were then still reloads of a library the previous
scene had released, because the authored flow separates every pair of real
scenes with a tiny transition movie and draining there dropped everything.
A scene now keeps the overlays it is about to use, and a transition carries
one spare across: the largest shared cast library. Exactly one, because
overlays load largest-first into a drained arena — that one sits at the
bottom and the region above it stays coalesced, whereas keeping several
punched holes through the middle. Free bytes were unchanged either way; the
largest block was not, falling by a third of a megabyte, and the map
screen's full-stage planes churned for it (its image traffic more than
doubled for the same frames). With the single spare the largest block at
every scene entry is unchanged, `dlopen` falls 1.88 s to 1.24 s, and dropped
time reaches 9.1 s.

This port also benefits from work measured on the Director 6 port, where
instrumenting a scene entry on the console showed that most interpreter
time went into linear searches of generated const tables rather than into
interpreting. Resolving a member by name now binary-searches a generated
folded-name index rather than striding the member table, and a property
read confirms where that name was last found before walking an object's
key/value pairs again. Service work across the release boot fell 20%, from
8.08 s to 6.46 s, and its dropped time from 1.21 s to 1.04 s. The showcase
reaches 7.5 s dropped at 0.751 worst-case delivery.

The ROM then had to come down. A cartridge may not exceed 56 MiB — the SDK's
USB debug channel reserves the last eight of the 64 and writes every line the
game prints to the start of that area, so a larger ROM overwrites its own
data while it runs — and this port was shipping 61.3 MiB, losing a few
hundred bytes of whatever the DragonFS layout put at that offset. Storing
four bits per colour channel instead of five brought it to 53.4 MiB with
room to spare: the stored size and layout are unchanged and each channel
keeps its endpoints, so pure white still keys matte ink, and the flat
authored artwork shows no difference at stage size. The largest images moved
off the fastest codec in the same change, because level 1 packs a fifth
larger than level 2 for the same tick delivery; the release boot's dropped
time is unchanged at 1.24 s.

The boot showcase is a 220 s replay-driven probe-profile capture
completing `REPLAY_COMPLETE id=deutsch-login-keyboard`: the intro with
music, the on-screen keyboard typing ANNA into the login name line, profile
confirm, class pick, demo skip, a visit to the castle's centre room, and its
exercise book into the TEST2A spelling task with the first answer card picked.
`director64 capture --game lernerfolg-deutsch-1-2` writes its video, frames and
validation receipt under the selected build directory.

```sh
uv run --locked director64 extract --game lernerfolg-deutsch-1-2
uv run --locked director64 recover --game lernerfolg-deutsch-1-2
uv run --locked director64 build --game lernerfolg-deutsch-1-2
uv run --locked director64 boot --game lernerfolg-deutsch-1-2
uv run --locked director64 capture --game lernerfolg-deutsch-1-2
```

The authored 800x600 stage renders on the 640x480 target through a 4/5
prescale of every converted image (`stage_prescale` in the source policy,
mirrored by `dg_prescale_dims` at the render boundary); geometry, scripts
and hit-testing stay in the authored space. Mask ink renders from the
authored `<name>_mask` member that follows each masked image, baked into
the texture's alpha at load; mask ink over any other neighbour copies, as
the original does with an unsuitable mask member (the class panel's
full-stage base is authored that way). A full-stage opaque plane releases
the planes beneath it from the 8 MiB image budget, and the conversion
snaps isolated sub-opaque resample pixels so effectively opaque planes
stay in the two-byte format — isolation is judged by density (two per
mille of the plane, and never on an image with any effectively
transparent pixel, which marks authored punch-out art), since authored
soft edges carry percents of partial pixels while resample strays
scatter a few hundred noise pixels over a room or task background at
any depth. Recorded approximation: the deferred Flash intro members
draw nothing (`NATIVE_RENDER_DEFERRED`) per their policy pins.

Text entry uses the platform's on-screen keyboard: clicking an editable
field (the login name line, write-in exercise boxes) opens the controller
grid, which types windows-1252 characters — including Ä Ö Ü ß — one
keyDown at a time through the same replacement machinery desktop typing
used, so the authored Enter/backspace/character filters all run. Text
glyphs render from 4/5-scaled font variants emitted at conversion, so
authored layouts keep their proportions under the stage prescale. The grid
answers to the player who opened it; outside it all four ports point with
their own coloured cursors
([shared pointer](../../docs/architecture.md#players-and-cursors)).

A scripted journey (`director64 native -- --sanitizers --journey`) drives
the full authored flow under ASan/UBSan: it creates a profile on the
login screen, types the name through the keyboard commit path, confirms,
picks a class, skips the map demo and enters a room — ending with zero
script alerts and the profile persisted to the FlashRAM archive.

`director64 probe` gates on that passing sanitized journey and bakes a
controller replay into a probe-profile ROM whose input follows the same
pointer path as manual play: it opens the on-screen keyboard on the name
line, walks the key grid to type ANNA, commits with Start, confirms,
picks class 1 and idles through the map intro show. `director64 capture`
records that replay as the showcase and requires its completion marker;
the capture continues through the narrated castle map that follows the
intro.

Several fixes hold the 60 Hz service clock through that flow. Hover
transitions sample once per tick instead of after every dispatched
handler, which had rescanned the thousand-channel score dozens of times
per tick (the former `DEUTSCH_MAINSCR_OVERRUN` gap). The image heap's
compaction pre-pass now runs only when the largest free block cannot
serve the frame's biggest missing image, tested by one probe allocation;
the map's large cycling film poses are uncached on every pose change but
ordinarily fit, and compacting for them reloaded the whole scene each
frame. Movie loads and residual overload cap the service backlog at
twelve ticks — the original never repaid loading time, and bursting
minutes of backlog would rush animation and audio far from the displayed
scene. If an image still does not fit, that sprite skips one frame and
the next render repacks the whole set, as the original skipped updates
under memory pressure rather than stopping the movie.

Two heap fixes keep scene entry within the 8 MiB budget. A movie's cast
overlays load largest-first: each decompresses into one contiguous block,
and authored order interleaved a megabyte library with small ones until
the next large library found no hole. Streamed speech decoders close on
movie entry — a `playFile` stream is not a channel member, so stopping
the sound channels left its buffers in the middle of the heap. Scene
entries log `largest=` beside `free=`, since a load fails on the largest
hole rather than the total. Speech and effects stream case-insensitively:
the converted `voc/` and `sfx/` set canonicalizes stems to lowercase, as
the authored Windows paths were resolved (`sfx\silence.aif` names the
on-disk `Silence.aif`).

`director64 perf` walks reachable states on the native probe: the
scripted journey first, then a systematic crawl that clicks every live
sprite region it has not tried in each movie, dwelling after each
transition and retracing known exits toward whatever still has ground to
cover. When a branch is exhausted it restarts the probe and replays the
journey to the deepest checkpoint with hotspots left, so the map's rooms
and exercises are reached in turn rather than the walk ending in the
first room. The probe accounts CPU time per movie tick; the report
(`work/native/perf/report.json`) gives per-movie mean and peak service
cost and peak Lingo value-heap use, flags movies over `--budget-us`,
lists unvisited registry movies, and records a probe failure as a finding
with the commands that reached it.

The sweep opened the exercise movies and drove the work that makes them
load. `DG_FILES` and `LV_SHARED` now cover a stage movie's eleven linked
cast files beside the controller window's own. The exercise task
databases ship verbatim under `tests_db/` and are read through the Buddy
API `baReadBinFile`, which answers the file's bytes as the list the
authored parser walks; the shipped path is resolved from the authored
Windows path by its folder tail, and the bytes are stored one per element
rather than a full value each, so the largest database costs 9 KiB of the
Lingo value heap instead of 150 KiB. A sprite answers `the castLibNum` of
the member it shows and `the frame` of its Flash timeline, a sound
channel answers `the endTime` of what is playing, `the
keyboardFocusSprite` follows the field an exercise activates, `bitOr` and
its siblings compute the UTF-8 encoding the exercises use, and
`debugAlert` traces instead of failing. Settings the conversion already
baked — Flash playback flags, vector outlines, per-character colour and
spacing — are accepted and dropped rather than stopping the movie.

The exercise prompts render. Their text lives in named edit fields inside
Flash card assets, which the conversion flattens to film timelines while
recording the dynamic surface: the PlaceObject2 instance names (`my_txt`,
`my2_txt`, `Selected`…), each field's bounds, font, size, color, alignment,
wrap and margins, and the frame-label table (`normal`/`red`/`green`/…). The
runtime ships all of it per member. A script addressing `sprite(n).my_txt`
gets a property bag seeded from that field's descriptor
(`FLASH_FIELD_OBJECT`); `setText` and `setVariable("text1", …)` decode the
authored percent-escapes (the recovered `int2hex` path) into UTF-8 — the
card SWFs' own DoInitAction literal pools show `setText` passing its
argument through `unescape`, which pins that decode — lay the text out
with the font's measured advances, and answer `textHeight` so the
authored vertical centering runs; `getTextFormat`/`setTextFormat` carry the
whole-field size and color (a character-range format renders in the base
style, traced `FLASH_FORMAT_RANGE_PENDING`). The platform draws the wrapped
lines above the flattened frame in the sprite's own draw order.
`goToFrame("red")` and `findLabel` resolve through the recorded label
table onto the flattened poses, so the cards' feedback states switch as
authored. A named non-field child still answers a geometry-seeded bag
(`FLASH_OBJECT_PENDING`) — its visibility toggles read back and the label
frames carry the authored art for the states the exercises use.
`setFlashProperty`/`getFlashProperty` address those bags; only a field's
`_visible` changes what renders.

Text metrics are measured, not guessed: the asset postprocessor packs a
metrics build of every authored (font, size) variant with the pinned
`mkfont` and decodes advance tables covering codepoints 32–255, so
`charPosOf` highlight boxes and the prompt overlays lay out with the same
glyph widths the renderer draws. A member whose style still has no table
keeps the half-em fallback, traced `TEXT_METRICS_PENDING`.

The two MX 2004 JavaScript-dialect handlers the pinned decompiler reports
as `unk26` are recovered from their compiled literal pools and lowered by
a source-pinned compatibility transform: `int2hex` is
`function int2hex(num) { return num.toString(16) }` (the exercises'
percent-encoder) and `clearGarbage` is
`function clearGarbage() { _system.gc() }`; both now run natively instead
of raising alert-and-continue script failures.

The Lingo value heap is the other exercise constraint. Live data after a
collection is about 128 KiB of the 384 KiB budget; the rest of a peak is
uncollected garbage. Collection runs between interpreter steps, so
whatever one step needs must fit in what is left: D10 collects at half
the heap rather than three quarters, and a list that cannot double near
the ceiling grows exactly instead of failing. Peak use across the swept
movies fell from 288 KiB to 192 KiB. When a single step still outgrows
the remainder (the deep-exploration `SCRIPTS.CXT new` and `SCRIPTSENG
exitFrame` cases), an emergency pass now frees garbage from earlier
steps in place: nothing moves and everything born during the current
step survives, so the raw heap pointers and untabled values native
helpers hold stay valid, and the allocation retries against the freed
tail or a first-fit hole. The next between-steps collection compacts
the holes away. The behavior lifecycle events (`new`, `beginSprite`)
dispatch through a collector-visible runner instead of one atomic span,
because a thousand-channel begin fan-out otherwise fills the heap with
garbage no pass may touch; and the mutable field table grew to 192
slots for the exercises' per-letter stamp members. The pinned fuzz seed
set that previously produced three unique failures — the two heap
exhaustions and the `int2hex` alert — now completes 80 episodes with
none. A longer 1167-episode sweep of the same seed base reached the
`TEST2CB1.DXR` exercise through `ROOM05` and found one open gap:
`TASKSCRIPTS.CXT initFields` asks for character metrics of a font the
recorded metrics table does not carry yet ("text character metrics:
unavailable"); the same episode fails identically on the previous code
generator, so it is a metrics-coverage gap, not a regression.

Saves follow the authored mechanism faithfully: the "save" cast is a
mutable text-member archive whose `castLib("save").fileName` retargets
between the shared "save" file and per-player "save01".."save08" files;
`save()` serializes the records (D6TF) into the two-generation FlashRAM
archive, `fx_FileDelete` removes a profile as a zero-length write, and
`the timeoutList`, `the globals` and `the result` behave as authored so
the movie-transition cleanup sweeps run unchanged.

## Port scaffolding and the controller window

The original projector opens `control.dir` as one invisible movie-in-a-window
("control"), navigates it between controller movies, and stage code calls the
window movie's interface handlers whose instances live in shared globals; the
window also opens as a visible statistics panel. The port models this as a
bounded controller-window service, not a desktop window manager:

- Every recovered `tell` form lowers natively: `tell the stage` inline,
  `tell window(…)` through window services, and sprite tells (references or
  sprite values) onto the flattened film timelines (`sprite_frame`,
  `sprite_lastframe`, `sprite_go`, with handler sends through `sendSprite`).
- The window movie's code attaches as a resident shared movie
  (`dg_window_attach`) so its handlers resolve from stage code across every
  movie change; `fileName`/`gotoMovie` requests record the movie stem for the
  platform to resolve. `visible`, `rect`, `open`, `forget`, `moveToFront` and
  `windowPresent` are served with the authored semantics.
- The platform completes window loads on both targets: the host probe and
  the N64 overlay loader resolve requested movies (`find_movie`), keep the
  window movies, their cast archives and any `castLib fileName` swap target
  resident across stage transitions, and exclude that resident set from
  other movies' event broadcasts. Explicitly pending, traced at runtime:
  the window's own score tick and rendering the visible panel
  (`WINDOW_MOVIE_SCORE_PENDING`, `WINDOW_PANEL_PENDING_RENDER`) — the
  core play loop never shows the panel. Deeper exercise journeys and
  service-tick performance are the next milestone.
- The two `unk26` handlers lower to explicit alert-and-continue script
  failures; `put … into member(…)` assigns member text, and `delete
  <variable>` disposes by assigning VOID.
- Saves: a two-generation FlashRAM archive holds the six authored
  preference/progress files plus eight numbered player profiles; the
  read-only `tests_db` exercise databases ship on the ROM filesystem. The
  D10 profile grew `LV_ROOTS` to 1536 — a thousand sprite behavior roots
  plus 192 mutable field slots and the Flash object bags overran the Lingo
  root table, caught by the target compiler and now held by a static
  assertion for every profile.

The manifest pins ISO SHA-256
`4c87e10dcc99dfb68882b51c9daceb7ce4761b482a2a43bf620093289353e0f4`.

## Source

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

Every file reports Director version 1000 (MX 2004). The host recovery tools
implement a D10 profile for this disc; the native runtime still implements
D5–D8 only. `recover` audits all 76 Director files plus the `Deutsch.dat`
projector against the pinned source policy, decodes every versioned score, and
verifies the 4,542 parsed handlers against their bytecode identities.

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
  decompiler cannot translate (opcode `unk26`); both have live call sites. They
  parse to explicit `unrecovered` nodes and are reported as
  `undecompiled-bytecode-unk26` lowering blockers, alongside 68
  `tell-window-context` sites: the projector drives `control.dxr` as a
  movie-in-a-window.
- Tempo commands use opcode 246 only, with rates 8–30.

## What a port still needs

- Native runtime: the D10 build profile exists and passes the synthetic native
  contracts (1,000 sprites for the recovered channels through 983, seventeen
  loaded files and sixteen shared casts for the window movie plus eleven
  linked externals, and the D7-shared hover cache, absent-cursor and
  cast-preserving member semantics). No ROM builds yet, and the
  movie-in-a-window launcher structure still has no runtime support.
- Xtras: the movies request Text Asset, TextXtra, Font Asset, Mix Services and
  Script Agent; the disc additionally ships Flash Asset, QT6Asset, ActiveX,
  INetURL/NetLingo/NetFile, ShockTalk, Sound Control, FileIO/FileXtra4/BudFile,
  PNG Import Export, SharpExport, PMatic, Cursor Asset, DirectSound and the SWA
  streaming pair. Current ports cover a much smaller surface.
- Budget: measured, and it fits — see "ROM packing" below.
- Host work: a save codec, pointer targets, journeys and a `runtime/game.h`
  adapter, none of which exist for this slug.

## Asset conversion

The shared converter compiles the corpus with 14 explicit residual problems;
`model.json` records every unconverted member. Converted today:

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
  the OpenType export accepts both signs. **Recovery gap closed ("Original
  fonts")**: the pinned LibreShockwave glyph-program decoder mis-decoded a
  minority of glyphs in all five of this game's fonts — `e`, `s`, `3`, `8`,
  `S` and several counters came out as filled blobs because closing curves
  ended hundreds of units past their contour start. The missing semantics
  were recovered from the disc's own `Font Xtra.x32` (the original
  Bitstream TrueDoc Character Shape Player): an orus-table lookup with
  direction 0 — the default for a curve command's unencoded control
  coordinates — infers its search direction from the travel of the current
  point relative to the previous one instead of returning the coordinate
  unchanged, and the y-control table is preprocessed before any command
  runs (glyph flag bit 2 duplicates its first entry, and an odd-length
  table is padded with its last). Both corrections were first applied as
  fail-closed patches to that decoder. Since 2026-09-27 the converter decodes
  PFR1 itself (`compiler/src/convert/pfr.rs`), implemented from the original
  player's behaviour rather than from LibreShockwave. Emulating the player's
  outline loop (Unicorn, identity output transform;
  `tools/fonts/pfr1-verify.py`) over every simple glyph of all six recovered
  fonts across the ports gives 688 of 688 identical, and 15,000 mutated glyph
  programs agree wherever the player's result is defined. The recovered
  OpenType fonts are byte-identical to the earlier ones.
- All 42 vectorShape members and 104 of the 111 Flash members, through the
  extended SWF rasterizer: multiple solid and gradient fills under per-style
  nonzero winding, solid line styles (including LineStyle2 cap/join/miter
  profiles) with minimum one-pixel stroke coverage, deterministic
  quadratic-curve flattening, partial-alpha fills, style replacement, nested
  sprite timelines, color transforms, clip-depth masking, embedded SWF font
  glyphs (`DefineFont` 1–3) with static `DefineText` and initial edit-text
  rendering through the corpus HTML subset, and full `PlaceObject2`/`3`
  timelines. Multi-frame timelines (85) flatten to per-frame `FDIA` assets
  played through the film mechanism (`flash-timeline-flattened`). The
  dynamic surface is recorded, never guessed: variable-bound edit texts
  become `flashFields` descriptors with instance name, bounds, font, size,
  color, alignment, wrap and margins that the runtime renders as prompt
  overlays (`flash-dynamic-text`; the Lingo surface is `setText`,
  `setVariable`, `getTextFormat`/`setTextFormat`, `play`, `goToFrame`,
  `rewind` and `findLabel`), frame labels and top-level `stop()` frames
  persist as `flashTimeline` and drive the labelled feedback seeks, named
  non-field children persist as `flashNamed` evidence for the
  `setFlashProperty` surface, and every other ActionScript sequence,
  filter list, blend mode and clip action is retained as classified evidence
  (`flash-actions-ignored`, `flash-effects-ignored`). SWF 6+ strings decode
  as UTF-8. The single Mucklas vectorShape converts byte-identically.
- 354 of 357 film loops flatten offline. The extended flattener draws
  unfilled shape children with the runtime's exact span geometry, stencils
  mask ink through the neighbouring mask member (the corpus masks are 8-bit
  with anti-aliased edges; grays threshold at half luminance under a recorded
  `film-loop-mask-antialiased` limit), captures D6+ sound-channel references
  (native playback still D5-only, recorded as
  `film-loop-sounds-pending-native`), resolves external-cast children, and
  treats behaviors on children as inert render-only channels per the
  reference. Cast-library-zero and stale absent-member references draw
  nothing, as in the reference, each recorded per channel.

The 14 residual conversions — 7 long full-stage timelines (the Game 2 track
displays at 133–155 frames of 800×600, `goToFrame`-driven native-playback
candidates; the 128-frame flatten bound is a deliberate budget limit),
2 Flash members with streaming sound, 1 with a button, 3 film loops (a text
child, a nested loop child and an unconverted Xtra child), and 1 cursor Xtra
member — are pinned per member as `deferred_conversions` in the source
policy with their dispositions. Each becomes an explicit
`deferred-conversion` approximation; a pin without a matching residual fails
the conversion as stale.

## ROM packing

The shared pack pipeline (`mkasset -c 3` images, `audioconv64
--wav-compress 2` ULC audio, `mkfont` variants, all inside the pinned
toolchain) packs the full converted corpus. Measured results:

| Component | Source | Packed |
| --- | ---: | ---: |
| Member images (11,294 assets) | 983 MiB FDI/FDIA | 27.4 MiB |
| Member sounds (301 assets) | 44 MiB WAV | 1.9 MiB |
| Font variants (15) | | 0.1 MiB |
| Speech `voc/` (3,660 files) | 304 MiB | 17.4 MiB |
| Effects and music `sfx/` (54) | 19.5 MiB | 0.6 MiB |
| Assets total | | 47.4 MiB |

The speech measurement converts every `voc/` and `sfx/` file to ULC wav64
with zero failures. The disc stores 853 of the `.aif` files as MP3 (one with
an ID3 tag) and three as WAV; conversion selects the codec by magic bytes,
not extension. ULC was selected over VADPCM (77.6 MiB projected) and Opus
(33.5 MiB projected) on measured ratios; it is the codec the pipeline
already applies to member sounds and Löwenzahn's video audio, and the Lingo
`sound playFile` call sites map onto wav64 file streaming. With a
Mucklas-scaled code/scene allowance (~10 MiB for 2.4× the handlers), the
projected ROM was ~58 MiB; the built ROM is 53.4 MiB against the 56 MiB
ceiling. The speech wav64 set is
a measured build artifact (`build/…/voc-wav64/`); the port's asset
postprocessor adopts it when the `[port]` section lands.

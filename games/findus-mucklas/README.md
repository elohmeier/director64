# Findus bei den Mucklas

Experimental Director 8 N64 port of the locally supplied German disc. Generated
source, converted assets, saves, captures, and ROMs stay in ignored game/source
workspaces; the repository contains the compiler, runtime, adapters, and tests.

```sh
uv run --locked director64 assets --game findus-mucklas
uv run --locked director64 native --game findus-mucklas --sanitizers --activity
uv run --locked director64 native --game findus-mucklas --sanitizers --regressions
uv run --locked director64 build --game findus-mucklas --reuse-assets
uv run --locked director64 boot --game findus-mucklas
uv run --locked director64 capture --game findus-mucklas train obstacle
uv run --locked director64 capture --game findus-mucklas train-departure
uv run --locked director64 capture --game findus-mucklas train-soak
```

The manifest pins ISO SHA-256
`ad4c7afb4b19339c45c2a69756b522b59ba4869199740fd528527f190a806925`.
The converter processes 48 media archives and the embedded projector as the
native `START.DXR` unit. All 630 scripts map to 1,896 native handlers in 49 code
units. `aot/program.json` retains the media AST; `aot/native-program.json` adds
the original launcher with its source identity and records selected-port corrections.
No Lingo bytecode interpreter runs on the console.

Native lowering covers persistent script objects, resumable methods and nested
calls, properties, list and property-list indexing, `case`, collection loops,
and chunk reads/writes/deletion. The D8 profile supports 800 sprites, twelve
cast libraries, eight sound channels, RGB score colors, rotation/skew, and the
recovered tempo operands through 999 FPS. Source recovery retains the original
48-byte score records, field validity masks, and extended values.

All 5,344 bitmap references, 1,285 sound references, and 21 film loops are
accounted for, including one authored empty bitmap and one empty film. The static
vector-shape Xtra is rasterized from its embedded SWF. All 61 text members retain
their source payloads and Paige typography. Both embedded font members contain
the same original Pettson PFR1 font: its 223 glyphs are recovered into OpenType
with cubic outlines intact, then rasterized at the seven authored sizes by the
pinned SDK. The glyph-program decoder carries the curve-command corrections
recovered from the original Bitstream player (see `tools/fonts/README.md`);
128 Pettson glyphs pick up small authoritative outline refinements from them,
verified byte-identical against the emulated original for every glyph. Player-name and score fields use this font with authored colors,
alignment, baselines and line spacing. Two font variants can reside at once.
Source hinting is not exported; no original-projector pixel comparison is claimed.
Missing system fonts and unsupported authoring-field layout remain explicit in
`director/model.json`; `analysis/font-reachability-audit.json` distinguishes
concrete references from absence of reference evidence.

Soft-alpha images use compact FDIA resources: RGB555 color plus an exact A8
coverage plane, drawn with paired N64 textures. Binary-alpha images retain the
smaller FDI1 format. Film-loop composition also preserves partial coverage.
Images wider or taller than 1024 pixels are stored as 32×32 tiles, which is
what lets the RDP address them: this covers the train activity's 1182×127
`ts_st4` texture assertion, its other wide backgrounds and BR's 1920-pixel
images. Tile uploads use local UVs and retain full color/alpha precision, with
matching CPU addresses for matte hit testing and Darken ink, and a padded
plane's size counts against the cache budget with its lifetime following the
display completion fence.

The converter writes that order, so the console never rearranges a plane it
loads. It used to: the cache repacked each wide image in place on arrival,
which cost about 460 ms for a station pavement and was paid again every time
the scene scrolled back to one — 1.85 seconds per minute of station play, a
fifth of all the time this port spent on images. Nothing in the file records
the layout. All three implementations derive it from the stored dimensions —
`plane_pixels` in `compiler/src/convert/fdi.rs`, `bitmap_plane_pixels` in
`runtime/director/bitmap_tiles.h`, and `plane_pixels` in `full_assets.py` for
the colour quantiser — and `compiler/tests/image_tiles.rs` and
`tests/test_image_tiles.py` compile the runtime's arithmetic against the
converter's and the packer's to check they agree, because a disagreement
would render every wide image scrambled rather than fail.
Padding is zero, which every ink leaves alone, so ink conversion walks the
stored plane in one sequential pass. Portable tests cover the tile order, the
one-slot-per-pixel property, edge padding and the 1024-pixel boundary.
Image loading accounts for the SDK's decompression buffer size and can evict
completed cache entries on allocation failure, including fragmentation below
the nominal cache budget.
ULC preserves source rates and channels, with sub-millisecond trailing alignment
truncation recorded in `analysis/asset-packing.json`. The release budget is the
framework's 56 MiB ceiling: the SDK's USB debug channel reserves the cartridge
above that as scratch and overwrites it while the game runs, so the earlier
62 MiB budget was silently losing a few hundred bytes of whatever the DragonFS
layout happened to put at that offset. This port paid for the ceiling, and for
a faster image codec, by storing four bits per colour channel instead of five
(`image_color_bits` in the source policy). The stored size and layout are
unchanged and each channel keeps its endpoints, so pure white still keys matte
ink; the reduction packs about 30% smaller and is not visible in the
watercolour artwork at stage size. The ROM fell from 59.75 MiB to 51.98:
image decompression across the boot fell from 5.08 s to 1.25 s, dropped
service time from 3.50 s to 0.67 s, and tick delivery rose from 0.738 to 0.934.

That room is what pays for the codec. Every image now uses the middle level
rather than only those a scene waits for above 64 KiB decompressed, which is
a compression setting, not a quality one — the pixels are identical either
way. Measured on the console, level 3 decompresses at 0.7-1.3 MB/s against
level 2's 3.4-5.3, and the small images the earlier tier left at level 3 are
most of what a scene entry waits for: decompression across the train capture
fell 3.84 s to 2.59 s and across the departure capture 5.27 s to 3.43 s, for
2.33 MiB of ROM (50.59 to 52.92, against the 56 MiB ceiling). Promoting the
large band to level 1 as well would be another 7 MiB and does not fit.

The game adapter exposes only its fixed installation/save filenames and uses a
separate `M64D` FlashRAM journal. A fresh game starts without imported player
progress. Two independent 64 KiB generations preserve the previous commit on a
failed or torn write. Analog-stick pointer control and A operate the original
mouse interface. Click the player-name field to open the on-screen keyboard:
use the stick or D-pad to select a character, A to type, B to delete, and Start
or OK to close it. The original confirmation button saves the name. Key presses
run LO's original handlers, including letter sounds and its 20-character limit.
C-buttons supply arrow keys in KR/KB. Controller 2 supplies directions through
its stick/D-pad and space through A; KR uses the live localized second-driver
bindings. Steering reads those two ports directly, because a race belongs to the
players sitting at pads one and two rather than to whoever holds the pointer.
Unplugging a pad or leaving the activity releases its keys. The ROM header and
menu metadata declare four controllers/players: outside the races every port
points with its own coloured cursor
([shared pointer](../../docs/architecture.md#players-and-cursors)).
The ROM requires 8 MiB RDRAM (Expansion Pak on an original N64) and FlashRAM saves.

Local outputs live under `build/findus-mucklas/sha256-ad4c7afb4b19/` and
`dist/findus-mucklas/sha256-ad4c7afb4b19/`. Accountability, compiler, packing,
native journey, and ROM reports distinguish source coverage from execution.

ScummVM was clean at `41ac2b31847622d0662d22c03fe6979e3b43cfbc` on 2026-09-06.
The local implementation reference and recovered source are corroborated with
focused compiler/native tests. The sanitizer journey creates a fresh player,
enters the house, reboots, reloads that player, and solves the clock activity
through the original controls. It checks all three correct arrivals, the reward
animation, the inventory addition, and persistence across another reboot.
Sparse event/film tracking, cached event masks, bounded allocation/GC scans and a
four-way handler lookup cache preserve authored call counts. The lookup cache
prevents alternating `count()`/`sound()` calls from repeatedly evicting each other.
Ancestor lookup scans existing property names without allocating a temporary
symbol on each missing method; a native contract checks repeated misses for heap
growth while retaining inherited method resolution.
An earlier LO benchmark reduced opening-speech CPU time from about 6.97 to 4.13 seconds
per 300 service ticks in Gopher64. Complete measured LO work windows, including
rendering and audio service, fit their five-second budget. Transient scene-loading
backlog remains and drains; whole-game realtime pacing is not qualified.

The boot gate records 60 seconds in LO, requires the original
`lo_state=#valjer` player chooser, and rejects over-budget work or missing samples
across at least eight consecutive complete 300-tick intervals. An optional
saved-chooser capture uses the
sanitizer journey's hashed FlashRAM fixture to exercise visible original-font
glyphs; this is an emulator rendering check, not an N64 controller journey.
These checks cover the recorded journeys, not a complete playthrough.
No original projector comparison, M64, or original N64
qualification has been performed.

The activity regression driver replays fifteen routes from a fresh installation,
including invention release/drag, NB's fly, BR's course drawing, and FA's catching
phase. It records commands, states, sanitizer logs and the executable hash under
`native/regressions/`. This extends entrance and interaction coverage, not
completion coverage for every activity.

D8 sprite constraints clamp the registration point against the live bounds of
the referenced channel during position changes and dragging. `keyPressed()`
queries held Macintosh key codes or characters through `dg_key`; no held keys
correctly returns false during the invention mouse-up checks. Zero/truncated-zero
random bounds use the reference implementation's 1–65535 range, fixing FA's
`random(abs(distance - 40))` path while retaining the existing seeded generator.

BR's two authored `getAt(avstList, a)` calls use symbol keys with a property list.
Their bytecode extcall offsets are 546 and 579 in member 4's `ritaBana` handler.
`host/compatibility.py` corrects those calls to `getaProp` for the pinned disc and
script hash, and fails if either source identity or the two expressions change.
Generic `getAt` still requires a numeric index; the original recovered AST stays
unchanged. The native program/manifest records this correction explicitly. It is
not verified against the original projector. A fresh installation also sets
`Vind=0`, honoring the source's gate for the VI attic activity absent from this disc.

Remaining qualification includes full activity completions, second-player
playthroughs, double-click behavior, unresolved font/layout approximations,
worst-case runtime memory/timing, and hardware testing.

D8 collection now compacts in maintained allocation order without sorting handles.
It collects after half the handle capacity has been allocated, with additional
heap/handle pressure guards.
Handler calls clear declared locals; flat list duplication retains the same object
identity semantics without allocating a full recursive-copy map. Method caches are
cleared when overlays change, including cached misses. Focused tests cover handle
reuse, reordered compaction, nested duplication, keyboard transitions and cache
invalidation; the sanitizer regression set also types, deletes, saves and reloads a
player name through the recovered LO scripts.

The N64 renderer clips offscreen source tiles, skips unchanged D8 scenes, and
caches repeated bitmap command sequences within a separate 192 KiB ceiling.
Command blocks share their image's display fence and are released on eviction or
scene changes. Recording starts only after the image has survived a completed
frame, avoiding command allocations between large initial image loads. Allocation
pressure can discard completed command blocks while retaining their pixels.
Changed geometry uses ordinary drawing; CPU Darken remains outside
recorded blocks. `NATIVE_CACHE` reports allocation retries, loads, evictions,
command memory and observed free heap in each 300-tick interval.

Cached image pixels come from the scene's bundle block, one allocation the
platform takes at every scene entry for the images the scene's recorded
working set draws most (`host/working-sets.json`, `director64 working-sets`),
and returns whole at the next entry. Image blocks are the largest and
shortest-lived allocations the loop makes, and cycling them through the
malloc arena fragmented it: an hour of station play ended with an 814 KiB
allocation failing for 688 consecutive frames against 2.4 MB of free heap, so
the pavement went unrendered for seconds at a time and every frame reloaded
its neighbours. A 3,456 KiB region reserved at boot, with its own compactor,
fixed that for this port alone; the bundle gives every port the same
guarantee, sized to the scene, and only images past the block or outside the
recorded set still cycle through the arena's overflow cache.
`NATIVE_BUNDLE` reports each entry's rows, residents, bytes and budget, and
`NATIVE_CACHE bundle_bytes` the block in each 300-tick interval.

The probe-only train/obstacle captures use the on-screen name keyboard and original
activity controls before and during a 60-second, 60 FPS recording. The gate checks
at least eight contiguous complete five-second guest work windows, allocation and
cache limits, decoded video/audio, and interaction markers. This is measured
emulator activity coverage, not full-game pacing or hardware qualification.

The train departure regression answers the station's passenger-count question
through the original number board and checks sustained piston animation. The
native journey continues to the next station. This covers the reported left-edge
flicker and `rdpq_triangle_rsp` floating-point cast exception at `0x80069798`:
the piston script repeatedly feeds `sprite.height` into its next quad, and the
runtime previously returned the deformed bounding-box height, causing exponential
growth. Sprite width/height now retain their dimensions before transformation;
rect/edges still describe the transformed bounds. The clean ScummVM reference
at `41ac2b31847622d0662d22c03fe6979e3b43cfbc` reads width/height from sprite
dimensions; its implementation does not verify D8 custom-quad behavior against
the original projector. Portable contracts cover rotation, skew, stretching,
member dimensions and repeated quad updates; recovered-source replays exercise
the actual train script.
The departure capture records pacing and recovered allocation retries separately
from its crash/geometry gate, while still enforcing the image-cache ceiling;
the existing train and obstacle timing gates still require every measured
five-second interval to fit its work budget. Departure now meets that budget
too — 4.79 s against 5 — but its gate stays advisory, because what it exists
to catch is the piston geometry rather than the pacing.

The probe answers the shared `perf` and `heap` RPCs
(`platforms/native/probe_stats.inc`), and `host/full_journey.py` declares a
perf-sweep spine that boots through login and the bedroom into the train
station, so `director64 perf --game findus-mucklas` measures TS directly
(22.7 µs mean host tick over 12k ticks) and `--soak 216000` holds it for an
hour of game time. The idle soak is stationary — 5,496 live handles and
197,584 live bytes, flat across 720 windows.

The reported long-session slowdown was not the value heap. Five hundred
played departure cycles on the host probe left the live set flat (5,496 to
5,577 handles, mean tick cost 11.7 to 11.9 µs), and the collector costs
2-3% of the guest's service budget throughout. It was the malloc arena
fragmenting under image churn, which only the console's allocator shows:

    director64 capture --game findus-mucklas train-soak

plays an hour of station arrivals through the original number board
(`DIRECTOR64_REPLAY_SCENARIO=4`) and judges the shape of that hour rather
than a single window — tick delivery from its first quarter to its last, and
whether any sprite went unrendered for want of image memory. Before the
reserved region, station play decayed from 0.965 to 0.571 delivery within
four minutes and then halted on `visible image working set exceeds cache`,
with the platform strip missing for runs of 217 and 844 consecutive renders.
After it, 701 station windows over 58 minutes hold 0.975 to 0.965 with no
skipped render, no allocation retry and no failed save; live objects and
value-heap bytes are flat across the run. The scene does not fit a 60 Hz
tick even so — it delivers at ~0.96, not 1.0, and its worst window is 0.55 —
so the gate asserts the shape of the hour rather than the budget.

What the station spends its service budget on is its own score dispatch and
scripts, and both were paying for work the shape of the data already
answered. Three findings, each measured on the native probe driven through
the station and confirmed on the ROM:

* Every frame asked all ninety-odd member-bearing channels whether they
  handle prepareFrame, enterFrame and exitFrame, and nearly none do.
  `event_quiet` indexes that answer per channel — the same fact
  `event_misses` already caches per member, indexed so the phase's bit scan
  passes over a silent channel instead of resolving its member. It is
  dropped by the funnel that already maintains `event_channels`, so a
  changed member or behavior list re-asks.
* Film-loop advancement resolved every one of those channels' members on
  every frame to find the handful that animate; the answer is memoised
  against the member ID that would change it.
* Property reads, operator dispatch and `the` reads each walked a chain of
  full string comparisons against literals. At `-Os` every one of those is a
  real call, so each now rejects on a folded first byte, and `fold_char`
  itself is forced inline rather than left as a call in the hottest path in
  the runtime.

Together with inlining the expression-stack ops and answering the property
memo before computing the key's length, that is a 21% cut in `ticks_us` per
station window on the ROM (3.50 to 2.77 seconds per 300 ticks) and a drop in
measured duty from 0.80 to 0.71. None of it changes what runs: the dispatch
still charges its guard per channel, and the pinned-seed fuzz replays
21,346 commands to byte-identical outcomes.

Symbols are interned for every profile rather than only the extended-D6 one,
as Director interns them: a literal evaluated in a loop yields the object it
yielded last time instead of a fresh copy. The cached ID is weak and is
confirmed against the live object, so a collected symbol simply misses. The
image cache also carries a hashed key per slot, held apart from the entries,
so a lookup walks 768 bytes of integers rather than calling `strcmp` against
a hundred and ninety-two names — every sprite drawn asks for its image on
every frame, and that scan alone cost 57 seconds of an hour of station play.
With the codec change above and conversion-time tiling, image work across the
departure capture falls from 9.06 to 5.08 seconds, and the three activity
captures all fit their five-second work budget — the departure capture
included, which it never had before, though its interactive replay varies
enough between runs that its gate stays advisory.

An hour of station play did not slow down, but it stuttered: three times a
minute the loop stalled for 250 to 600 ms, and every stall longer than the
AI's 160 ms of queued audio played on a console as a gap with a pop at each
edge. The emulator hides that: gopher64 records only the samples the AI
fetched, so an underrun is spliced out of the capture instead of heard, and
it shows as a capture 8.4 s shorter per quarter hour than the time it
covers. Nine tenths of the stalls were the image block compacting — moving
2 to 3 MB of images for a request as small as 8 KB, while half a megabyte
or more of images that had not been drawn for hundreds of frames sat in
the block. A load now evicts the cheapest contiguous run of stale images
first, then moves only the images inside the cheapest run, and compacts
the whole block only if neither exists; the moves, the decompression and
ink passes, and the save's FlashRAM writes serve the audio between their
pieces. A sound replaced or stopped while loud now ramps down over 128
samples instead of being cut mid-waveform. Over the hour, stalls of 100 ms
or more fell from 3.2 to 0.8 a minute, tick delivery rose from 0.9955 to
0.9993, and audio lost to underruns fell from about 34 s to 0.2 s. What
remains is the authored save handler (about 450 ms, twice in fifteen
minutes), decompressing a platform strip (about 130 ms) and a collection
during play (about 85 ms, inside the audio's cover). `NATIVE_CACHE` reports
the moves as `compact_us`, `compact_bytes` and `region_windows`.

The 2026-09-06 combined emulator check passed both activity gates: maximum measured
work was 4.571 seconds per five-second train interval and 4.882 seconds for the
obstacle course. Minimum observed free heap was 346,488 and 264,408 bytes,
respectively, with no allocation retries in the measured windows. Both recordings
include name entry and subsequent activity input. Artifacts and ROM hashes are in
`build/findus-mucklas/activity-fixes/final-capture/validation.json`. The sixteen
native regression cases and the clock completion/reward/reload journey also
passed under address/undefined sanitizers.

# Findus wartet auf Weihnachten

Experimental Director 7 port of the German Christmas calendar. The self-contained
N64 ROM contains the original launcher, calendar, 24 activities, artwork, speech
and music. It requires 8 MiB RDRAM. Move the pointer with the analog stick; A clicks
or drags. Click a calendar door to open it, then click it again to enter. The
original buttons at the bottom of each activity start/restart and return to the
calendar. All four console ports play together, each with its own coloured cursor
([shared pointer](../../docs/architecture.md#players-and-cursors)).

The clock is fixed to Sunday, December 24, 2000, making all 24 doors available.
There is no RTC or wall-clock dependency.

```sh
uv run --locked director64 build --game findus-christmas
uv run --locked director64 native --game findus-christmas --sanitizers --journey
uv run --locked director64 boot --game findus-christmas
uv run --locked director64 probe --game findus-christmas
uv run --locked director64 capture --game findus-christmas
```

The playable ROM is
`dist/findus-christmas/sha256-1b087d267195/findus-christmas.z64` (approximately
20 MiB). The separate `findus-christmas-probe.z64` drives construction-piece
placement, clearing, calendar return, and the Christmas Eve story with controller
input. Native journey evidence is required before generating that replay.

## Recovery and runtime

The local `FINDUS3.ISO` is pinned by SHA-256
`1b087d267195d9be864f75c3d14dcb4a119ef0ded2ba178c41315a35475b18d3`.
The 31 media files in `MAIN` and embedded launcher identify Director 7 (700).
All 589 selected script blocks and 655 handlers map to 32 native overlays.
The recovered score uses format 13, 48-byte channel records, and 1,006 allocated
channels. Displayed channel counts include 120, 150 and 500; the runtime provides
800 sprites, eight sound channels and the D7 tempo range through 999 FPS.

The executable contains five embedded archives. The first supplies the launcher.
The remaining copies are hash-pinned and compared with the external media:
KICKER, ROUTS and SHARED have identical scripts. The embedded calendar differs
only in member 12's pre-December date gate (2000 versus the external copy's 1997
logic). The port selects the external calendar; both agree for the fixed
Christmas Eve date. Archive dispositions are recorded in source accountability.
D7 repeated external cast resource IDs are normalized only when their duplicated
local script contents agree.

The shared extended runtime services handle behavior instances, yielding frame
and mouse events, local script helper calls, dragging, cast-preserving memberNum
animation, marker/movie navigation, FileIO and bounded allocation. D7 sprite frame
events run before frame scripts, so a frame's `go(the frame)` cannot skip button
behaviors. Movie links ending in `.dir` resolve to recovered `.DXR` files. Mouse
hit testing excludes decorative behaviors with no mouse handlers; this is
corroborated by the source's overlaid snowboard controls and pointer graphics.

FlashRAM has two CRC-checked generations with readback and torn-write recovery.
It stores the pointer preference and the bounded `ByggFil.txt` file. This edition
retains construction save/read handlers but no callers or exposed save controls;
the port preserves them without adding a new save UI. The file adapter has native
contract tests; construction placement, clearing and return use the source UI.

## Evidence and limits

The sanitized native journey enters all 24 doors, starts their activities and
returns to the calendar. It additionally checks construction-piece placement and
clearing. Gopher64 captures separately validate rendered release startup and the
controller replay. ROM receipts verify the header, metadata, DFS bytes, packed
resources, native overlays and pinned libdragon image. None of these checks is a
complete playthrough of every puzzle or an original-projector comparison.

Reference behavior was inspected in the clean local ScummVM checkout at
`41ac2b31847622d0662d22c03fe6979e3b43cfbc`. Its versioned score decoder, event
handling, memberNum semantics, cursor handling and FileIO are implementation
references, not hardware evidence. Neither original N64 nor M64 hardware has
been tested for this port.

Known fidelity limits are retained in the asset and ROM receipts:

- Source text uses a pinned Droid Sans substitute with recovered STXT metrics;
  mixed fonts and faces use the first source run.
- Day 15's unsupported pattern 63 uses a hash-pinned solid-fill substitute.
- Day 24 has bitmap palette references to cast 0/member 21. These use the pinned
  matching local CLUT also selected by the authored score. This repair has not
  been compared with the original projector.
- Day 13 references absent cursor members 2000/2001; the current cursor is kept.
  Day 23's absent score sound member 124 produces silence. Both are traced, and
  no replacement commercial media is invented.
- Puppet square transitions use a timed reveal while the score continues.
  Rendering and timing are not qualified against the original projector.

This port's remaining stall was image decompression. Every asset packed at the
smallest size, which the console expands at under a megabyte a second, so the
calendar and its day scenes spent 2.12 s of the release boot decompressing
bitmaps. Anything a scene waits for — 64 KiB decompressed and up — now uses the
middle codec, measured at four times the rate for 2 MiB more ROM. Image work
fell to 0.60 s and the boot holds full 60 Hz tick delivery from start to finish
with no dropped service time at all.

Source media and all derived commercial assets remain local and ignored.

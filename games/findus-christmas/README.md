# Findus wartet auf Weihnachten

Experimental Director 7 port of the German Christmas calendar: the original
launcher, calendar, 24 activities, artwork, speech and music. Move the pointer
with the stick (or a mouse); A clicks or drags. Click a calendar door to open
it, then click it again to enter. The original buttons at the bottom of each
activity start/restart and return to the calendar. Up to four controllers play
together, each with its own coloured cursor
([shared pointer](../../docs/architecture.md#players-and-cursors)).

The clock is fixed to Sunday, December 24, 2000, making all 24 doors available.
There is no wall-clock dependency.

```sh
uv run --locked director64 android build --game findus-christmas
uv run --locked director64 android install --game findus-christmas --serial <device>
uv run --locked director64 web --game findus-christmas --serve
uv run --locked director64 journey --game findus-christmas
```

The journey (`host/full_journey.py`) runs over the web build's probe.

## Recovery and runtime

The local `FINDUS3.ISO` is pinned by SHA-256
`1b087d267195d9be864f75c3d14dcb4a119ef0ded2ba178c41315a35475b18d3`.
The 31 media files in `MAIN` and embedded launcher identify Director 7 (700).
The recovered score uses format 13, 48-byte channel records, and 1,006 allocated
channels. Displayed channel counts include 120, 150 and 500; the runtime provides
800 sprites, eight sound channels and the D7 tempo range through 999 FPS.

The executable contains five embedded archives. The first supplies the launcher.
The remaining copies are hash-pinned and compared with the external media:
KICKER, ROUTS and SHARED have identical scripts. The embedded calendar differs
only in member 12's pre-December date gate (2000 versus the external copy's 1997
logic). The port selects the external calendar; both agree for the fixed
Christmas Eve date. D7 repeated external cast resource IDs are normalized only
when their duplicated local script contents agree.

The shared extended runtime services handle behavior instances, yielding frame
and mouse events, local script helper calls, dragging, cast-preserving memberNum
animation, marker/movie navigation, FileIO and bounded allocation. D7 sprite frame
events run before frame scripts, so a frame's `go(the frame)` cannot skip button
behaviors. Movie links ending in `.dir` resolve to recovered `.DXR` files. Mouse
hit testing excludes decorative behaviors with no mouse handlers; this is
corroborated by the source's overlaid snowboard controls and pointer graphics.

The save image has two CRC-checked generations with torn-write recovery. It
stores the pointer preference and the bounded `ByggFil.txt` file. This edition
retains construction save/read handlers but no callers or exposed save controls;
the port preserves them without adding a new save UI. The file adapter has
contract tests; construction placement, clearing and return use the source UI.

## Evidence and limits

The scripted journey enters all 24 doors, starts their activities and returns
to the calendar. It additionally checks construction-piece placement and
clearing. It is not a complete playthrough of every puzzle or an
original-projector comparison.

Reference behavior was inspected in the clean local ScummVM checkout at
`41ac2b31847622d0662d22c03fe6979e3b43cfbc`. Its versioned score decoder, event
handling, memberNum semantics, cursor handling and FileIO are implementation
references.

Known fidelity limits:

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

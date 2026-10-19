# Löwenzahn 1

An experimental port of the German disc catalogued as
`sha256-f31970b980f0/LOEWENZA.iso`. The recovered media declares Director 5
(raw version 1217), so this adapter uses the D5 runtime profile.

The conversion includes all 66 linked MOV files (58 with video), 219 external
AIFF files and 109 cast sound references; FFmpeg transcodes the videos to VP9
with their sound. This game writes no player saves.

## Build and controls

```sh
uv run --locked director64 android build --game loewenzahn-1
uv run --locked director64 android install --game loewenzahn-1 --serial <device>
uv run --locked director64 web --game loewenzahn-1 --serve
```

- Move the pointer with the stick, D-pad or a mouse; A clicks or holds a
  source control.
- Up to four controllers play together, each with its own coloured cursor
  ([shared pointer](../../docs/architecture.md#players-and-cursors)).
- Use the panorama's arrows and objects to explore. The wagon icon returns
  from a topic. The flower opens the original volume/exit dialog; click outside
  it to return to the suspended scene.
- The print stamp in either book prints the selected chapter. The web app
  opens the browser's print dialog with a page it lays out itself: the
  chapter's recovered title and text and the book's original print artwork
  (the 1-bit PICT logos and illustrations, converted to PNG by
  `compiler/src/convert/print.rs`), A4. The Android TV player has no printer
  and shows a notice instead. Either way B returns to the book.

## Journeys

```sh
uv run --locked director64 journey --game loewenzahn-1
uv run --locked director64 journey --game loewenzahn-1 full_regressions
uv run --locked director64 journey --game loewenzahn-1 cursor_journey
uv run --locked director64 journey --game loewenzahn-1 printing_journey
```

The journeys run over the web build's probe with pointer input only. The full
journey runs the source launcher and intro, opens all nine topic menus (wood,
mole, weeds, potatoes, ships, hammer, salt, film, hearing), selects their first
topic, and returns to the appropriate panorama view. The wood case also opens
and closes DIALOG; the weeds case opens the craft book. They do not exhaust
every activity, quiz answer, recipe or craft page.

The regressions place and hammer a nail, hang a picture, require its complete
reward video, reset the wallpaper, play the kitchen radio and stop it on
navigation, and measure animation advancement at 1, 11 and 24 fps. D5 retains
the mouse-release target while a drag handler finishes, so releasing a picture
cannot stop its new video. The dialog case drags the volume control through
mute, midpoint and maximum, then checks that the chosen volume survives closing
and reopening the dialog. Classic `sound i` references resolve to mixer
channels. The printing journey presses every print stamp and checks the pause,
dismissal and chapter return.

The runtime converts Director's default 60-unit Lingo video times to the
internal 600-unit media clock, so both intro clips play their full source
durations (27.77 and 24.93 seconds). Flattened film loops retain transparent
padding, including the panorama's orange flag.

## Fidelity and remaining work

D5 additions include direct script references, ancestor method lookup,
actorList callbacks, mouse-event propagation, sound/video waits,
movieTime/rate/stopTime, shared stopMovie dispatch, cross-movie labels, and
suspension/restoration of the single source dialog window. A loop that polls a
linked movie's rate or time or `the stillDown` (the intro's
`repeat while the movieRate of sprite 2`) yields at its back edge once per tick,
as timer, mouse and sound polls do.

The film activity includes one explicit game correction: FIL frame 33's
authored 30 fps command is removed so the source slider's `puppetTempo` controls
the animation. The model records this difference as
`film-speed-score-correction`; the recovered score remains untouched in
`analysis/`. ScummVM gives the authored tempo command precedence too, so this
correction does not change the shared Director scheduler or claim an
original-projector comparison. Picture-name symbols retain their extensions,
drag constraints use live sprite bounds, and external audio lookup tries an
exact filename before the reference's `.AIF`/`.WAV` extension fallbacks.
Numeric assignments to text fields are converted to strings, including the
film slider's speed display.

The shared engine renders the seven original monochrome cursors from CURSOR.CXT:
four navigation arrows, Hotspot, Hand and Greif. Cursor selection follows sprite
hit geometry and masks, with a movie default when no sprite supplies an override.
The cursor journey covers all seven shapes, Hand→Greif→Hand while dragging the
volume mower, and the stage cursor outside the dialog. This remains
implementation-reference agreement; mixed sprite/default hiding precedence has
not been compared with the original projector.

Recorded presentation differences:

- Lossy video and audio conversion.
- Standard OS cursor roles use target glyphs; authored bitmap cursors use their
  recovered image/mask pairs. Executable-resource cursors and Cursor Asset Xtras
  are not implemented.
- Cuts instead of authored visual transitions, and an unimplemented built-in
  palette change in OUTRO frame 10.
- Regular Droid Sans instead of non-embedded Geneva/Monaco, including the
  source's one bold style. Mac Roman text is decoded and wrapped; original
  typography has not been compared pixel for pixel.
- Printed pages use the recovered text and print artwork in an A4 layout;
  the original projector's print layout has not been compared.

Reference behavior was checked against a clean local ScummVM checkout at
`41ac2b31847622d0662d22c03fe6979e3b43cfbc`, especially Director's `frame.cpp`,
`score.cpp`, `stxt.cpp`, sound/video cast members and Lingo services, and
corroborated with the local disc and focused synthetic contracts. This is
implementation-reference agreement; no original-projector comparison has been
completed for this port.

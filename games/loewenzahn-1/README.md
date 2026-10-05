# Löwenzahn 1

An experimental native N64 port of the German disc catalogued as
`sha256-f31970b980f0/LOEWENZA.iso`. The recovered media declares Director 5
(raw version 1217), so this adapter uses the D5 runtime profile.

The self-contained ROM includes all 66 linked MOV files (58 with video),
219 external AIFF files and 109 cast sound references. Video is converted to
160 pixels wide, 15 fps H.264, with audio resampled to 16 kHz and packed as
ULC. The ROM is about 55 MiB and needs an Expansion Pak (8 MiB RDRAM). Its cartridge header retains
the framework's FlashRAM requirement; this game writes no player saves.
Source and generated media remain local and ignored.

## Build and controls

```sh
uv run --locked director64 build --game loewenzahn-1
# After runtime edits, reuse verified generated assets:
uv run --locked director64 build --game loewenzahn-1 --reuse-assets
```

Output: `dist/loewenzahn-1/sha256-f31970b980f0/loewenzahn-1.z64`.
Host conversion also needs FFmpeg/FFprobe and libqrencode for print links. The video converter and N64
libraries come from the project's pinned libdragon container.

- Move the pointer with the stick or D-pad; A clicks or holds a source control.
- All four ports play together, each with its own coloured cursor
  ([shared pointer](../../docs/architecture.md#players-and-cursors)).
- Use the panorama's arrows and objects to explore. The wagon icon returns
  from a topic. The flower opens the original volume/exit dialog; click outside
  it to return to the suspended scene.
- The print stamp in either book opens a QR code for the selected PDF. Scan
  with a phone that can reach the server you host the PDFs on; B returns to
  the book. See [printing.md](printing.md) for PDF export, the configurable
  document address and validation.

## Validation

```sh
mise run check
uv run --locked director64 native --game loewenzahn-1 --sanitizers --journey
uv run --locked director64 native --game loewenzahn-1 --sanitizers --printing
uv run --locked director64 native --game loewenzahn-1 --sanitizers --regressions
uv run --locked director64 native --game loewenzahn-1 --sanitizers --cursors
uv run --locked director64 probe --game loewenzahn-1 CURSORS
uv run --locked director64 capture --game loewenzahn-1 CURSORS
uv run --locked director64 boot --game loewenzahn-1
uv run --locked director64 probe --game loewenzahn-1 HOL
uv run --locked director64 capture --game loewenzahn-1 HOL
uv run --locked director64 probe --game loewenzahn-1 UNK
uv run --locked director64 capture --game loewenzahn-1 UNK
# The same probe/capture commands also accept HAMMER, RADIO and FILMSPEED.
```

Native pointer journeys run the source launcher and intro, open all nine topic
menus (wood, mole, weeds, potatoes, ships, hammer, salt, film, hearing), select
their first topic, and return to the appropriate panorama view. The wood case
also opens and closes DIALOG; the weeds case opens the craft book. These run
with address/undefined-behavior sanitizers. They do not exhaust every activity,
quiz answer, recipe or craft page.

The regression journeys additionally place and hammer a nail, hang a picture,
require its complete reward video, reset the wallpaper, play the kitchen radio
and stop it on navigation, and measure animation advancement at 1, 11 and 24
fps. Their N64 probes assert selected sprite members, sound channels and active
tempo against the native checkpoints. D5 retains the mouse-release target while
a drag handler finishes, so releasing a picture cannot stop its new video.

The dialog journey drags the volume control through mute, midpoint and maximum,
then checks that the chosen volume survives closing and reopening the dialog.
The HOL probe compares all four target sound-channel volumes with the native
checkpoints. Classic `sound i` references resolve to mixer channels.

The 75-second release boot capture requires both intro clips to reach their
full source durations (27.77 and 24.93 seconds), rendered panorama, decoded
video frames and non-silent audio. The runtime converts Director's default
60-unit Lingo video times to the internal 600-unit media clock. Flattened film
loops retain transparent padding, including the panorama's orange flag.
Separate probe ROMs replay only pointer
input, assert native movie checkpoints and require a final return marker.
Release ROMs exclude replay code. Each capture freezes its ROM, ELF, symbols,
command, emulator version, log and media beneath the selected build's
`captures/` directory. `validation.json` records hashes and validation scope.
The ROM receipt also verifies metadata, all manifested packed assets, 23
overlays and 591 compiled handlers from 392 recovered scripts.

## Fidelity and remaining work

Source navigation and scripts execute as native C. D5 additions include
direct script references, ancestor method lookup, actorList callbacks,
mouse-event propagation, sound/video waits, movieTime/rate/stopTime, shared
stopMovie dispatch, cross-movie labels, and suspension/restoration of the
single source dialog window.

The film activity includes one explicit game correction: FIL frame 33's
authored 30 fps command is removed so the source slider's `puppetTempo` controls
the animation. The model and ROM receipt record this difference as
`film-speed-score-correction`; the recovered score remains untouched in
`analysis/`. ScummVM gives the authored tempo command precedence too, so this
correction does not change the shared Director scheduler or claim an
original-projector comparison. Picture-name symbols retain their extensions,
drag constraints use live sprite bounds, and external audio lookup tries an
exact filename before the reference's `.AIF`/`.WAV` extension fallbacks.
Numeric assignments to text fields are converted to strings, including the
film slider's speed display; the native probe checks this renderer contract.

The shared engine renders the seven original monochrome cursors from CURSOR.CXT:
four navigation arrows, Hotspot, Hand and Greif. Cursor selection follows sprite
hit geometry and masks, with a movie default when no sprite supplies an override.
The CURSORS journey covers all seven shapes, Hand→Greif→Hand while dragging the
volume mower, and the stage cursor outside the dialog. Its Gopher64 probe checks
11 source cursor/hotspot checkpoints and all seven composed bitmap hashes against
the native journey. The capture shows the original masks and registration points
in the N64 renderer. This remains implementation-reference agreement; mixed
sprite/default hiding precedence has not been compared with the original projector.

The asset model records these presentation differences:

- Reduced video resolution/frame rate and lossy H.264/ULC conversion.
- Standard OS cursor roles use target glyphs; authored bitmap cursors use their
  recovered image/mask pairs. Executable-resource cursors and Cursor Asset Xtras
  are not implemented.
- Cuts instead of authored visual transitions, and an unimplemented built-in
  palette change in OUTRO frame 10.
- Regular Droid Sans instead of non-embedded Geneva/Monaco, including the
  source's one bold style. Mac Roman text is decoded as UTF-8 and wrapped;
  original typography has not been compared pixel for pixel.
- Printing uses QR links to hosted PDFs with recovered text and print artwork,
  A4 layout and Liberation Sans. Original-projector print layout has not been compared.

Gopher64 captures show alternating lines from the interlaced output path;
the converted source images do not contain those lines. Captures retain RGB
chroma to prevent 4:2:0 sampling of blank scanlines from producing gray frames.
The guest also logs
temporary service-clock backlog during media/scene changes. Neither the
capture speed nor a successful replay establishes hardware performance.

The intro used to run over the service budget: its scripts poll about 144
property reads and 72 calls per tick, and each one walked a dispatch chain of
up to 222 `strcmp` links that the `-Os` target never inlines. Prefiltering
those chains on the first character cut the boot capture's service work per
300-tick window from 5.51 s to 3.41 s, restoring full 60 Hz tick delivery and
dropping its timing overruns from 713 to 21. The panorama is now reached with
about a third of the capture window to spare rather than a few percent.

What remained was the loop itself. The intro's QT-Loop is
`repeat while the movieRate of sprite 2 / updateStage()`, and the source ran
it as fast as the machine allowed; here it consumed the whole 288-step
service budget every tick to re-read a rate that only the tick's own decode
can change. The compiler already yields such a loop at its back edge for
timer, mouse and sound polls, and it now recognizes a linked movie's rate and
time and `the stillDown` as well — the same argument covers a drag loop,
which wants one pass per displayed frame. That took the intro from 3.29 s of
script work per 300-tick window to 0.15 s. Composing the stage only when
something it draws has changed took the intro's render from 1.05 s to 0.64 s
and the panorama's from 2.69 s to 0.84 s per window, and the panorama's
service work from 0.73 s to 0.59 s. The boot capture's remaining dropped time
is entirely the two scene entries.

Those entries wait on image decompression, and this port was the one that
could not buy its way out of it. Giving its images a faster codec made the
intro's second video fail in the decoder — `SLICE_DATA`, `macroblock_layer`
— reproducibly, at the first slice, with the stream's own headers parsed
correctly. Reading the file back through DragonFS at runtime and checksumming
it in 4 KiB blocks against the host's copy found exactly one bad block, and a
hex dump of it held the console's own most recent debug output. The SDK's USB
debug channel keeps its scratch area at `0x04000000 - 8 MiB`, so every line
the game prints is written to cartridge offset 56 MiB with SDRAM writes
enabled — and a faster codec had grown this ROM to 57.5 MiB, putting the
intro's own video under it. Nothing about decompression speed was involved.
The ceiling is now enforced for every port, the sounds are resampled to
16 kHz to pay for it, and everything a scene waits for above 64 KiB
decompresses with the middle codec: image work across the boot fell from
1.09 s to 0.26 s, dropped service time from 1.80 s to 0.11 s, and tick
delivery rose from 0.845 to 0.981.

Reference behavior was checked against a clean local ScummVM checkout at
`41ac2b31847622d0662d22c03fe6979e3b43cfbc`, especially Director's `frame.cpp`,
`score.cpp`, `stxt.cpp`, sound/video cast members and Lingo services, and
corroborated with the local disc and focused synthetic contracts. This is
implementation-reference agreement. No original-projector comparison,
independent-emulator qualification, M64 test or original-N64 hardware test
has been completed for this port.

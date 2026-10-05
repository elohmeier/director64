# Autos bauen mit Willy Werkel

Experimental Director 6 port using the recovered game scripts, score, artwork,
film loops, sound and databases. The build produces a self-contained 25 MiB N64
ROM with FlashRAM saves and requires 8 MiB RAM. This is not a full-game or hardware
qualification.

```sh
uv run --locked director64 build --game willy-werkel-cars
uv run --locked director64 native --game willy-werkel-cars --sanitizers --journey
uv run --locked director64 boot --game willy-werkel-cars
uv run --locked director64 probe --game willy-werkel-cars
uv run --locked director64 capture --game willy-werkel-cars
```

The release ROM is
`dist/willy-werkel-cars/sha256-938be20a50e2/willy-werkel-cars.z64`.
The separate `-probe.z64` exercises controller text entry, workshop entry,
car naming, saving, loading, toolbox navigation and certificate notice dismissal;
use the release ROM for manual play. Build receipts record ROM/DFS hashes,
metadata, every embedded resource and the pinned libdragon toolchain.

## Controls and saves

Move the pointer with the analog stick and click or drag with A. All four ports
play together, each with its own coloured cursor
([shared pointer](../../docs/architecture.md#players-and-cursors)).
While driving, A accelerates, the pointer steers, and B brakes or reverses. Click the original player-name field to open the controller
keyboard: D-pad or stick selects a character, A inserts, B deletes, and Start
accepts the name. The same keyboard edits names in the in-game car save screen.
The keyboard supports up to 20 ASCII characters; the original game handlers
apply their name and pixel-width restrictions when the edit is accepted. Click Willy
to create/select the player; a click skips the introduction once it is ready.

The game's database writes are stored in two independent FlashRAM generations,
with CRC validation and readback after writes. Profiles and car data use bounded
native text records, not serialized pointers. Rebooting restores those records
before the source login handlers run. Invalid saves are not silently erased.
External PC car-file import/export and its separate window remain unsupported.
The certificate's print button shows a dismissible N64 availability notice;
A, B or Start returns to the certificate after releasing the initiating button.

## Source and implementation

The selected local ZIP is `Autos bauen mit Willy Werkel.zip`, SHA-256
`938be20a50e2825f5acf4788aabfdc145b94d1a1fb61b7264a4a81156428afac`.
The recovered media and projector identify Director 6. The source policy selects
`Willy1/MOVIES`, the fresh `Willy1/Data` database casts, and the embedded launcher
from `Willy32.exe`. Root-level duplicate movies are not compiled twice. Unusable
extra `FAHRT.DXR`/`ROUTE*.DXR` files have no references in the recovered scripts.
The root `DATA.CST` contains different existing progress and is not the fresh
profile template used by this port.

All 302 scripts and 1,353 handlers from 30 native units are accounted for against
recovered source/bytecode identities. Two desktop-window handlers carry explicit
traps: opening the external file chooser raises a recoverable unsupported alert
before the original handler would deactivate the save screen, and the close-path
window tell remains fail-closed. The certificate print handler has a third,
source-pinned disposition that shows the platform notice. Conversion accounting
does not imply that every gameplay path runs successfully.

The extended D6 runtime supports multiple attached behaviors and initializer
property lists, resumable `sendSprite`, parent properties, marker destinations,
mutable text cast members, binary topology strings and the game's sound services.
Editable member and sprite properties control keyboard availability. Source
keyDown/idle handlers validate accepted names; hover events run before click
activation, including when a held pointer enters a button. Outgoing endSprite
and stopMovie handlers finish before score replacement or movie unloading,
including callbacks that yield. Erasing dynamic text members releases their
modified-field slots and retained values.
Looping frame scripts allow sprite frame callbacks to finish, and unhandled
frame idle events reach the movie's input poller. Animated cursor sprites do not
intercept buttons or editable text. Inherited methods read properties from the
ancestor that supplies the handler while retaining the original receiver argument.
Numeric string comparisons and numeric Xtra lookup match the game's toolbox and
driving initialization. The recovered `PI` function notation remains a call even
when the handler also declares a local `pi` variable.
Background-transparent shape hit areas stay invisible, allowing the saved-car
preview beneath them to render.
Road topology fields retain their byte values, including zero and 255. Their
concatenation and character access are tested as binary data. A weak symbol cache
and bounded heap sorting reduce database allocation and collection costs.
The shared-cast overlays, including the multi-megabyte database cast, stay
resident across scene changes; their scene tables are generated const. The
renderer loads each frame's missing bitmaps largest-first, compacts the image
cache and font atlases before placing a new full-stage plane, defers a plane
that first appears mid-frame to the next frame's placement pass, and releases
completed images before FlashRAM commits stage their write-and-verify buffers.

ScummVM was inspected at clean revision
`41ac2b31847622d0662d22c03fe6979e3b43cfbc`, principally its versioned score reader,
behavior creation, marker helpers, parent properties and Lingo builtins. Recovered
Willy data and focused tests provide separate evidence. No original projector
comparison has been performed.

## Compatibility choices and limits

Source media remains unchanged. The asset postprocessor pins and reports three
exact data corrections: an unmatched final bracket in `UserNewTemplateDB`, and a
trailing `333` in each of `MulleFreezeAnimChart` and `MulleAnimChart` in `00.CXT`.
They allow strict literal parsing to continue; their behavior in the original
projector has not been verified. The separate desktop chooser dispositions are
pinned to their handler hashes in `host/compatibility.py`, as is the print notice.

Text uses source STXT metrics with Droid Sans as an explicit system-font
substitute; mixed font runs use the first run. Controller name-width checks use
ASCII advances and kerning extracted from the same pinned mkfont build used by
the renderer. Character positions use the documented one-based, member-local
coordinates ([Macromedia scripting reference](https://eclass.hmu.gr/modules/document/file.php/TP194/drmx2004_scripting_ref.pdf)).
The car-save wipe and the shutdown edges-in transitions render as timed
reveals of the new stage using the requested durations (quarter-second floor)
and chunk sizes; unlike the original projector the score keeps playing during
the reveal, and a changing-area request reveals the whole stage. Behavior
initializers that reference the final indexed score record are recovered
through the index's closing fencepost, so every score behavior receives its
authored initializer. The runtime has bounded capacities of
32 dynamic text members, 64 modified text fields and 65,000 saved payload bytes.

Validation currently covers source conversion, source-free C/Python/Node
contracts, sanitized native player creation/workshop entry/reboot login, and
Gopher64 captures of player selection and a controller-created player naming,
saving and loading a car through the original screens, followed by toolbox
navigation, certificate notice dismissal and restored workshop controls.
Captures run at 60 fps
to observe both interlaced fields and check
rendered markers, duration, video frames, representative image color and audio.
The sanitized journey also visits the yard and
junk pile, collects a part, returns to the workshop and drags that part to a new
floor position. It checks held-button hover suppression, the source name-width
limit, car saving, renaming, loading and loading again after reboot. It also checks
the toolbox, certificate notice and restored workshop buttons. A separate driving
fixture uses the recovered `makeCar(#Good)` helper and checks all 16 headings,
acceleration, pointer steering, braking, reverse and input release; it does not
establish that a player can assemble every drivable car. Focused
contracts cover yielding cleanup and 200 dynamic member create/erase cycles.
Randomized input fuzzing drives clicks, drags, idle playback and
controller-keyboard text entry against the sanitized probe. Startup, journey,
controller replay and capture checks reject unexpected script errors even when
the runtime recovers and continues. Agreement with ScummVM does not establish
that an alert occurs in the original projector.
Complete driving routes, all car combinations, missions, external file exchange,
original-projector fidelity and N64 hardware remain unqualified.

The car-save capture now gates guest pacing rather than only recording it. On
the emulator every scene it visits delivers ticks at 1.000 once it has settled;
the only windows below 0.90 are the ones a scene change lands in, never two in
a row and never below 0.80. The gate asserts that, and bounds handle and heap
growth across the run at 1.7x — the run ends in a richer scene than it starts,
so some growth is the scenario rather than a leak.

Three windows used to read far worse — down to 0.497 — because the score was
being held on purpose while the controller keyboard or the certificate notice
was up. Guest wall time passes for those and the score's clock does not, which
reads exactly like a console failing to keep up. The loop counts held service
steps now and the analyzer credits them, so what the number reports is the
console's pacing. Scene entry is still the slowest thing here; nothing in this
establishes hardware pacing, which VI capture throughput alone cannot.

This port paid the most for engine work scoped to Director 7 and later. Its
start screen sits on one score frame running almost no Lingo, yet composited
the whole stage 60 times a second at 12 ms a frame, because the check that
skips an unchanged stage was version-gated; it now renders about once per
five seconds there. The collector below Director 7 ran every 256 allocations
out of an 8,192-handle table and took 47 to 96 ms per pass, so entering the
workshop collected fifty times in five seconds — it now keeps a quarter of the
table spare instead, since there is no in-place emergency pass at this
profile, and collects three times. Method resolution rescanned every handler
the movie declares on each behavior send, and a member search resolved the
requested name once per member rather than once per scan. Together those took
the workshop screen from 88% of the service budget to 30%, and its entry
stall in the boot capture from 7.6 s to 3.5 s.

Instrumenting that entry on the console attributed nine tenths of it to the
interpreter, but not to interpreting: three linear searches of generated
const tables cost 57 to 162 cycles for every entry they stepped over, which
is what streaming a table of 80-byte records past an 8 KiB data cache costs.
Resolving a member by name scanned 605 records on average — 672,508 record
comparisons during the entry, because most lookups miss the primary cast and
fall through to all five, including the 1,131-member database cast. The
member table now carries a folded-name hash index, and that search became
869 comparisons and 48 ms. Asking whether a sprite can receive the mouse
walked every handler its movie declares: 159,588 comparisons per entry, and
256,500 per five seconds of a screen that was merely sitting still. The
answer is a fact about generated code, so it is now remembered per member.
Reading a property walked an object's key/value pairs; it now confirms where
that name was last found before walking again, which halved the pairs
examined. Finally the shared casts, which stay resident for the whole
session anyway, are opened one per idle pass while the boot screens run at a
third of the budget, so entering the workshop expands 38 ms of overlay
instead of 395 ms. The boot capture's dropped service time fell from 3.50 s
to 2.00 s and its tick delivery rose from 0.588 to 0.715; the workshop
screen's steady service cost fell 27%. The price is a brief backlog on the
loading screen, where a shared cast is now expanded.

What the entry then waited on was image decompression. Every asset packed at
maximum compression, which the console expands at under a megabyte a second,
so the workshop's first composite spent 0.86 s on one screen of bitmaps.
Anything a scene waits for — 64 KiB decompressed and up — now uses the middle
codec instead, measured at four times the rate for 26% more ROM bytes
(21.5 to 24.6 MiB). Image work across the boot fell from 0.94 s to 0.27 s,
dropped service time from 2.00 s to 1.36 s, and tick delivery rose to 0.787.

Generated reports and captures live under the selected source's `build` directory.
No ROM, source media or captures are published by the build commands above.

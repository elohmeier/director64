# Autos bauen mit Willy Werkel

Experimental Director 6 port using the recovered game scripts, score, artwork,
film loops, sound and databases. This is not a full-game qualification.

```sh
uv run --locked director64 android build --game willy-werkel-cars
uv run --locked director64 android install --game willy-werkel-cars --serial <device>
uv run --locked director64 web --game willy-werkel-cars --serve
uv run --locked director64 journey --game willy-werkel-cars
```

## Controls and saves

Move the pointer with the stick (or a mouse) and click or drag with A. Up to
four controllers play together, each with its own coloured cursor
([shared pointer](../../docs/architecture.md#players-and-cursors)).
While driving, A accelerates, the pointer steers, and B brakes or reverses.
Click the original player-name field to type a name: Android TV opens its
keyboard over the field, the web app a text box with an on-screen keyboard for
controllers. The same entry edits names in the in-game car save screen. The
original game handlers apply their name and pixel-width restrictions when the
edit is accepted. Click Willy to create/select the player; a click skips the
introduction once it is ready.

The game's database writes are stored in two independent generations of the
save image, with CRC validation. Profiles and car data use bounded text
records, not serialized pointers. A restart restores those records before the
source login handlers run. Invalid saves are not silently erased. External PC
car-file import/export and its separate window remain unsupported. The
certificate's print button shows a dismissible notice that printing is not
available; A, B or Start returns to the certificate after releasing the
initiating button.

## Source and implementation

The selected local ZIP is `Autos bauen mit Willy Werkel.zip`, SHA-256
`938be20a50e2825f5acf4788aabfdc145b94d1a1fb61b7264a4a81156428afac`.
The recovered media and projector identify Director 6. The source policy selects
`Willy1/MOVIES`, the fresh `Willy1/Data` database casts, and the embedded launcher
from `Willy32.exe`. Root-level duplicate movies are not compiled twice. Unusable
extra `FAHRT.DXR`/`ROUTE*.DXR` files have no references in the recovered scripts.
The root `DATA.CST` contains different existing progress and is not the fresh
profile template used by this port.

All 302 scripts and 1,353 handlers are accounted for against recovered
source/bytecode identities. Two desktop-window handlers carry explicit traps:
opening the external file chooser raises a recoverable unsupported alert before
the original handler would deactivate the save screen, and the close-path window
tell remains fail-closed. The certificate print handler has a third,
source-pinned disposition that shows the platform notice. All three are pinned
to their handler hashes in `compiler/src/ports.rs`. Conversion accounting does
not imply that every gameplay path runs successfully.

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
concatenation and character access are tested as binary data.

ScummVM was inspected at clean revision
`41ac2b31847622d0662d22c03fe6979e3b43cfbc`, principally its versioned score reader,
behavior creation, marker helpers, parent properties and Lingo builtins. Recovered
Willy data and focused tests provide separate evidence. No original projector
comparison has been performed.

## Compatibility choices and limits

Source media remains unchanged. The conversion pins and reports three exact
data corrections: an unmatched final bracket in `UserNewTemplateDB`, and a
trailing `333` in each of `MulleFreezeAnimChart` and `MulleAnimChart` in `00.CXT`.
They allow strict literal parsing to continue; their behavior in the original
projector has not been verified.

Text uses source STXT metrics with Droid Sans as an explicit system-font
substitute; mixed font runs use the first run. Name-width checks use advances
and kerning measured with the same pinned mkfont build the converter uses.
Character positions use the documented one-based, member-local
coordinates ([Macromedia scripting reference](https://eclass.hmu.gr/modules/document/file.php/TP194/drmx2004_scripting_ref.pdf)).
The car-save wipe and the shutdown edges-in transitions render as timed
reveals of the new stage using the requested durations (quarter-second floor)
and chunk sizes; unlike the original projector the score keeps playing during
the reveal, and a changing-area request reveals the whole stage. Behavior
initializers that reference the final indexed score record are recovered
through the index's closing fencepost, so every score behavior receives its
authored initializer. The runtime has bounded capacities of
32 dynamic text members, 64 modified text fields and 65,000 saved payload bytes.

The scripted journey (`host/full_journey.py`) creates a player, enters the
workshop, visits the yard and junk pile, collects a part, returns to the
workshop and drags that part to a new floor position. It checks held-button
hover suppression, the source name-width limit, car saving, renaming, loading
and loading again after a restart, the toolbox, the certificate notice and the
restored workshop buttons. Its driving fixture uses the recovered
`makeCar(#Good)` helper and checks all 16 headings, acceleration, pointer
steering, braking, reverse and input release; it does not establish that a
player can assemble every drivable car. Randomized input fuzzing drives clicks,
drags, idle playback and text entry. Agreement with ScummVM does not establish
that an alert occurs in the original projector. Complete driving routes, all
car combinations, missions, external file exchange and original-projector
fidelity remain unqualified.

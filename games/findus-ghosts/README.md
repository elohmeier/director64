# Pettersson und Findus: Die Gespenster-Erschreckmaschine

Experimental Director 8.5 port of the locally supplied German disc, for the
Android TV player and the standalone web app. Director 8.5 runs as the D10
runtime family with extended behaviors; `-DDIRECTOR64_D85` selects the 8.5
conventions (`runtime/lingo/family.h`).

```sh
uv run --locked director64 android build --game findus-ghosts --abi armeabi-v7a
uv run --locked director64 android install --game findus-ghosts --serial <device>
uv run --locked director64 web --game findus-ghosts --serve
```

The launcher embedded in `PFSTART.EXE` converts as `START.DXR`. Its developer
switch (`pfStart.ini` `[custom] hdEnabled=1`, answered by `runtime/game.c`) plays
from the installed layout, which skips the CD search and the `optgraph.dll` copy
check. Scores save as `pfScore.dat` in the save archive.

Each scene streams its speech from its own `DATA/<scene>/SOUNDS` folder, which
Lingo addresses as `@/sounds/NAME`; the port packs them as one `sounds`
namespace. Scripts name scenes by their long Joliet names (`Baumhoehle`); the
runtime resolves them to the 8.3 files and answers the long name as
`the movieName`.

The three `puppetAllSprites` sweeps over flattened film loops are lowered to
nothing (`compiler/src/ports.rs`, pinned by source hash).

Known gaps:

- The Baumhöhle character loops reference loop-local cast library numbers; one
  wrapper loop shows a mouth mask at the lower left.
- `(the stage).image.crop()` answers a blank image, so the broken-egg floor
  shot in the hen house is empty.
- Nothing has been compared against the original projector.

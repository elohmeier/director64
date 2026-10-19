# Pettersson und Findus: Der Torten werfende Geburtstagskater

Experimental Director 8.5 port of the locally supplied German disc, for the
Android TV player and the standalone web app.

```sh
uv run --locked director64 android build --game findus-birthday --abi armeabi-v7a
uv run --locked director64 android install --game findus-birthday --serial <device>
uv run --locked director64 web --game findus-birthday --serve
```

`PF3.EXE`'s launcher (converted as `PF3.DXR`) goes to `Utilities`. The engine
reads its debug switches from the projector's INI; `runtime/game.c` answers it
with `CheckCopyProtection=0`, `CheckCDInsert=0` and `ChangeScreenDim=0`, which
skip the `optgraph.dll` copy check, the CD search and the display mode change.

String tables, room descriptions and game logic come from `SPIEL/XML`, shipped
as read-only data and read through FileIO and the XML Parser Xtra (implemented
over property lists). The engine animates every room by driving placeholder film
loops' channels inside `tell sprite(n)`; the runtime keeps those channels and
the compositor draws them. Saves (`info.dat`, `generalSettings.dat`, `NNN.sav`)
live in the save archive.

Known gaps:

- Text formatting set on run-time text members (font, colour) is not applied.
- Nothing has been compared against the original projector.

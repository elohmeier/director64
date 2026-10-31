# Pettersson und Findus: Die Gespenster-Erschreckmaschine

Experimental Director 8.5 port of the German disc pinned in `game.toml`
(D10 runtime family with `DIRECTOR64_D85`, 800×600 stage). The launcher in
`PFSTART.EXE` converts as `START.DXR`.

```sh
uv run --locked director64 android build --game findus-ghosts
uv run --locked director64 web --game findus-ghosts --serve
```

`[player]` installs `pfStart.ini` with the developer switch that plays from
the installed copy (no CD search, no `optgraph.dll` copy check) and saves
scores as `pfScore.dat`. `[convert]` maps each scene's `SOUNDS` folder into
one `sounds` stream namespace and empties three `puppetAllSprites` sweeps
over film loops the converter flattens.

`visual-cave.json` follows ordinary input into the cave and animates its board
wrapper to check nested mouth masks. `visual-hens.json` starts the hen-house
game and checks the textured floor snapshot produced by
`(the stage).image.crop()`. These browser component checks do not establish
original-projector pixel parity or hardware display qualification.

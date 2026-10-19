# Pettersson und Findus: Der Torten werfende Geburtstagskater

Experimental Director 8.5 port of the German disc pinned in `game.toml`
(D10 runtime family with `DIRECTOR64_D85`, 800×600 stage). The launcher in
`PF3.EXE` converts as `PF3.DXR`.

```sh
uv run --locked director64 android build --game findus-birthday
uv run --locked director64 web --game findus-birthday --serve
```

`[player]` installs the projector's INI with the developers' debug switches
(no `optgraph.dll` copy check, no CD search, no display mode change), saves
`info.dat`, `generalSettings.dat` and `NNN.sav`, and exposes `SPIEL/XML`
(string tables, rooms, game logic) as the read-only data folder, which
`[convert] data` ships.

Known gap: text formatting set on run-time text members (font, colour) is
not applied.

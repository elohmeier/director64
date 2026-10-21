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

The disc installs `FONTS/PETTSON.TTF` from outside the `SPIEL` directory.
`host/source-policy.json` pins its original bytes and the exact `Pettson`
family, so authored labels and the name popup's runtime font request use it.
Omitting this resource left the font pack empty and stopped Android text
rendering when a player name was entered. The popup requests Pettson at
24 points (`ENGINE.CXT`, member 281, `getPopUpData`). ScummVM's text member
font-name lookup is a behavioral reference at commit
`41ac2b31847622d0662d22c03fe6979e3b43cfbc`
(`engines/director/castmember/text.cpp`); no code is copied.

Known gaps: full activity coverage and original-projector/Android TV
comparison of runtime text formatting remain unverified.

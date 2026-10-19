# Findus bei den Mucklas

Experimental Director 8 port of the German disc pinned in `game.toml`. The
embedded projector converts as `START.DXR`; its Pettson PFR1 font is
recovered from the media.

```sh
uv run --locked director64 android build --game findus-mucklas
uv run --locked director64 web --game findus-mucklas --serve
uv run --locked director64 journey --game findus-mucklas --activity
uv run --locked director64 journey --game findus-mucklas full_regressions
```

Clicking the player-name field opens the keyboard and sends every key to the
game's own handlers. In the KR and KB races the first two pads steer
directly and the right stick gives arrow keys (`runtime/controls.c`, switch
`MUCKLAS`).
`[player]` lists the six player files and settings, and installs
`pettson3.ini` with the VI attic activity (absent from this disc) disabled.

`[convert]` fixes BR's two `getAt` calls on a property list to `getaProp`.

Known gaps: full completion of every activity, second-player playthroughs
and double-click behavior are untested.

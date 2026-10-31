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
game's own handlers. KR's single human driver uses the pointer or the
controller's arrow-key steering (`runtime/controls.c`, switch `MUCKLAS`).
The ball game retains its mouse-controlled player and computer opponents.
`[player]` lists the six player files and settings, and installs
`pettson3.ini` with the VI attic activity (absent from this disc) disabled.

`[convert]` fixes BR's two `getAt` calls on a property list to `getaProp`.
Source-pinned patches hide the race and battleship human-opponent controls,
remove their rectangle hit targets, and block the ball game's hidden Shift+2
shortcut. The original single-player rules and AI remain in use; saved player
profiles and historical scores are preserved.

Known gaps: full completion of every activity and double-click behavior are
untested.

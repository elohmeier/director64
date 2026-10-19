# Pettersson und Findus: Hühnertanz und Katerzirkus

Experimental Director 8.5 port of the locally supplied German disc, for the
Android TV player and the standalone web app.

```sh
uv run --locked director64 android build --game findus-circus --abi armeabi-v7a
uv run --locked director64 android install --game findus-circus --serial <device>
uv run --locked director64 web --game findus-circus --serve
```

The projector `PF2.EXE` embeds a launcher (converted as `PF2.DXR`) that starts
the Engine cast's application object. `runtime/game.c` places the projector in
`C:\PF2\`, the developers' layout, so the engine needs no installation,
registry entry or CD drive. The copy check calls `initdisplay` in the disc's
`optgraph.dll` through the GLU32 Xtra; `getCDCode` is lowered to the authored
`Verification` constant (`compiler/src/ports.rs`, pinned by source hash), as a
stub DLL does under the original projector.

Voice lines and effects are cast members linked to `MOVIES\SOUNDS\*.AIF`; the
port converts those files and binds them to their members. The player's name is
typed through the game's own keyDown handlers: on Android TV the keyboard opens
over the name field (`game_key_field`). Scores save as `score.dat`, settings as
`PF2.ini`.

Known gaps:

- The patchwork puzzle builds its patches with imaging Lingo the runtime does
  not compose; the patches show as squares.
- `sound(n).pause()` keeps the sound playing.
- Nothing has been compared against the original projector.

# Pettersson und Findus: Hühnertanz und Katerzirkus

Experimental Director 8.5 port of the German disc pinned in `game.toml`
(D10 runtime family with `DIRECTOR64_D85`, 800×600 stage). The launcher in
`PF2.EXE` converts as `PF2.DXR`.

```sh
uv run --locked director64 android build --game findus-circus
uv run --locked director64 web --game findus-circus --serve
```

`[player]` places the projector in `C:\PF2\` (the developers' layout, so no
installation or CD drive is needed), saves `score.dat` and `PF2.ini`, and
routes typed keys to the login's name field (`key_field`). `[convert]` binds
the linked `MOVIES\SOUNDS\*.AIF` voice files and answers the `getCDCode`
copy check with the authored constant.

Known gaps:

- The patchwork puzzle's patches, built with imaging Lingo, show as squares.
- `sound(n).pause()` keeps the sound playing.

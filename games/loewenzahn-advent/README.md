# Löwenzahn Adventskalender

Experimental Director 8.5 port of the German disc pinned in `game.toml`
(D10 runtime family with `DIRECTOR64_D85`, 800×600 stage). The movies are
Shockwave files; `START` is the entry movie.

```sh
uv run --locked director64 android build --game loewenzahn-advent
uv run --locked director64 web --game loewenzahn-advent --serve
```

`[convert]` binds the DirectMedia Xtra's MPEG-1 movies to their disc files
(converted to VP9) and answers the `getRegOptimal` copy check with its
serial. `[player]` saves the `.usr` preferences file. The calendar follows
the device's date: before December it counts down to the first door.

Known gap: the paint box's file dialogs are not ported.

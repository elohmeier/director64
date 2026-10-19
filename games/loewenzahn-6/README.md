# Löwenzahn 6

Experimental Director 8.5 port of the German disc pinned in `game.toml`
(D10 runtime family with `DIRECTOR64_D85`, 800×600 stage). The movies are
Shockwave files; `START` is the entry movie.

```sh
uv run --locked director64 android build --game loewenzahn-6
uv run --locked director64 web --game loewenzahn-6 --serve
```

`[convert]` binds the DirectMedia Xtra's MPEG-1 movies to their disc files
(converted to VP9) and answers the `getRegOptimal` copy check with its
serial. `[player]` saves the `.usr` preferences file.

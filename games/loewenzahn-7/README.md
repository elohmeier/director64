# Löwenzahn 7

Experimental Director 8.5 port of the German disc pinned in `game.toml`
(D10 runtime family with `DIRECTOR64_D85`, 800×600 stage). The movies are
Shockwave files; `START` is the entry movie.

```sh
uv run --locked director64 repair-media --game loewenzahn-7 --replacement VI_05_01.MPG
uv run --locked director64 android build --game loewenzahn-7
uv run --locked director64 web --game loewenzahn-7 --serve
```

The pinned image has damaged MPEG data. `repair-media` writes a separately
pinned image (recipe in `host/media-repair.json`): it restores `VI_05_01.MPG`
from an independently recovered copy, matched by SHA-256, and holds one
40 ms picture in each of five short clips
([media repairs](../../docs/architecture.md#media-repairs)).

`[convert]` binds the DirectMedia Xtra's MPEG-1 movies to their disc files
(converted to VP9) and answers the `getRegOptimal` copy check with its
serial. `[player]` saves the `.usr` preferences file.

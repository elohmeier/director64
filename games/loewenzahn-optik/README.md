# 25 Jahre Löwenzahn: Laser, Linsen, Flaschenzüge

Experimental Director 8.5 port of the German disc pinned in `game.toml`
(D10 runtime family with `DIRECTOR64_D85`, 800×600 stage). The movies are
Shockwave files; `START` is the entry movie.

```sh
uv run --locked director64 repair-media --game loewenzahn-optik
uv run --locked director64 android build --game loewenzahn-optik
uv run --locked director64 web --game loewenzahn-optik --serve
```

Filesystem journal data overwrites part of one MPEG (`VLUZIFER`) on the
pinned image. `repair-media` writes a separately pinned image (recipe in
`host/media-repair.json`) that holds pictures for 3.3 s and silences 1.2 s
of audio across the damage, keeping the timeline
([media repairs](../../docs/architecture.md#media-repairs)). The missing
content is reconstructed, not restored.

`[convert]` binds the DirectMedia Xtra's MPEG-1 movies to their disc files
(converted to VP9). `[player]` saves the `.usr` preferences file.

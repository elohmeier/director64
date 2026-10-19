# Pettersson und Findus (Findus' workshop)

Supported Director 6 port of the German disc pinned in `game.toml` (an
alternate CloneCD rip of the same pressing is accepted too).

```sh
uv run --locked director64 android build --game findus-workshop
uv run --locked director64 web --game findus-workshop --serve
uv run --locked director64 journey --game findus-workshop routes
```

The port needs no source corrections. `[player]` in `game.toml` lists the
seven player profiles (`vakt1.txt`–`vakt7.txt`) and the construction and
music files the game writes.

Journeys (`routes`, `hubs`, `treasure`, `exit`, `garden`, `profiles`) live in
`tests/full_probe.py`. `host/reference_runner.py` captures the original
projector under Wine (`director64 reference`, see
[architecture](../../docs/architecture.md#original-projector-reference)).

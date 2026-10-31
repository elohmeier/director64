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
serial. `[player]` saves the `.usr` preferences file and image/temp/library cast archives. The calendar follows
the device's date: before December it counts down to the first door.

Verified Joliet stream aliases expose both the authored `SOUNDS/SCHORNSTEIN`
folder and the disc's primary `SOUNDS/SCHORNST` name, including their original
filename spellings.

The runtime implements the recovered paint box's BMP/JPEG import and BMP stage
export through browser and Android document pickers. Native component tests
cover cancellation, exact pixels and durable 560×420 painting-cast saves. The
ordinary input route into `PAINTBOX` is not yet established: the recovered
calendar contains its handler on late score frames that the tested route does
not reach. Component qualification is not a completed in-game painting journey.

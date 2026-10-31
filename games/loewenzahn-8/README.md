# Löwenzahn 8

Experimental Director 8.5 port of the German disc pinned in `game.toml`
(D10 runtime family with `DIRECTOR64_D85`, 800×600 stage). The movies are
Shockwave files; `START` is the entry movie.

```sh
uv run --locked director64 android build --game loewenzahn-8
uv run --locked director64 web --game loewenzahn-8 --serve
```

`[convert]` binds the DirectMedia Xtra's MPEG-1 movies to their disc files
(converted to VP9) and answers the `getRegOptimal` copy check with its
serial. `[player]` saves the `.usr` preferences file, which the game writes
through the vList Xtra.

The `login-dog-registration` journey exercises registration, the save on
return, reboot and restored dog state. The opt-in [invariant model](invariants.json)
adds conditional checks for this named route; see the
[model invocation](../../docs/fuzzing.md#conditional-game-models).

Pet reminders use the authored `Tamagotchi/sounds` folder name even after the
player leaves the activity. The stream pack preserves verified Joliet names
and the disc's primary names, including `TAMAGOTC/SOUNDS`. This also preserves
the underscore spelling of the dog-word and sailing narration filenames;
longer fuzz replays caught both folder and filename lookup failures.

The dog's linked Shockwave animations still need scene-by-scene original-engine
comparison. The registered route establishes entry and saved state.

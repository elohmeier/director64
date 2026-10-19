# Findus Workshop

The completed Director 6 Pettersson und Findus port is Director64's first game
adapter.

```sh
uv run --locked director64 android build --game findus-workshop
uv run --locked director64 android install --game findus-workshop --serial <device>
uv run --locked director64 web --game findus-workshop --serve
```

- Stick or D-pad moves the pointer; A (✕ on a DualShock) is the mouse button.
  A mouse works too.
- Up to four controllers play together, each with its own coloured cursor
  ([shared pointer](../../docs/architecture.md#players-and-cursors)).
- The game provides seven profiles and its construction/music save files.

The adapter owns the Swedish source-global mappings, virtual `vakt1.txt` through
`vakt7.txt`, construction/music files, and the two-generation save journal. Its
host code owns the startup equivalence audit and the original-projector
reference runner (`host/reference_runner.py`, `director64 reference`; see
[reference capture](../../docs/reference-capture.md)).

Preserved runtime behavior includes rational score timing, retained elapsed time,
resumable `updateStage`/wait execution, score ownership of sprite properties,
trail/stage commits, and audio/field cleanup when movies change. These behaviors
are tested in the shared runtime; game-specific assertions live under `tests/`.

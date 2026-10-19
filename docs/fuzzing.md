# Fuzzing, parity and journeys

All three drive the web build's probe
(`dist/<slug>/<source>/director-web-probe`, see [web.md](web.md#probe)):
the same WebAssembly runtime and converted game as the web app, under Node.
Run `director64 web --game <slug>` first.

## Fuzzing

```sh
uv run --locked director64 fuzz --game loewenzahn-1 --duration 900 --jobs 4
uv run --locked director64 fuzz --game findus-workshop --episodes 500 --seed 7
uv run --locked director64 fuzz --game findus-mucklas --replay <case-dir>
```

| Option | Effect |
| --- | --- |
| `--duration S`, `--episodes N` | Stop after S seconds (default 300) or N episodes |
| `--actions N` | Input actions per episode (default 250) |
| `--jobs N`, `--seed N` | Parallel probes; base seed (default random) |
| `--movie NAME` | Entry movie (repeatable) |
| `--prelude FILE` | Scripted opening run after boot (repeatable, one chosen per episode) |
| `--replay CASE` | Re-run a recorded case; exits 1 if it still fails |
| `--no-minimize`, `--output DIR`, `--probe EXE` | Skip minimization; artifact directory; another probe |

Each episode boots the game like the player (entry movie, erased save image)
and performs a seeded random walk of player-reachable input: clicks and
drags (mostly aimed at visible sprites), idle playback, press jitter,
controller samples, text entry into editable fields, and restarts that keep
saves. Accepted inputs are declared in `FEATURES` in
`src/director64/fuzz.py`.

| Kind | Detection |
| --- | --- |
| `script-error` | Error in the probe state, or `NATIVE_FAIL` |
| `script-alert` | `script_errors` increased: a recovered script error, as the projector's alert |
| `crash` | Probe killed by a signal |
| `hang` | No state response within the deadline |
| `protocol` | Unparseable state or unexpected exit |
| `sanitizer` | ASan/UBSan/LSan report, with a sanitizer probe passed via `--probe` |

Failures are deduplicated by a normalized signature. Each new one writes
`case-<hash>-<kind>/` under `build/<game>/<source>/fuzz/<run>/` with
`commands.txt`, `commands.min.txt` (delta-debugged), `report.json` and
`stderr.txt`; `summary.json` has per-signature counts and movie coverage.
The run exits non-zero for any failure other than `script-alert`. Replay is
deterministic. A `script-alert` may still be a port gap: compare it with the
original before deciding.

## Parity

`director64 parity` runs the fuzzer's random walk on two probes and compares
the JSON state after every command. Use it to show that a refactor or
speed-up does not change behavior.

```sh
uv run --locked director64 web --game findus-workshop
cp -r dist/findus-workshop/<source>/web build/findus-workshop/<source>/parity/site-a
# change the converter or runtime, then:
uv run --locked director64 web --game findus-workshop
uv run --locked director64 parity --game findus-workshop --episodes 4 --actions 120
```

Side A defaults to `build/<game>/<source>/parity/site-a` (`--against` names
another web build or probe); side B to the current web build (`--probe`).
Options: `--episodes`, `--actions`, `--seed` (default the fuzz baseline
seed), `--movie`, `--output`. Heap metrics are excluded, instance handles
are renumbered by first appearance, and text compares case-folded. The
summary lists the first diverging command per episode with a field diff.

## Journeys

```sh
uv run --locked director64 journey --game <slug> [NAME]
```

Runs one of the game's scripted journeys (`games/<slug>/host/*journey.py`,
`full_regressions.py`; default `full_journey`) and writes its report under
`build/<game>/<source>/journeys/<name>/`. `--activity` selects Mucklas'
activity journey. The workshop's journeys (`routes`, `hubs`, `treasure`,
`exit`, `garden`, `profiles`) come from `games/findus-workshop/tests/full_probe.py`.

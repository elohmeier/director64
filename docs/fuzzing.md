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
| `--duration S`, `--episodes N` | Stop starting episodes after S seconds (default 300), or finish at most N episodes |
| `--actions N` | Input actions per episode (default 250) |
| `--episode-timeout S` | Bound each episode/replay, including its opening inputs (default 600 seconds) |
| `--jobs N`, `--seed N` | Parallel probes; base seed (default random) |
| `--movie NAME` | Entry movie (repeatable) |
| `--prelude FILE` | Input commands or a journey's `replay.json` (repeatable, one chosen per episode) |
| `--corpus DIR` | Retain/reuse novel input prefixes for this edition; default `output/corpus` |
| `--unguided` | Disable corpus retention/reuse and use independent walks |
| `--model FILE` | Check a versioned, conditional game invariant model |
| `--require-model-coverage` | Fail if any requested model rule was never checked |
| `--strict-alerts` | Fail on recovered script alerts as well as hard failures |
| `--replay CASE` | Re-run a recorded case; exits 1 if it still fails |
| `--no-minimize`, `--output DIR`, `--probe EXE` | Skip minimization; artifact directory; another probe |

Each episode boots the game like the player (entry movie, erased save image)
and performs a seeded random walk of player-reachable input: clicks and
drags (mostly aimed at visible sprites), idle playback, press jitter,
controller samples, text entry into editable fields, ordinary keyboard
events where the runtime supports them, and restarts that keep saves.
Eligible input classes have separate weights, so controller input cannot
starve text entry. Target coordinates use the probe's stage dimensions,
with the selected game policy as a fallback for older builds.

Guided exploration is enabled by default. The runner retains input prefixes
that discover sampled score frames, movie transitions, active handlers,
sprite/member/script sets or save names. Later episodes replay a retained
prefix with 70% probability, then continue exploring. The corpus is bounded
to 128 prefixes of at most 2,048 commands. Each entry records the edition's
source identity and exact inputs; importing another edition's corpus fails.
Input text files and journey replays can seed routes beyond login screens.
Imported inputs allow pointer/controller/keyboard/text input and reboots;
movie jumps, global assignments and other state injection are rejected.
An imported prelude has at most 4,096 commands and one million service ticks.
Journey imports also reject a failed companion report, including dirty
shutdowns whose replay itself recorded no assertion error.

`summary.json` reports input command counts, movie coverage, sampled semantic
coverage, corpus reuse and model checks. These sampled observations do not
measure all executed handlers, branches, activities or visual correctness.
Parallel scheduling and the evolving corpus can change which path a seed
selects. The saved command stream is the replay evidence. Command waits have
deadlines and each episode/replay has a total timeout; an in-flight episode
may finish after `--duration` has elapsed. Probe cleanup terminates its
decoder subprocesses too. Minimization uses at most 120 seconds per case
and stops by the run's duration limit plus 120 seconds.

| Kind | Detection |
| --- | --- |
| `script-error` | Error in probe state, `NATIVE_FAIL`, or the rendered probe's fatal runtime diagnostic |
| `script-alert` | `script_errors` increased: a recovered script error, as the projector's alert |
| `crash` | Probe killed by a signal |
| `hang` | No state response within the deadline |
| `protocol` | Unparseable state or unexpected exit |
| `sanitizer` | ASan/UBSan/LSan report, with a sanitizer probe passed via `--probe` |
| `invariant` | An enabled conditional game expectation failed |

Failures are deduplicated by a normalized signature. Each new one writes
`case-<hash>-<kind>/` under `build/<game>/<source>/fuzz/<run>/` with
`commands.txt`, `commands.min.txt` (delta-debugged), `report.json` and
`stderr.txt`. Failure reports and the summary include source identity,
build metadata and hashes of the selected probe, runtime and harness where
available. Invariant failures retain their model specification too, so
`--replay <case-dir>` restores that model without needing the original file.
Replay rejects a recorded case from another game/source edition when its
identity is available. Plain command files start at the selected game's entry
movie. Replaying a recorded script-alert case exits nonzero when that same
alert recurs, even without the stricter exploration policy.
The run exits nonzero for hard failures or harness errors. Script alerts
remain recorded by default; `--strict-alerts` also gates them. An alert is
not evidence that the game matches the original: compare it before deciding.

### Conditional game models

Models have `schema_version: 1`, an `id`, and named `rules`. Every rule has
an `expect` map using the journey assertion syntax (literal equality,
`gte`, `contains`, `in`, dotted paths and JSON pointer paths). Optional
`when` conditions select the states to check. Optional `after` conditions
arm a rule once reached and remain armed across reboots within that episode.
Each episode/replay starts with a fresh model. The report records the model
hash, checks per rule and unreached rules. A model passing without reaching
its rules provides no evidence; use `--require-model-coverage` for a required
route.

Löwenzahn 8 includes `games/loewenzahn-8/invariants.json` for its registered
REX save/reload route. It arms only after the named pet has been committed
on return to the exterior, then checks the saved name at login and the
restored name in the dog activity:

```sh
uv run --locked director64 journey --game loewenzahn-8 \
  login-dog-registration --output build/journeys/lz8-model
uv run --locked director64 fuzz --game loewenzahn-8 \
  --prelude build/journeys/lz8-model/loewenzahn-8/login-dog-registration/1/replay.json \
  --model games/loewenzahn-8/invariants.json --require-model-coverage \
  --episodes 1 --actions 1 --jobs 1 --seed 7
```

This is an opt-in route model, with the assumption that the pet is not
renamed before the reload checks. It is not a universal rule for arbitrary
profiles. New game rules need their own recovered-data or original-projector
evidence. The source-free tests exercise a generated profile/save/reboot
state machine and verify that the model catches deliberately lost saves.

## Parity

`director64 parity` runs the fuzzer's random walk on two probes and compares
the initial JSON state and the state after every command. Use it to show that a refactor or
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
are renumbered by first appearance. Displayed text, saved files and other
strings compare exactly; the current untyped value format cannot safely
distinguish symbols from strings for case normalization. The summary lists
the first diverging command per episode with a field diff, and each episode
keeps its exact attempted command stream, including a command that failed.
Optional probe observations such as framebuffer hashes also compare exactly.
State parity alone does not establish visual/audio fidelity or correctness
against the original Director engine.

## Journeys

```sh
uv run --locked director64 journey --game <slug> [NAME]
uv run --locked director64 journey --game all --list
mise run test-journeys --output build/journeys/current
```

Defaults to the game's registered suite in `games/<slug>/journeys.json`.
Every playable port has one; `--game all` runs all ports and explicitly
lists catalogued games without ports as unavailable. The suite checks
rendering and recovered script alerts, bounds hangs, retains failed inputs,
and measures command/scene timing and memory. `--baseline DIR` enables
performance regression gates. See [journeys.md](journeys.md) for the coverage
matrix, limits, report layout and baseline workflow.

Existing named scripts (`games/<slug>/host/*journey.py`,
`full_regressions.py`) remain available, including `--activity` for Mucklas'
`full_journey`. The workshop's `routes`, `hubs`, `treasure`, `exit`, `garden`
and `profiles` come from `games/findus-workshop/tests/full_probe.py`.

# Input fuzzing

`director64 fuzz --game <slug>` hunts for crashes and script failures in a
ported game by driving its web build's probe with randomized input. The probe
(`dist/<slug>/<source>/director-web-probe`, written by `director64 web`) runs
the same WebAssembly runtime and converted game as the standalone web player
under Node (`platforms/web/node/probe.mjs`) and speaks a line-based RPC
protocol. Episodes boot the game exactly like the player (entry movie, erased
save image) and inject only events a player could produce, so every reported
failure is reachable in play.

```sh
uv run --locked director64 web --game loewenzahn-1          # builds the probe
uv run --locked director64 fuzz --game loewenzahn-1 --duration 900 --jobs 4
uv run --locked director64 fuzz --game findus-workshop --episodes 500 --seed 7
uv run --locked director64 fuzz --game findus-mucklas --replay build/findus-mucklas/<source>/fuzz/<run>/case-...
```

`--probe` names another probe executable; `--prelude FILE` (repeatable)
gives scripted openings, one chosen per episode and run right after boot, so
the walk can start deep inside a game.

## What an episode does

Each episode is a seeded random walk of `--actions` input actions (default 250)
against a fresh probe process:

- clicks and drags, 80% aimed at visible sprite bounds read back from the probe
  state, 20% at arbitrary screen points;
- idle score playback with the pointer parked somewhere;
- rapid press/release jitter;
- raw `pad` samples through the shared pointer controller (stick, D-pad,
  buttons);
- keyboard text entry: up to 20 characters from the player keyboard's set
  into a currently editable field while the interpreter is idle and no entry
  is draining (Willy Werkel player and car names, Mucklas name entry);
- occasional `reboot`: a restart that keeps persisted saves.

Pointer coordinates stay inside the envelope the shared input controller
clamps to (`runtime/interaction/input.c`, x 8–630, y 8–470); out-of-range
coordinates would report unreachable bugs.

What the probe accepts is declared in `FEATURES` in `src/director64/fuzz.py`
(pad support, button mask, text characters). Director key events have no RPC
on the web probe, so the `keys` table is empty.

## Failure classes

| Kind | Detection |
| --- | --- |
| `script-error` | non-empty `error` in the probe state, or `NATIVE_FAIL` |
| `script-alert` | `script_errors` counter increased: a recovered script error |
| `crash` | probe killed by a signal |
| `hang` | no state response within the per-command deadline |
| `protocol` | unparseable state or unexpected exit (also catches fuzzer bugs and WebAssembly traps) |

The fuzzer also classifies ASan/UBSan/LSan reports as `sanitizer` when a
probe built with sanitizers is passed through `--probe`.

A `script-alert` reproduces the original projector's alert-and-continue: the
runtime discards the Lingo call contexts and the score keeps playing. The
episode continues, the alert is still recorded with its own case artifact,
and only non-alert failures make the run exit non-zero. An alert can still
point at a port gap, so new signatures deserve triage against the original.

Failures are deduplicated by a normalized signature (numbers and addresses
stripped). The first occurrence writes `case-<hash>-<kind>/` into
`build/<game>/<source-id>/fuzz/<run>/` with:

- `commands.txt`: the exact probe command stream that failed;
- `commands.min.txt`: the same failure minimized by bounded delta debugging
  (unless `--no-minimize`);
- `report.json`: signature, seed, entry movie, visited movies, final state;
- `stderr.txt`: probe stderr tail (`NATIVE_TRACE` output, runtime failures).

`summary.json` records per-signature counts and per-movie coverage for the
whole run; the run exits non-zero when any failure other than a recovered
`script-alert` was found.

## Triage

Replay is deterministic: the probe seeds its runtime with a fixed value, so
feeding `commands.min.txt` back always reproduces the same failure.

```sh
uv run --locked director64 fuzz --game <slug> --replay <case-dir>   # exits 1 if it still fails
```

The occurrence count in `summary.json` is a severity hint: a signature hit in
a third of episodes is something players will run into quickly. A
`script-error` is a real defect even when the message looks graceful: it halts
the Lingo runtime. A `script-alert` matches an original script error the
projector recovered from; compare against the original before deciding
whether it needs a fix.

The synthetic contracts in `tests/test_fuzz.py` verify the generator against
a strict protocol double, artifact round-trips, per-seed determinism, and
minimization.

## Scripted journeys

`director64 journey --game <slug> [NAME]` runs one of a game's scripted
journeys (`games/<slug>/host/*journey.py`, `full_regressions.py`; default
`full_journey`) over the same web probe and writes its report under
`build/<game>/<source-id>/journeys/<name>/`. `--activity` selects Mucklas'
activity journey.

## State parity between two probes

`director64 parity --game <slug>` drives the fuzzer's random walk against a
reference probe and replays every command on the working tree's probe,
comparing the JSON state after each command. A change that should not alter
behaviour (a refactor, a speed-up) must reproduce the previous build's states
byte for byte before it claims its gain.

```sh
uv run --locked director64 web --game findus-workshop          # reference build
cp -r dist/findus-workshop/<source>/web build/findus-workshop/<source>/parity/site-a
# ... change the converter or runtime, then rebuild ...
uv run --locked director64 web --game findus-workshop
uv run --locked director64 parity --game findus-workshop --episodes 4 --actions 120
```

Side A defaults to the kept web build `parity/site-a` under the source's
build directory (`--against` names another web build or a probe executable)
and side B to the web build's probe (`--probe` names another). The summary lists the movies visited and the first diverging
command per episode with a field-level diff; a differing outcome (a script
error on one side only) counts as a divergence too. The seed defaults to the
fuzz baseline seed so runs are comparable across changes.

The comparison sees authored state, not the runtime's footprint: the heap
metrics and the live handle count are excluded, and the state renders a
script instance as `<instance N>` with N its allocation serial rather than
its handle. The comparison renumbers those serials by first appearance
in each side's state stream (starting over at a `reboot`), so a change to
value size, collector cadence or what the runtime allocates that leaves
behavior alone leaves the comparison alone, while two globals that share
an instance on one side and not the other still diverge. Text is compared
case-folded, as Lingo compares strings and symbols.

# Input fuzzing

`director64 fuzz --game <slug>` hunts for crashes and script failures in a
ported game by driving its sanitized native probe with randomized input.
Episodes boot the game exactly like the playable ROM (entry movie `START`,
blank FlashRAM) and inject only events a player could produce, so every
reported failure is reachable on hardware.

```sh
uv run --locked director64 fuzz --game loewenzahn-1 --duration 900 --jobs 4
uv run --locked director64 fuzz --game findus-workshop --episodes 500 --seed 7
uv run --locked director64 fuzz --game findus-mucklas --replay build/findus-mucklas/<source>/fuzz/<run>/case-...
```

## What an episode does

Each episode is a seeded random walk of `--actions` input actions (default 250)
against a fresh probe process:

- clicks and drags, 80% aimed at visible sprite bounds read back from the probe
  state, 20% at arbitrary screen points;
- idle score playback with the pointer parked somewhere;
- rapid press/release jitter;
- raw `pad` samples through the shared pointer controller (stick, D-pad,
  buttons, respecting each probe's accepted button mask);
- Director key events on probes that support them, limited to the movies and
  (code, character) pairs the platform layer can produce (Mucklas name entry
  on LO.DXR, pad-synthesized arrows in KR/KB);
- controller-keyboard text entry on probes that support it: up to 20
  characters from the platform keyboard's set into a currently editable
  field while the interpreter is idle and no entry is draining (Willy
  Werkel player and car names);
- occasional `reboot` — a console power cycle that keeps persisted saves.

Pointer coordinates stay inside the hardware envelope from `input.c`
(x 8–630, y 8–470); the shared input controller can never produce anything
outside it, so out-of-range coordinates would report unreachable bugs.

## Failure classes

| Kind | Detection |
| --- | --- |
| `script-error` | non-empty `error` in the probe state, or `NATIVE_FAIL` |
| `script-alert` | `script_errors` counter increased: a recovered script error |
| `sanitizer` | ASan/UBSan/LSan report on stderr (including exit-time leaks) |
| `crash` | probe killed by a signal without a sanitizer report |
| `hang` | no state response within the per-command deadline |
| `protocol` | unparseable state or unexpected exit (also catches fuzzer bugs) |

A `script-alert` reproduces the original projector's alert-and-continue: the
runtime discards the Lingo call contexts and the score keeps playing. The
episode continues, the alert is still recorded with its own case artifact,
and only non-alert failures make the run exit non-zero — an alert can still
point at a port gap, so new signatures deserve triage against the original.

Failures are deduplicated by a normalized signature (numbers and addresses
stripped). The first occurrence writes `case-<hash>-<kind>/` into
`build/<game>/<source-id>/fuzz/<run>/` with:

- `commands.txt` — the exact probe command stream that failed;
- `commands.min.txt` — the same failure minimized by bounded delta debugging
  (unless `--no-minimize`);
- `report.json` — signature, seed, entry movie, visited movies, final state;
- `stderr.txt` — probe stderr tail (sanitizer report, `TRACE` output).

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
`script-error` is a real defect even when the message looks graceful — on the
ROM it halts the Lingo runtime. A `script-alert` matches an original script
error the projector recovered from; compare against the original before
deciding whether it needs a fix.

## Adding a game

The probes speak slightly different RPC dialects; `PROBE_FEATURES` in
`src/director64/fuzz.py` declares what each probe accepts (`pad` support,
button mask, `key` support, `text` keyboard characters). A new port works
immediately with the conservative default (only `step` and `reboot`); add an
entry when its probe supports more. The synthetic contracts in `tests/test_fuzz.py` verify the
generator against a strict protocol double, artifact round-trips, per-seed
determinism, and minimization.

## Performance sweep

`director64 perf --game <slug>` walks reachable states deterministically on
the plain (unsanitized) native probe: the game's scripted journey first, then
a systematic crawl that clicks every live sprite region it has not tried in
each movie and dwells after each transition. The probe accounts CPU time per
movie tick and reports it through the `perf` RPC command, covering the work
since the previous report so a polling caller counts each tick once. The
accounting and the `heap` live-census RPC are shared probe code
(`platforms/native/probe_stats.inc`); a game declares them in
`PROBE_FEATURES` (`src/director64/fuzz.py`) once its `tests/director_probe.c`
includes them — Deutsch 1-2 and Mucklas do today, and the sweep refuses a
probe without them instead of hanging.

The spine comes from the game's `host/full_journey.py` `STEPS`: raw RPC
lines, or `("click", sprite)` entries the sweep resolves from the live
sprite bounds so a spine survives animated layouts (that is how the Mucklas
spine reaches the train station).

`--soak <ticks>` replaces the crawl with a stationarity test: after the
spine, the sweep holds the final movie for the given game time (216000 ticks
is one hour, minutes of wall time on the host), sampling a cost window every
300 ticks and a heap census every ten windows. It fails when the last
quartile of windows costs more than 1.25x the first, or the live handle
count grows the same way — the deterministic reproduction of "the longer it
runs, the slower it gets". The crawl report carries the same early-vs-late
comparison per movie as an advisory `degradation` entry.

Emulator captures measure the other side — rendering, audio and real
pacing — through `director64.pacing`: every game's ROM reports
`NATIVE_TICK`/`NATIVE_COST` windows with a guest wall-clock stamp, live
object counts, GC counters, ticks dropped at the service-clock clamp and
per-channel audio positions, and `pacing.analyze` turns a capture log into
tick-delivery ratios (guest ticks per guest wall second — sustained values
below 1.0 are the slow-motion gameplay and audio desync observed on
hardware), achieved render rates, A/V sync ratios and growth trends.
`assert_pacing` gates a capture on a per-scenario budget once one is
measured.

`NATIVE_COST` also splits render time into `image_us` (cache traffic),
`text_us` (glyphs), `flash_us` (overlay fields, whose glyph time `text_us`
also counts) and `order_us` (the draw-order sort), so a capture says which
phase to optimize rather than only that rendering is slow. Divide by
`renders` for a per-frame figure. `NATIVE_IMAGE_COST` splits the cache
traffic once more — `scan_us` (hit lookup), `open_us` (DragonFS open and
size probe), `headroom_us` (the eviction sweep), `read_us` (ROM read and
decompression), `ink_us` (the per-pixel ink pass) and `heap_us`/`heap_calls`
(what `sys_get_heap_stats` costs, since it walks the whole malloc arena).
Reading those against `NATIVE_CACHE` `loads`/`evictions` is how the image
path was diagnosed: evictions running one-for-one with loads while
`peak_bytes` sits far below the budget means the sweep is taking the live
working set, not that the cache is too small.

`NATIVE_ENTER_COST` splits a scene transition the same way — `drain_us`
(stopping sound, releasing overlays and images, `malloc_trim`), `overlay_us`
over `overlays` (the `dlopen` calls, which `NATIVE_OVERLAY_LOAD` also times
individually as `us=`), `plane_us` (decompressing the opening full-stage
planes), `script_us` (`dg_enter`, which runs the authored prepare/start
handlers) and `probe_us` (the largest-hole diagnostic). Scene entry stalls a
single tick for its whole duration, and the service clock discards backlog
beyond 200 ms, so most of an entry over that becomes dropped game time.

To profile the host side, build a probe with `-pg` using the compile line
from the game's `host/full_host.py`, drive it through its journey spine over
the RPC protocol, and read `gprof`. That is how handler lookup was found to
dominate: `equal_text` under `lv_find` and `named_property_index` held 40%+
of service CPU (the handler table is indexed by the converter now).

Click points come from the probe's sprite bounds and are the movie's authored
coordinates, not the console's physical screen: the ROM scales the pointer
into authored space, so clamping to the hardware envelope would miss every
hotspot near a large stage's right or bottom edge.

Coverage is a graph walk. Each click records where it came to rest, so an
exhausted movie retraces a known exit toward anywhere that still has untried
hotspots. Scores animate, so fresh sprite ids never stop appearing: each
movie gets `--hotspots` clicks before the walk moves on. When nothing
reachable is left, the sweep restarts the probe and replays the journey to
the deepest checkpoint that still has work — the journey's per-command movie
trail supplies those checkpoints — which is how a hub-and-spoke game's rooms
get visited in turn instead of the walk ending in the first one. `--restarts`
bounds the runs.

The report (`work/native/perf/report.json`) lists per-movie mean and peak
service cost, peak Lingo value-heap use, the movie transition edges,
unvisited registry movies, and any probe failures as findings — after a hard
failure the sweep restarts, bans the click that reached the failing state and
continues, so one broken screen does not hide the rest. Movies whose mean
cost exceeds `--budget-us` over at least `--min-ticks` sampled ticks fail the
sweep; the default budget of 12 µs is about 12 ms of the console's 16.7 ms
tick at the measured host-to-N64 scale, leaving the rest for rendering. The
scale is calibrated by pairing, not assumed: the same movies' `NATIVE_COST`
service microseconds from a Gopher64 capture against this sweep's host
means measured ~1000x for the heaviest Deutsch scene (MAINSCR: 11.0 µs host
vs 10,574 µs guest) and 570–700x for lighter ones, so the budget uses the
worst case. Re-pair the two reports after host CPU or compiler changes.
The tick floor keeps boot-chain movies, which tick only for their setup,
from failing on startup cost.

## State parity between two probes

`director64 parity --game <slug>` drives the fuzzer's random walk against a
reference probe and replays every command on the working tree's sanitized
probe, comparing the JSON state after each command. It is the correctness
gate of the roadmap's Track A (see [roadmap.md](roadmap.md)): a milestone
first reproduces the previous milestone's states byte for byte, then claims
its measured gain.

```sh
uv run --locked director64 native --game findus-workshop --build-only   # reference
cp build/findus-workshop/<source>/native/director-probe build/findus-workshop/<source>/parity/probe-a
# ... change the generator or runtime ...
uv run --locked director64 parity --game findus-workshop --episodes 4 --actions 120
```

Side A defaults to `parity/probe-a` under the source's build directory and
side B to the sanitized probe, which `parity` rebuilds unless `--probe` names
one. The summary lists the movies visited and the first diverging command
per episode with a field-level diff; a differing outcome (a script error on
one side only) counts as a divergence too. The seed defaults to the fuzz
baseline seed so runs are comparable across milestones.

The comparison sees authored state, not the runtime's footprint: the heap
metrics and the live handle count are excluded, and the state renders a
script instance as `<instance N>` with N its allocation serial rather than
its handle. The comparison renumbers those serials by first appearance
in each side's state stream (starting over at a `reboot`), so a change to
value size, collector cadence or what the runtime allocates that leaves
behavior alone leaves the comparison alone, while two globals that share
an instance on one side and not the other still diverge.

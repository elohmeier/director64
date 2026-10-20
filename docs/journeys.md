# Game regression journeys

Every supported or experimental port registers a default suite in
`games/<slug>/journeys.json`. CI checks this against the game catalog, so
adding a port without a journey fails the media-free tests. Actual gameplay
runs locally against the owner's web builds and converted disc data.

```sh
uv run --locked director64 journey --game all --list
uv run --locked director64 web --game loewenzahn-6
uv run --locked director64 journey --game loewenzahn-6
mise run test-journeys --output build/journeys/current
```

Build each selected game's web player first. A missing build fails that
game and the suite continues. `--source` selects an edition for one game;
`--game all` uses every game's default edition. A run starts with erased
saves; explicit reboots retain that session's saves and never touch the
browser's or Android player's saves.

## Coverage

These are representative regression paths, not complete-game walkthroughs.
The existing deeper activity and regression scripts remain callable by name.
Coordinates and movie/frame expectations were checked against recovered
local score/Lingo data and rendered probe runs. They are implementation
regressions; none is an original-projector comparison or Android TV result.

| Port | Default journeys | Checked behavior |
| --- | --- | --- |
| Findus workshop | `hubs`, `profiles`, `garden` | Hubs and all 12 constructions; profile saves/reload; repeated visible farmyard animations |
| Willy Werkel cars | `full_journey` | Player creation/reload; part collection and dragging; car save, rename, reboot/load; certificate and print notice |
| Findus Christmas | `full_journey` | All 24 calendar doors and returns; day 16 drag/removal |
| Findus Mucklas | `full_journey` with activity; `train-station-soak` | Player creation/reload; clock victory, inventory reward/reload; ten simulated minutes in the reported train-station slowdown scene |
| Findus ghosts | `player-yard` | Publisher skip, name entry, player save, story to yard, sustained scene playback |
| Findus circus | `player-yard` | Clown selection, keyboard name entry, save, story to yard, sustained scene playback |
| Findus birthday | `player-registration` | Bedroom film loops, name entry, saved registration, room entry and sustained playback |
| Löwenzahn 1 | `full_journey` | Both complete intro clips, volume slider and retained settings, all nine menus/first topics and panorama returns |
| Löwenzahn 6 | `login-frog-video` | Profile selection, preferences, exterior navigation and frog video playback |
| Löwenzahn 7 | `login-piano` | Music profile, roof navigation, piano entry and key input |
| Löwenzahn 8 | `login-dog-registration` | Boat profile, intro skip, pet name entry, return/save, reboot and restored dog state |
| Löwenzahn Advent | `countdown-wagon-desk` | Seasonal start, countdown exit, desk navigation and nutcracker interaction |
| Löwenzahn Optik | `wagon-laptop-topic` | Intro, desk, laptop, first topic menu, quiz entry and sustained playback |
| Lernerfolg Deutsch 1–2 | `full_journey` | Name/class selection, saved profile, room and exercise entry, Flash prompt layout and answer selection |

The six additional experimental adapters register these routes:

| Port | Journey | Route |
| --- | --- | --- |
| Findus garden | `guardian-garden` | Launcher, guardian selection, save generations, garden playback |
| Fritz & Fertig | `opening-player-screen` | Player folder discovery, name entry, story entry, save/reboot and profile resume |
| Löwenzahn 2 | `intro-wagon` | Launcher and QuickTime introduction to the wagon |
| Löwenzahn 3 | `opening-login` | Startup checks, QuickTime cast selection, profile save and wagon bedroom |
| Löwenzahn 4 | `rocket-login` | Rocket intro, skeleton profile, saved preferences and wagon exterior |
| Löwenzahn 5 | `opening-login` | Device checks, snail profile, opening story skip and wagon exterior |

The suite registers 23 default journeys across 20 games. The compatibility
workarounds and conversion limits are documented in
[additional-ports.md](additional-ports.md). Each route includes concrete
state expectations; startup alone does not establish activity coverage.

## Correctness and failure evidence

Every suite uses the rendered probe: intermediate frames inside a multi-tick
command render too. Runtime/compositor errors, recovered script alerts,
unexpected exits, failed checkpoints and timeouts fail the journey. Unlike
fuzzing's separate alert policy, journeys do not tolerate script alerts.
Capability failures remain failures; the runner has no expected-failure
allowlist. Pointer, keyboard, editable text, pad input and save-preserving
reboots are allowed; arbitrary movie jumps or script/state injection are not.

`--timeout` bounds each journey including all its probe sessions (default
600 seconds); timeout termination includes the process group. The runner
continues with other games and exits nonzero if any journey fails.

The output directory contains `summary.json` and
`<slug>/<journey>/<repetition>/report.json`, worker/probe logs, the journey's
replay/checkpoints, and per-session metrics. Each metrics session has a
`.commands.txt` file written **before** executing each command, preserving
the input that hangs or crashes, plus completed `.jsonl` measurements.
Reports identify the source SHA-256, selected repaired image where relevant,
build/package/runtime identity, plan hash, harness hashes and host environment.
Outputs stay under ignored `build/`; do not commit or distribute game traces.

Without `--output`, one game writes under its source's `journeys/suite/`;
all games write under `build/journeys/suite/`. Runs overwrite the selected
output, so choose distinct directories when preserving evidence or baselines.

## Performance gates

A run without a baseline records measurements and marks its performance
gate `unbaselined`. Its passing status establishes only the configured
correctness checks. To compare a passing port before and after a change:

```sh
uv run --locked director64 journey --game findus-workshop \
  --repeat 3 --output build/journeys/before
# Make the change and rebuild that game's web player, then:
uv run --locked director64 journey --game findus-workshop \
  --repeat 3 --baseline build/journeys/before --output build/journeys/after
```

Repeated runs compare median timings across independent cold processes and
the highest memory measurement across every repetition. Each repetition must
pass correctness and use the same edition, build, plan, harness, environment
and command stream. Individual reports and timing ranges remain available;
`median/report.json` contains the aggregate. Repeated baseline comparisons need at
least three repetitions and the same repetition count. A single run still
compares its individual report.

The default gate rejects more than 25% growth in total measured runtime,
boot time, command p95/maximum, or an individual scene's measured time, and more
than 10% growth in peak process RSS or Wasm memory capacity.
`--max-slowdown` and `--max-memory-growth` change these ratios (both at
least 1). A missing/failed baseline, different edition or repair, changed
journey, different host/Node environment or harness, or different command streams
cannot establish a passing comparison. Scene command counts must match.
Advent also requires the same local calendar date, since it takes seasonal
paths. No date or gameplay state is injected to bypass its calendar.

Measurements include rendering, asset loading and synchronous video decode
waits. Boot includes runtime/package startup. Per-command wall time and
Node CPU time are recorded; timing is attributed to the scene answering the
command. RSS includes Node and loaded packs; Wasm capacity is separate.
Decoder child-process CPU and RSS are not included in Node CPU/RSS.
Command p95 is **not frame time or FPS**: commands can advance different
numbers of ticks. Keep other host workloads quiet when comparing performance.

## Findings and remaining qualification

The 20 October 2026 host validation completed conversion for all 20 default
editions, then ran all 23 journeys three times for a baseline and three times
for a comparison: 138 passing rendered runs. All 23 median timing/peak-memory
comparisons passed the unchanged gates. This checks repeatability of the
current builds and establishes future baselines; it does not measure an
improvement against the previously failing builds. The full source-free
repository checks also pass. Media and generated evidence stay local.

The October 2026 audit found and fixed the registration, text, movie-path,
film-channel, FileXtra, palette-transition, stream-packaging and source-script
blockers exercised by these routes. Birthday now enters a room after saving;
the piano guard handles free play's empty task list; the dog name is stored as
text and restored after reboot; Lernerfolg enters an exercise and selects a
visible answer. The six new adapters convert and run their registered routes.

Best-effort substitutions remain explicit in conversion reports or runtime
traces. Palette fades use truecolor scene snapshots through black/white with
approximate timing. Text sizing/alignment work, while unavailable font faces
and fixed/adjust box differences retain the bounded native layout. The damaged
Löwenzahn 5 video uses a pinned packet discard and decoder concealment. Missing
Löwenzahn 6 narration is skipped by one pinned source patch. None of these is
claimed as recovered original output.

Rendered execution catches compositor errors but cannot prove pixel or
audible output matches the original. In particular, the linked dog animations,
Circus imaging puzzle, Ghosts masks/crop, Birthday text styles and palette fade
timing still need visual comparisons. Packaged streams now use their converted
audio durations instead of the probe's missing-stream fallback. The routes do
not establish completion of every activity or every save/reward path; the
matrix states their actual scope.

For Android TV qualification, follow each registered route on the target
device with diagnostics enabled, test the controller and text-entry path,
return/reenter its activity repeatedly, and keep a longer session running
in its busiest scene. Record service delivery, render costs, dropped time,
CPU/RSS and script alerts before/after the repeated visits, then restart and
verify the saves exercised by the route. A web benchmark cannot qualify TV
pacing, device memory, sound output, controller mapping or suspend/resume;
see [Android pacing diagnostics](android.md#pacing). Original-projector and
hardware qualification flags remain false in automated host reports.

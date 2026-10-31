# Detecting port regressions

Use several independent checks. A clean script state does not establish a correct
picture or audible output; matching two port builds does not establish fidelity
to the original. Android emulator and physical-device results remain separate.

Game-dependent recordings, screenshots, PCM, original-engine outputs, corpora and
baselines stay under ignored `build/`. Commit the synthetic tests and authored
input plans, never retail assets or links to captures. None of these commands
publishes artifacts or connects a projector automatically.

## Source-free checks

```sh
mise run check
mise run test-sanitize
mise run test-mutations
mise run fuzz-native                 # bounded 15-second synthetic corpus run
FUZZ_SECONDS=120 mise run fuzz-native
npx playwright install chromium      # once, after npm ci
mise run test-browser
```

`test-sanitize` checks native rendering, film/Flash ownership and lifecycle with
ASan/UBSan. `fuzz-native` instruments package loading and image decoding with
libFuzzer, ASan and UBSan. Its seeds are generated synthetic packages/images.
Crashes retain the reproducer under `build/validation/native-fuzz/`.

`test-mutations` compiles isolated source copies with deliberately broken stage
bounds, scene-cache invalidation and popup blend ownership. The unmodified
control must pass and each faulty copy must fail. The working tree is unchanged.

`test-browser` loads a real font in Chromium, captures moving glyph pixels and
checks the actual Web Audio mix against a deliberately muted signal. It uses
synthetic graphics and a generated oscillator, without game data. Public CI runs
these checks alongside the Python, Node, Rust and native contracts.

## Gameplay and exploration

```sh
mise run build-web-all                # local discs required; logs each game
mise run test-journeys --output build/validation/journeys
uv run --locked director64 fuzz --game findus-birthday \
  --episodes 20 --seed 7 --strict-alerts
```

Builds record hashes of the actual runtime/player inputs and reject source changes
during compilation. Browser captures require a matching receipt, package, Wasm,
player and edition; a stale build is a failure. Conversions of the same edition
are serialized across the web, Android and convert entrypoints.

The fuzzer mixes pointer, dragging, controller, keyboard and editable-text input.
It uses the actual stage dimensions and retains bounded prefixes that discover
new sampled state. Use successful journey `replay.json` files as `--prelude` to
reach activities beyond registration. Its sampled state coverage is not complete
activity or branch coverage. Conditional gameplay invariants, source identity,
bounded replays and corpus controls are described in [fuzzing.md](fuzzing.md).

A longer Löwenzahn 8 replay found a pet reminder requesting the original Joliet
folder name after the player left the activity. The package only exposed its
shortened disc name. Both names are now packaged; source review found and fixed
the same mismatch for Löwenzahn 7 composer clips and Advent chimney cues.
Synthetic packing tests check all three folder pairs against the real manifests.

## Browser images, sequences and PCM

Five authored plans live in `games/<slug>/visual.json`: Birthday, Ghosts, Circus,
Löwenzahn 8 and Löwenzahn 4. They reuse reachable journey input. Birthday observes
typed-name rendering, Löwenzahn 8 observes the dog scene, and Löwenzahn 4 samples
the wagon transition inside its original settle interval. Entering Circus's yard
does not establish coverage of its imaging puzzle.

```sh
uv run --locked director64 visual --game findus-birthday capture \
  --output build/validation/browser/birthday-before
```

The default uses controlled 60 Hz service, real fonts and host video/audio
decoders. The production RAF loop is disabled explicitly for validation, and
each run uses an isolated browser/save context. `--fast` accelerates route
traversal; bounded observation windows still use real time. These are controlled
replays, not production-browser pacing qualification. A late service clock or a
late asynchronous decoder error is retained as evidence. Host decode failures
fail the recording; cold host timing failures have a separate `timing_status`
and cannot become a production-pacing pass. A timing backlog never triggers
unbounded catch-up.

`--checkpoint LABEL` stops after that reachable checkpoint. `--plan FILE` selects
another input-only plan. Observations can name image regions, motion/blank-frame
rules and audio signal requirements. An `observe` annotation samples inside an
existing input interval without adding ticks. Requested checkpoints, frame counts
and audio must actually be present. A frozen scene is an error only where the
plan requires motion.

Recordings contain stage images, optional RGBA, PCM, exact inputs, states and
source/build/environment provenance. Differences report affected regions and
generate highlighted images. Regional tolerances avoid letting a small missing
control disappear inside a whole-screen score. Audio checks cover silence,
duration, envelope and normalized frequency content, with explicit tolerances.
Ghosts' default plan treats its unanchored narration sample as exploratory:
the PCM remains available, but content comparison stays `unbaselined` while
the image/sequence comparison can pass. Content-sensitive audio remains the
default for the other configured cases. Artifact encoding/transport pauses
test service and its AudioContext together, outside the timed sample window.

A capture without an approved baseline is **unbaselined**, not a fidelity pass.
Inspect the images and sound before approving a new baseline:

```sh
uv run --locked director64 visual --game findus-birthday approve \
  --recording build/validation/browser/birthday-before \
  --baseline build/validation/baselines/birthday-v1 --reviewed-by NAME

uv run --locked director64 visual --game findus-birthday capture \
  --baseline build/validation/baselines/birthday-v1 \
  --output build/validation/browser/birthday-after
```

Baselines are immutable directories; choose a new name for a reviewed change.
A port recording cannot be relabelled as original-engine evidence. Comparing
two recordings of the same port catches regressions while preserving any
inaccuracies both share.

Image inspection exposed a white band beneath Löwenzahn 4's nighttime stars.
The recovered score selects Blend ink for an 8-bit bitmap whose stored exterior
matte correctly cuts out that area. The Director 7 decoder incorrectly made the
matte opaque. It now preserves multi-bit Blend coverage while keeping one-bit
Blend and full-opacity Copy rectangular. The source-free
[pixel regression](../tests/native/test_d7_blend_matte.c) also checks opaque white
interiors and partial blending; deliberately restoring the old decoder fails it.

This correction is corroborated by recovered bitmap/score facts and the clean
ScummVM revision `41ac2b31847622d0662d22c03fe6979e3b43cfbc` (`channel.cpp`,
`getMask`), consulted for behavior without copying code. The original Director 7
nighttime comparison remains unqualified behind the optical check described
below; agreement with this reference is separate evidence.

## Equivalent execution checks

```sh
uv run --locked director64 metamorphic --game findus-birthday \
  --replay build/validation/journeys/findus-birthday/player-registration/1/replay.json \
  --relation all --timeout 600 --output build/validation/equivalence/birthday
```

`cache` compares retained scene/derived-plane reuse with recomposition.
Immutable decoded assets remain cached in both runs. `chunks` compares a batched
held input with one service tick at a time, preserving key edges. Both compare
state, ordered audio host callbacks, every serviced frame's 64-bit checksum and
the final RGBA SHA-256. They retain the first divergence and inputs even if later
shutdown fails. Instance identities are canonicalized at shared observation
boundaries so extra intermediate observations do not create false differences.

These are Node compositor checks: nominal glyphs and audio callbacks do not
establish actual font or audible-output equivalence. Sessions have finite total
timeouts and bounded input streams; a slow uncached reference can need a longer
timeout. A timeout is a failure, not an accepted comparison.

## Original Director evidence

```sh
mise run reference-toolchain
uv run --locked director64 reference --game findus-birthday \
  --windows-version win7 --experiment ink-precision
uv run --locked director64 reference --game findus-birthday \
  --windows-version win7 --experiment list-relations
uv run --locked director64 reference --game findus-birthday \
  --windows-version win7 --experiment list-relations-extra
uv run --locked director64 reference --game findus-birthday \
  --windows-version win7 --experiment stream-empty
uv run --locked director64 reference --game findus-birthday \
  --windows-version win7 --experiment stream-empty-control
```

The runner installs a private copy, restores verified Joliet aliases and applies
the manifest's original-player settings using Windows-style case-insensitive
names. Normal captures use a bounded timestamped pointer/key replay. Synthetic
experiments run authored Lingo through `LINGO.INI` and are explicitly marked;
they cannot become normal gameplay image baselines.

`ink-precision` compares the exact 448-input synthetic grid with our arithmetic
implementation. Its comparator requires completed output and observed Director
8.5 playback DLLs. The list probes record scalar/list, property-list and nested
ordering; source-free native tests encode their independently measured results.
Other probes record output without claiming an automatic comparison unless a
comparator is registered.

Record the loaded playback DLL hashes/versions as well as the launcher. The
Birthday experiments load DIRAPI/IML32 **8.5.1r104** although PF3.EXE advertises
9.0r383 and PROJ.DLL 9.0r371. These are original-engine results **under Wine**,
not native Windows validation.

The list-ordering measurements disagreed with the previous implementation and
ScummVM's `compareArrays` at clean commit
`41ac2b31847622d0662d22c03fe6979e3b43cfbc`. Recovered Birthday bytecode required
`freeChannels > 0` to allocate a sound channel. Original-engine measurements
establish first-difference ordering and scalar broadcasting for this 8.5 profile;
the correction is deliberately version-scoped. No ScummVM code is copied.

The stream probes generate a synthetic ten-second PCM WAV and call `sound
playFile` with an empty string, existing/absent directory paths, and a missing
named file. The control probe observes `soundBusy(1)` after an explicit stop,
valid playback, the invalid request, and another stop: **0, 1, 1, 0** for every
case. Thus these invalid requests leave existing playback running in original
8.5.1r104 under Wine; they do not stop the channel as ScummVM's `playFile` does
at the revision above. The 8.5 runtime now ignores an empty filename or a path
ending in a directory separator, including LZ8's repeated-heading request
`"@:Sounds:" & EMPTY`. Missing named packaged streams still fail with a useful
diagnostic so incomplete conversion cannot silently pass. Native tests exercise
active and idle channels, preserved playback state, and named-asset failures
with both virtual and fallback path resolution (`mise run test-native`).

Stream conversion also preserves the disc's verified Joliet filenames and
folder names alongside its primary ISO names, including mixed long/short names
at corresponding directory and filename components. Component choices are
deduplicated without case before expansion; more than 256 variants for one
selected stream fail conversion with a diagnostic. Aliases are admitted only for
the actual source files accepted by the selected stream folders and duplicate
preference; unrelated files cannot donate an alias. Conflicting targets fail
conversion. Alias index entries share the primary encoded byte range and
duration. Paths can mix verified short and long spellings at corresponding
components; expansion is bounded before generating variants. Synthetic
dual-namespace ISO tests exercise underscore spellings, mixed directories and
filenames, folder boundaries, duplicate preference, and shared pack ranges.

For original gameplay image comparison, `visual import-original` verifies the
original capture/source/replay/PNG/module hashes and requires a reviewed explicit
mapping to port checkpoints. See its CLI options. Only mapped regions and
checkpoints receive comparison evidence; successful media capture alone does
not establish equivalence or game completion.

Reference recipes can request a private optical drive with
`"optical_drive": {"letter": "D", "raw_image": false}`. Its Joliet aliases and
volume label come from the pinned disc. The serial is Wine 8's checksum of the
selected ISO descriptor, with the configured Windows version's byte order; it
is not a substituted copy-protection return value. The namespace is mounted
read-only, with Wine's optical type configured under
`HKLM\Software\Wine\Drives`. Optional `raw_image: true` exposes the unchanged
original cooked ISO read-only through `d::`; Wine 8 regular-file raw backing can
make volume-information queries fail, so Löwenzahn 4 uses the folder-backed drive.
These facilities follow [Wine 8's mount-manager behavior](https://github.com/wine-mirror/wine/blob/wine-8.0/dlls/mountmgr.sys/device.c).
The container uses an init process to reap Wine descendants rather than exhausting
its process budget with zombies.

Löwenzahn 4 now gets past its initial insert-CD dialog with verified Win32
observations: optical drive type 5, label `LOEWE4`, serial `7b6951f9` and filesystem
`CDFS`. The unchanged original then reaches `CHECKW`'s `CopyProtect` screen:
member 11 requires `getRegOptimal()` to return the string `13665984`. That handler
calls the original `OPTGRAPH.DLL` through `GLU32`; its failing return/loader status
has not been observed. No retail protection check was patched. The original
night-transition comparison remains unqualified behind this barrier.

The optional source-free Win32 drive probe records API results and raw-read
availability in `C:\director64-drive-probe.txt` inside a private Wine prefix.
It can be built without Windows SDK code using LLVM tools:

```sh
mkdir -p build/reference-drive-probe
llvm-dlltool -m i386 --kill-at -d tools/reference/drive-probe.def \
  -l build/reference-drive-probe/kernel32.lib
clang --target=i686-pc-windows-msvc -O1 -ffreestanding -fno-builtin -fno-stack-protector \
  -c tools/reference/drive-probe.c -o build/reference-drive-probe/drive-probe.obj
lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:x86 \
  /out:build/reference-drive-probe/drive-probe.exe \
  build/reference-drive-probe/drive-probe.obj build/reference-drive-probe/kernel32.lib
```

Run this generated executable only in the isolated reference environment. It
observes drive APIs and writes its own result file; it does not modify the game.

## Android and bug reports

Use [the Android validation APK](android.md#automated-android-journeys) to replay
the actual keyboard, pointer and remote paths in an isolated emulator app. It
has a separate application identity and saves. Read-only snapshots, normalized
stage screenshots, animation samples, logcat and pacing windows are retained.
AAudio callback signal checks distinguish silent/missing output from active PCM;
they do not validate speakers. Menu, Home/return and restart are explicit inputs.
Run the finished changes on the projector separately when it is available.

For a manual discovery, retain the exact edition/build, ordered input prefix,
checkpoint and recent image/audio/log evidence locally. Minimize the input and
add an assertion that fails before the fix. Validate the fix in the cheapest
relevant layer, then repeat the affected real browser/APK route. Never replace a
failed reference with newly generated output simply to make a test pass.

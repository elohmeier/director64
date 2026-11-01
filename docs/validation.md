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
mise run fuzz-native                 # bounded 15 seconds per D6/D8.5 corpus
FUZZ_SECONDS=120 mise run fuzz-native
npx playwright install chromium      # once, after npm ci
mise run test-browser
```

`test-sanitize` checks native rendering, film/Flash ownership and lifecycle with
ASan/UBSan. `fuzz-native` instruments package loading and image decoding with
libFuzzer, ASan and UBSan. Its seeds are generated synthetic packages/images.
Separate D6 and D8.5 runs exercise both package layouts. The modern seed comes
from the real Rust writer/C loader fixture and includes vector, transition and
rich-text sections, so those readers start with valid structured inputs.
Crashes retain the reproducer under `build/validation/native-fuzz/`.

`test-mutations` compiles isolated source copies with deliberately broken stage
bounds, scene-cache invalidation, popup blend ownership, film clipping, idle
cadence and hidden inverse-mapping offenders. The
unmodified control must pass and each faulty copy must fail. The working tree is
unchanged.

`test-browser` loads a real font in Chromium, captures moving glyph pixels and
checks the actual Web Audio mix against a deliberately muted signal. It uses
synthetic graphics and a generated oscillator, without game data. Public CI runs
these checks alongside the Python, Node, Rust and native contracts.

## Gameplay and exploration

```sh
mise run build-web-all                # local discs required; final source/artifact receipts
mise run build-android-all            # production APKs, all 3 ABIs; no device install
mise run test-journeys --output build/validation/journeys
uv run --locked director64 fuzz --game findus-birthday \
  --episodes 20 --seed 7 --strict-alerts
```

Builds record hashes of the actual runtime/player inputs and reject source changes
during compilation. Browser captures require a matching receipt, package, Wasm,
player and edition; a stale build is a failure. Conversions of the same edition
are serialized across the web, Android and convert entrypoints. The two full
build tasks create timestamped reports below ignored `build/validation/`;
`--output` chooses another ignored build directory. They recheck all completed
artifacts against the final tree, so a source edit during another port's build
cannot leave an older artifact marked passing.

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

## Performance without a projector

```sh
mise run test-performance
mise run test-performance --no-build --output build/validation/performance/current
# Optional regression comparison: use a previous run on this same host.
mise run test-performance --baseline build/validation/performance/previous
```

This builds all 20 local ports and runs every registered rendered journey with
Android's 96 MiB image-cache budget. It rejects missing games, missing counters,
state-only runs, stale builds and changes to the harness or input plans during a
run. Reports retain the movie, frame, channel and member responsible for wasted
inverse mapping, plus the most expensive host-rendered frames. Local discs are
required; no device is contacted.

Deterministic work gates catch algorithmic regressions even when a desktop is
fast: over 16 stage equivalents of bitmap scans per redraw, over eight inverse
mapping equivalents, or an individual inverse draw scanning more than four times
its sampled pixels while wasting over a quarter of the stage. Cache churn fails
after three consecutive scene redraws with over 32 capacity evictions each.
Normal replacement and one-time cold-scene turnover remain visible in the report
without being mistaken for sustained cache thrashing. Host timings rank hotspots;
the optional baseline gate compares time and memory on the same host. Neither
predicts Android frame rates.

The work protocol retains the largest rejected area separately from the largest
rejected area among draws exceeding the scan-ratio limit. Otherwise a legitimate
large crop can hide a smaller, very inefficient draw. Protocol version 2 records
the runtime's ratio threshold; stale counters or a different evaluator policy
fail explicitly. A synthetic mixed-draw case and its faulty mutation check this
false-pass regression independently of host timing.

Ghosts' ordinary `rainy-opening` route waits through the publisher reel and then
sustains the login scene with rain and all four ghosts. The former renderer
inverse-mapped each small child over its large film container. Integer translation
stacks now restrict scanning to the translated child bounds, intersected with
every ancestor viewport, while preserving the original mask, ink and sampling
arithmetic. Other transforms retain the general path. Synthetic pixel tests
compare both paths across inks, masks, stage edges, nesting and transform
fallbacks; the film-clipping mutation must fail the work oracle even with correct
pixels. `--relation clip` below also compares both paths on real game inputs.

These checks cover the registered routes and renderer costs. Extend journeys to
reach newly discovered problem scenes. Node uses nominal glyphs and records audio
callbacks; browser visual/audio tests and eventual TV pacing checks provide
separate evidence.

## Idle scheduling

`idleHandlerPeriod` belongs to the movie runtime, including independent movie
windows. Positive values space idle dispatches in 60 Hz ticks; changing a period
does not restart a suspended handler. Zero and negative values consume available
idle time after ordinary frame/input work, bounded by the existing service budget.
The scheduler does not invent catch-up idle events after a long handler. Source-free
tests cover periods 1/2/6/60, within-tick polling, changes during playback,
wraparound, yielding, frame progress, fresh boots and window isolation.

Ghosts' scene/idle objects and Birthday's event manager explicitly request 30 idle
updates per second. Their setters previously only stored a process-global value,
so the scheduling path ignored it and movie windows shared it. The selected
original 8.5.1r104 engine under Wine confirms default 1, independent window
readback, positive-period throttling and unrestricted zero/negative periods.
Its launcher advertises 9.0r383; the observed DIRAPI/IML32 DLLs identify playback
as 8.5.1r104. Cold first-phase timing is affected by original-game startup, so
the measurements are behavioral evidence rather than an FPS benchmark.

```sh
uv run --locked director64 reference --game findus-birthday \
  --windows-version win7 --experiment idle-handler-values
uv run --locked director64 reference --game findus-birthday \
  --windows-version win7 --experiment idle-handler-period --replay REPLAY
```

For the cadence experiment use a host-monotonic replay of at least 22 seconds
with a checkpoint near the end. The authored probes contain the input cases and
expected behavior; capture reports retain engine module hashes and the Wine
environment. This is original-projector evidence under Wine, not native Windows
or Android TV qualification. Macromedia's
[Director MX 2004 Scripting Reference, idleHandlerPeriod](https://eclass.hmu.gr/modules/document/file.php/TP194/drmx2004_scripting_ref.pdf)
documents positive tick intervals, default 1 and unrestricted zero. The selected
D6 full-engine profile retains its older zero default; modern profiles use 1.
The `idle-handler-defaults` probe also confirms default 1 and integer readback
in the selected Director 7.0.2r85 playback DLLs under Wine. Newly allocated movie
windows and linked movie contexts initialize their own defaults too.
The clean ScummVM reference at `41ac2b31847622d0662d22c03fe6979e3b43cfbc`
provides keyword/event context, but its idle-period scheduling is itself
unfinished; no implementation was copied from it.

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
held input with one service tick at a time, preserving key edges. `clip` compares
tight translated-film bounds with the original full-container traversal. All
compare state, ordered audio host callbacks, every serviced frame's 64-bit checksum and
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

Löwenzahn 4's disc discovery works with optical drive type 5, label `LOEWE4`,
serial `7b6951f9` and filesystem `CDFS`. Its separate `OPTGRAPH:initdisplay`
protection check still returns **5** through both a native probe and original
`GLU32`: Wine cannot satisfy `IOCTL_CDROM_READ_Q_CHANNEL` and reports
`MCI_MODE_NOT_READY`. This check reads the disc label, a `ZDAT` file and CD track
information; it does not read the volume serial. The selected cooked ISO has no
audio-track TOC from which to reproduce the physical-disc check.

The LZ4 reference recipe therefore applies a six-byte return stub only to
`initdisplay` in the private installed `OPTGRAPH.DLL`. It returns `13665984`, the
value required by the recovered `CHECKW` handler and the existing port patch.
The patch verifies the complete source DLL hash, locates the export through
the PE32 tables and checks executable file bounds before writing. Source media,
movies and playback DLLs remain unchanged. Every capture records its patch
offset and before/after hashes in `compatibility.json` and `capture.json`.

```sh
uv run --locked director64 reference --game loewenzahn-4 --windows-version win95
# Reproduce the original failing check without the private-copy workaround:
uv run --locked director64 reference --game loewenzahn-4 --windows-version win95 --unmodified
```

These runs are labelled `compatibility-reference-capture`, not unmodified
original-game evidence. `visual import-original` rejects modified captures,
including records relabelled as original while retaining modification markers.
The unchanged original comparison and native hardware qualification remain
separate from this compatibility run.

The reference image includes 32-bit GStreamer base/good/bad/libav plugins and
verifies them with a generated MPEG audio/video decode during its build.
Its entrypoint records and limits CPU affinity to four available CPUs, with
two llvmpipe workers; this prevents decoder worker pools from exhausting an
old 32-bit projector's address space. The capture container retains a bounded
512-task limit and an init process.

A fresh Löwenzahn 4 compatibility run displays the publisher MPEG and captures
nonzero audio without decoder, allocation or thread-creation errors. Its later
DirectMedia actor-video overlay remains absent over the drawn wagon background;
the complete intro therefore remains visually unqualified under Wine. The
gameplay check skips that intro before selecting a profile and entering the
wagon exterior.

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

`tools/reference/optgraph-probe.c` directly observes the protection DLL. Build it
with the commands above, substituting `optgraph-probe` for `drive-probe`, then
run it in a fresh copy of the private Wine prefix with the same read-only disc
mount. It writes `C:\director64-optgraph-probe.txt`, including DLL/export load
errors, the returned value and a completion marker. The unmodified DLL returns
`5`; the declared compatibility copy returns `13665984`. The probe itself never
patches the DLL or substitutes CD information.

## Android and bug reports

Player file operations suspend a native Lingo continuation until the host
returns the selected bytes, cancellation, or write/print completion. The native
contracts cover token validation, timer suspension, exact car-file bytes,
owned RGBA imports, cropped bottom-up BMP export, and the certificate's original
PrintOMatic commands. The compiler/interpreter regression also checks that a
locally bound `print(doc)` dispatches to its explicit Xtra receiver.

The browser test opens a real file input, checks opaque and translucent image
pixels, normalizes UTF-8 car text losslessly to Windows-1252, downloads exact
export bytes, cancels an outstanding read, and exercises the print handoff.
Image limits are checked before decode. Android uses the document picker and
PrintManager; its queue tests cover buffer ownership, stale operations,
SAVE-to-EXPORT token reuse, cancellation, and Activity teardown.

Mutable image-cast saves use D6TF version 2: the existing field records followed
by a D8IM version 1 block. Each bitmap records its member number, dimensions,
name, registration point, alpha/grayscale flags and depth, then exact RGBA words
in big-endian order. Text-only casts retain version 1. The native round trip
uses a 560 by 420 pixel image, tests truncation/trailing data rejection before
member installation, and verifies erasure does not leave a stale surface.

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

The authored `visual-patchwork.json` Circus plan follows the washing-basket
introduction into the puzzle itself; the default yard plan does not exercise
its imaging code. The source assigns polygon `vertexList` values to an alpha
shape and an outline, then uses `extractAlpha`, `copyPixels` and `setAlpha` to
build the cloth pieces. The converter now retains straight-polygon vertices
and original fill/stroke values, and the runtime rasterizes later edits into
owned image surfaces. Empty vector templates and compound/Bezier paths such as
the selected LZ6 VAMPIRMA members retain their recovered static SWF raster;
their descriptor structure is validated before conversion continues.
Unsupported live compound/curve/gradient edits report their
missing descriptor support. The synthetic imaging regression exercises that
complete alpha chain and checks transparent exterior corners.

The actual Deutsch FieldV template uses a Director10.1 FLSHv26 descriptor,
with an authored four-point list rather than a newly constructed shape.
The v26 parser retains the checked source header, points and typed trailer;
the local42-member audit finds40 editable straight shapes and2 curves retained
as static SWF artwork. Live fill/stroke colors own fresh raster surfaces, so
field feedback preserves source geometry and earlier image snapshots. Synthetic
D10/D8.5 tests check rectangle resizing, current color getters, alpha and cached
pixels independently from the original-projector constructor experiment.

`vector-polygon.lingo` and `text-box-layout.lingo` open a private window on the
original Birthday debug movie before creating synthetic members. Startup alone
has no current movie or cast libraries, so member creation at frame zero is not
a valid test. Loaded DIRAPI/IML32 8.5.1r104 under Wine reports a 12-by-12 image
and centered registration point for the authored triangle spanning eight pixels
with a two-pixel stroke: vector image bounds include a full stroke width of
exterior padding. No game script or artwork is copied into these probes.

The text probe distinguishes intrinsic member bounds from the visible page.
At width 40 and requested Arial 12, the synthetic eleven-letter word wraps into
two lines; fixed/scroll retain the requested ten-pixel `pageHeight` and image
height, while adjust follows the content height. The implementation shares the
host's line placement between text images, visible glyphs, and hit testing.
Long words also wrap within a line on both browser and Android. Native tests
check page geometry and invalidation; a source-free Chromium test checks actual
wrapped glyph pixels and live bold/underline changes. These are original-engine
structural observations under Wine, not native Windows or Android TV fidelity
qualification. The three later Findus ports now recover the original Arial
resource through the existing pinned Microsoft core-font installer transport;
unavailable requested faces use that recovered Arial resource while `.font`
retains the requested name. `text-font-fallback` tests a fresh member for each
face and both property orders; under the same original 8.5 Wine engine, missing
Corsetta/Elephant and a synthetic absent name produced identical geometry and
alpha totals to Arial. Rendering traces this substitution and still fails if its
required recovered fallback resource is absent. This is qualified Wine evidence,
not a claim that every native Windows font-selection policy has been measured.

The synthetic `text-char-position` probe uses the same 8.5 movie context and
records one-based `charPosToLoc` results for left/center/right fields, wrapped
text, newlines, empty text and final carets. At Arial 18 in a 100 px box, centered
`WI` begins at point(39,20); the next row ends at y41. The y coordinate is the
character cell's bottom row. Browser/Android character queries now use host
glyph layout for x, line wrapping and that bottom-row coordinate, including
live field alignment and legacy Windows1252 characters.

`text-mixed-format`, `text-spacing` and `text-zero-size` use that original 8.5
context to qualify character formatting. Arial 12 `AB` followed by Arial 24 bold
`CD EF` in 60 px wraps into two 28 px rows; character coordinates end at y27/55.
Tracking 2 adds two pixels after every character, including the final caret;
changing only character 2 to tracking 4 shifts later characters by four pixels.
Fixed line spacing 32/10 gives row ends31/9 respectively. Point size 0 remains 0
when queried and renders the same 12 px glyphs as the explicit size 12 case.
Paige's stored zero size is also documented as the default 12 by its primary
format manual. RICH package metadata preserves these raw sizes, with measured
host font metrics used when source ascent/line height are absent.
The selected Willy Director 6 STXT database members also contain raw zero sizes.
These remain zero in retained run metadata and use the same measured 12 px
fallback. Applying the original 8.5 default to these older STXT records is an
explicit compatibility inference, not a Director 6 glyph-pixel comparison.

The runtime keeps member references for character formatting and shifts live
styles through chunk edits. Native regressions cover overlaps, nested ranges,
Unicode offsets, alpha/color, cache invalidation and put/delete retention. Browser
and Android share each host's formatted rows between rendering and character
queries. Source-free Chromium checks two real fonts, mixed sizes/colors/faces,
tracking, fixed leading, zero-size equivalence, expanded full-justification hit
boxes, and no-wrap rows clipped against the viewport; pure-Java layout and strict
JNI/Android35 compilation checks cover the Android bridge. These checks do not
qualify native Windows glyph pixels or Android TV display fidelity. The current
Christmas source has 19 text members with single STXT styles; it exercises source
bold and multiple sizes, while synthetic fixtures exercise mixed runs.

`text-field-lines` measures an existing classic CASt field separately from a
modern text member. Under the pinned original 8.5 engine and Wine environment,
an Arial 12 request reports row pitch 15 with character-position y 13/28; fixed
lineHeight 32 changes the second position to 45 while the first remains 13.
The prefix contains Liberation Sans and no Arial font file, so these numeric
font results include Wine substitution. Hosts preserve the structural distinction:
source/fixed line height controls row advance; a classic character position ends
two pixels below its measured baseline. Missing source metrics use the actual
loaded host font's ascent/descent, without a hardcoded per-size font table.
The private text-height callback accepts width 0 to query row pitch without
reentering member rectangle measurement. Source-free native, Java and Chromium
checks cover this separation; they do not establish identical glyph rasterization
between Wine's substitute and recovered Arial.

Untouched classic fixed fields use their source viewport independently from
measured content: the original Birthday LogfileName field reports a145×15
content rectangle with pageHeight48. Source geometry queries do not reserve a
mutable slot for each static field. The fourth boxType, `#limit`, clips to its
fixed viewport and rejects default keyboard insertion that would exceed it;
scripted text assignment may still exceed that page. Source-free input tests
run both extended Director6 and Director8.5 default insertion paths, force GC
during measurement, and check wrapped-height and unwrapped-caret limits.

The synthetic transition probes run inside an original movie window so that
`updateStage` has an active cast and score. Run
`director64 reference --game findus-birthday --windows-version win7 --experiment transition-window-timing`
for Director 8.5.1r104, or
`director64 reference --game loewenzahn-4 --windows-version win95 --experiment transition-window-dseven`
for Director 7.0.2r85. The latter retains the declared private OPTGRAPH
compatibility patch; it is a compatibility reference. These experiments use
an original debug/check movie as a window context and synthetic stage-color
changes. They are experiments, rather than unmodified gameplay comparisons.
At 32-bit color depth, original 7/8.5 effects 1,9,10,23,26,37,50,51,52 block
`updateStage` for approximately 15/30 ticks at time 1/2, while the score frame
stays unchanged. Effect 0 returns immediately. The 8.5 probe additionally
confirms that an unchanged destination still waits and time 0 has a minimum
quarter-second duration. Startup frame 0 measurements are not valid evidence
for presentation timing.

The portable transition contracts (`test_transitions.c` in
`mise run test-native`) cover score-cast descriptors, paused enterFrame,
click-to-skip consumption, intermediate geometry, repeat reveals and equal
cached/uncached frames across Director 5–8.5 profiles. Wipes, pushes, reveals,
cover effects, pattern dissolves, blinds and grouped pixel/bit dissolves now
present over time. Dissolve order uses independently implemented maximal-length
registers with polynomial exponents from [Xilinx XAPP052](https://docs.amd.com/v/u/en-US/xapp052).
Exact original dissolve permutation and indexed-color bit-plane behavior
remain unverified; these tests establish timing and deterministic presentation,
not pixel parity with every original effect. Changed-area bounds use the
changed pixel rectangle, which can differ from the original dirty-sprite
rectangle around transparent padding. Effects 47–48 currently fail explicitly; none occurs in the selected games'
recovered transition requirements. Required checkerboard and diagonal strip
effects are included in the source audit and native intermediate-frame tests.

Early Director projectors can embed the playback engine in the executable.
The module recorder also retains mapped executables under the private original
installation when no DIRAPI/IML32 DLL exists. The selected Löwenzahn 1 launcher
contains Director 5.0.1r26; Löwenzahn 2 contains 6.0.2r32. Their pinned recipes
select the original 32-bit launchers and an optical namespace from the selected
ISO. Löwenzahn 1 declares a 256 MiB legacy RAM/swap report through a private,
read-only `/proc/meminfo` mount: the old executable otherwise adds two large
legacy memory counters into a signed overflow and rejects the machine as
having less than 3 MiB available. This changes Wine's reported environment,
while the original executable bytes remain unchanged. The generated report's
hash and reported totals are recorded separately from the actual container
memory limit and process affinity (both remain at host defaults).

Synthetic probes explicitly choose their FileIO notation. Ordinary probes use
Xtra object methods; `-- director64: fileio-functions` selects the source syntax
required by Director 6; `-- director64: fileio-xobject` selects classic XObject
calls where a compatible FileIO module exists. Completion is injected before
the matching close/dispose call and its interface is recorded as `io_mode`.
`-- director64: full-script` preserves explicitly authored event handlers;
without that first-line marker the existing startup wrapper is retained.
A missing result or completion marker remains an unsuccessful experiment.
The bundled Löwenzahn 1 FILEIO.DLL is 16-bit and cannot instrument its 32-bit
launcher; successful launch after the memory repair is not a successful D5
FileIO or palette experiment.

Palette transitions and high-color output
----------------------------------------

The previous compositor substituted a full-stage RGB fade for palette animation.
That invented black/white flashes at the default high-color display depth. It has
been removed. Macromedia's [Using Lingo, Version 4, chapter 4, p132](https://macos.retro-os.live/index.php/downloads?catid=183&id=16568&m=0&task=download.send)
documents palette transitions as a 256-color display effect, absent in thousands
or millions of colors. The later [Director MX2004 Using Director, p154](https://dit.uoi.gr/files/director_mx_2004using_director_compressed-151-350.pdf)
also explains that 8-bit bitmap palette mismatches are confined to 256-color
systems. Selecting that rule for the intermediate D5/D6 engines is an inference
from these primary manuals. The selected D6 6.0.2r32 engine reports depth24 under
Wine8, but our experimental MIAW baseline remained black; one activation variant
crashed the original. Neither is a successful original pixel comparison. We do
not use them as visual evidence for a no-op. The native palette test verifies that
high-color score commands do not paint an invented stage overlay, while attempted
8-bit interpolation fails with a specific diagnostic. The selected 8-bit Garden
port has no recovered palette interpolation requirement. Authentic indexed fades
and their timing remain unqualified, rather than being called implemented.

Text transport and byte operations
----------------------------------

Package and host text uses UTF-8. Character indexing, count, offset, comparisons
and edits decode that representation, so German umlauts do not occupy two Lingo
characters. `numToChar` and binary FileIO reads retain byte strings with explicit
lengths, including NUL. Concatenation preserves the authored Western byte
encoding when either operand is raw; assignment to a displayed member converts
that byte text to UTF-8 once. The same boundary preserves authentic desktop CAR
files without changing image headers or the recovered manual UTF-8 encoders.
`test_lingo_encoding.c` exercises both representations, mixed text, byte-level
edits and collection across all seven build profiles. Member chunk writes and
deletes retain owner identity and carry formatting runs through their splice;
`test_text_splice.py` runs the real compiler/package/VM with frequent collection.
Non-BMP text and other historical platform encodings have no original-player
qualification from these Western source cases.

The Deutsch controller is a separate movie context with shared globals and heap
objects, independent score timing, persistent member state and bounded startup
initialization. Its recovered launcher and common scripts require synchronous
interface initialization, immediate forget/recreate, and a hidden running score.
[Using Director MX2004, chapter18](https://www.manualslib.com/manual/389802/Macromedia-Director-Mx-2004-Using-Director.html?page=409)
describes simultaneous movie windows. The clean ScummVM reference revision
`41ac2b31847622d0662d22c03fe6979e3b43cfbc` independently corroborates separate
window stepping and transferring the active Lingo context for tell blocks;
its implementation was not copied. Support here covers the selected game's
single named controller and authored panel rectangle, rather than a desktop
window manager.

`test_windows.c` covers hidden score timing, bounded multi-budget startup,
shared object/member/range/image identity, nested tells, modifier keys, mouse
capture, clipped cached/uncached frames and lifecycle cleanup under ASan/UBSan.
The registered Deutsch journey now opens the statistics panel with the authored
Control+Shift+Q shortcut after login/class/room/exercise navigation. It requires
the controller's own score sprites, populated fields, a continuing clock, zero
script alerts and a changed rendered frame. This is source-driven portable
validation, not an original Director10 or physical Android TV comparison.
The same journey uses the visible Sequence Info tab, task-number text field,
room dropdown and Go button to reach `TEST2CB1.DXR`. It requires the authored
`InitFields` handler's completed, nonempty answer ranges within the sentence,
visible editable answer fields and vector backgrounds with positive geometry,
and a visible Flash prompt with measured text height. This covers the previously
missing font variants, tracking and live vector geometry on that concrete route;
entering the movie alone does not pass the check.

Single-player policies are checked by six source-free compiler-to-VM fixtures,
the ordinary 23 journeys, and strict alternate-entry source components. The
components skip `START` and establish bounded activity behavior, rather than
complete input-driven playthroughs: Mucklas race and battleships retain one
human and one computer, and ball retains one human and two computers. Their
human-opponent controls are hidden and inactive; Shift+2 cannot activate the
ball game's second human. The ordinary Optik quiz route and alternate-entry
pinball reach the authored single-player name screen; telephone reaches its
single-player setup after the retained introduction. These policies preserve
saved profiles and historical scores. The ordinary journeys separately check
profile persistence and reboot/reload paths; the component checks do not replace
those checks or original-projector comparisons.

`director64 reference --game loewenzahn-2 --experiment string-bytes-dsix`
corroborates the byte boundary with the original embedded 6.0.2r32 engine under
Wine 8. The ASCII-only probe observes `length(numToChar(n))=1` and the unchanged
character code for 0, 128, 164, 195, 223, 228 and 255. Concatenating bytes 195/164
has length 2 and retains both codes; the ä/ß/Euro byte triple has length 3.
The result includes the completion marker and the mapped executable version/hash.
This is an original-player numeric experiment under Wine, not a native Windows
or projector-display qualification.

Classic field boxes and chunk expansion
--------------------------------------

`text-color-values` verifies original 8.5 color getter types: a member or character
`.color` is RGB; `.foreColor` is an integer palette index. For synthetic
RGB(17,34,51), the tested original returns 239; setting a character's foreColor 255
changes only that character to black. Mutable/nested getters now read the same
live run data that rendering and chunk splices use.

`text-field-chrome` and `text-field-position` mutate only the existing classic
field 7 in the private debug movie. Unlike a new modern Text member, it supports
field-only chrome. Under 8.5.1r104/Wine 8, Arial request 12 yields lineHeight 15;
word wrapping changes a 40-pixel-wide two-row logical rect from 30 to 15 high when
turned off. Margins 0/2, borders 0/1 and box shadows 0/2 leave that logical rectangle
and character positions unchanged. The compositor keeps chrome outside the text
rectangle, retains exact recovered background RGB and shares geometry with hits.
The private prefix substitutes Wine Liberation for the Arial request; those row
numbers are observations of that environment, not proof of native Arial metrics.
The separate `text-field-lines` probe records this distinction at four sizes and
an explicit 32-pixel line-height override. The native chrome contract verifies
source settings, pixels, logical bounds, scrolling row selection and equality of
cached/uncached rendering. Primary box-property scope is documented in
[Macromedia Using Director MX2004, p178](https://dit.uoi.gr/files/director_mx_2004using_director_compressed-151-350.pdf).
The untouched `text-source-box-type` probe reports the same field's source
CASt type byte 2 as `#fixed`, with intrinsic rect 145 by 15 and pageHeight 48.
Source box descriptors therefore need the viewport and content distinction
before any script changes a property.
The `text-field-source-types` experiment uses Deutsch's actual TEST2CB1 fields
under its original Director 10 engine. The launcher advertises 10.1r11; loaded
DIRAPI/IML32 identify 10.1.1r16. Under Wine 8, six untouched fields with source
type byte 3 report `#limit`, wrapping enabled and zero margin/border/shadow.
The fourth mode limits keyboard input to the field's box; a script can still
assign overflowing text. Package type values distinguish adjust, fixed, scroll
and limit independently from the source byte ordering.

The controller's hidden task-information update also requires a native checkbox's
`hilite` property. Recovered button kinds use existing member fields; mutable
checked state occupies an independent GC root rather than a text-field slot.
The source checkbox handler marks data edited and relies on the native control
to toggle before mouseUp. Native regressions check that ordering, release outside,
radio selection, push-button feedback, direct edits and collection. The separate
pixel regression covers all three portable controls, labels, state changes and
cached/uncached equality. Its source checkbox has logical text width -2 but an
authored 16-by-16 sprite; that value must not become a 65,534-pixel allocation.
Original 10.1.1r16 constructor experiments under Wine confirm button types and
default false / mutable true-false hilite values. Portable control appearance is
not an original Windows theme pixel comparison.

`text-field-types` also distinguishes getter types: the existing classic field
returns strings for fontStyle and alignment, whereas a modern Text member
returns a style list and an alignment symbol. `text-large-spacing` confirms that
tracking -1000 is an actual negative advance, not an absent-value sentinel.
The selected Deutsch TEST2CB1 source contains that value. Hosts retain negative
coordinates and saturate bounded hit-box transport rather than wrapping distant
positions back onto the stage. In `text-spacing-inheritance`, tracking also
changes the advance of ASCII spaces; this Director behavior is measured
separately from the underlying Paige manual's character-extra description.
Whole-text replacement inherits the first character's tracking: mixed 3/-2
runs become a uniform 3 after replacement. The runtime retains that value for
both recovered source runs and later character edits, including a source -1000
run that previously lost its spacing after a script assigned new text.
The `text-hilite` experiment completed and confirmed the classic compound style
string `bold,italic`. Its Wine window remained black and global selection
indices stayed zero even after assigning a puppet field sprite. Those results
do not qualify selection indices or highlight pixels; portable selection tests
and the recovered dropdown route are separate evidence.

`chunk-expansion` corroborates original 8.5 gap writes with numeric byte lists.
Writing line 3 of EMPTY creates two carriage returns before the new text;
writing item 3 creates two commas. Writing char 5 of "ab" pads two spaces; writing
word 3 of "one" appends directly. The VM now preserves those distinctions.
This repairs the Deutsch controller's numbered dropdowns, which previously
concatenated all entries into one line. Native tests and the real compiler/VM
exercise the delimiter expansion alongside UTF-8/raw-byte handling and collection.
The controller also checks mouseLine against -1 when the pointer leaves a field.
The getter uses that documented outside-field result and counts logical Return
delimiters, consistent with the pinned ScummVM behavior and
[Macromedia's mouseLine reference](https://www.manualslib.com/manual/389805/Macromedia-Director-Mx-Lingo-Dictionary.html?page=397).

The Deutsch original-player recipe names the source's unchanged `Deutsch.dat`,
an x86 PE32 GUI projector despite its extension. Its separate launcher hash is
verified before Wine executes it; `.dat` files are never discovered implicitly,
and DLLs, PE64, console binaries and malformed headers are rejected. No executable
renaming or retail-byte patch is needed. The launcher and `proj.dll` advertise
10.1r11, while the actually loaded `DIRAPI.DLL` and `IML32.DLL` report
**10.1.1r16**. Reproduce the authored source-field probe with:

```sh
uv run --locked director64 reference --game lernerfolg-deutsch-1-2 \
  --windows-version win7 --experiment text-field-source-types
```

It reads actual `TEST2CB1.DXR` fields 49, 50, 56 and 73–75 before changing any
properties: all six raw CASt box-type-3 records report `#limit`, with their
authored rectangles, wrap enabled and zero margin/border/shadow. Subsequent
`#adjust`, `#fixed` and `#scroll` assignments report the assigned values. The
probe requires MX2004's explicit `window().new(name)`; `window(name)` returns
VOID for an absent window in this engine. The completed Wine8 experiment and
mapped DLL identities corroborate the fourth documented mode in
[Macromedia's MX2004 Scripting Reference, p671](https://eclass.hmu.gr/modules/document/file.php/TP194/drmx2004_scripting_ref.pdf).
This is original-projector semantic evidence under Wine, with no native Windows
or hardware claim. Completion records the output; this probe has no registered
automatic image or semantic comparator.

`--experiment native-button-types` uses that same original10.1 recipe and creates
authored push-button, checkbox and radio-button members in a safe source movie
window. All three report type `#button`, their assigned `buttonType`, initial
`hilite=0`, then exact `1,0` hilite updates. The completed probe corroborates the
runtime checked-state default; it does not claim that the original controller's
startup or pixels were compared. The [MX2004 Scripting Reference, p798](https://eclass.hmu.gr/modules/document/file.php/TP194/drmx2004_scripting_ref.pdf)
also documents the unchecked default. Clean ScummVM41ac2b3's text-cast decoder and an
audit of 164 selected native-button records independently identify the BE16 kind
after the 28-byte classic descriptor. Source type7 records now retain that kind
through the existing MOVI shape/filled fields, including Deutsch's empty-label
`TaskInfo_UseIntest` checkbox. No game artwork or retail bytes are rewritten.

`--experiment vector-defaults-dten` observes a new native vector shape in that
same original10.1/Wine environment: empty `vertexList`, rect(0,0,320,240),
stroke width1, fill RGB(221,221,221), and closed0. The completed getter probe
does not support inventing a default four-vertex rectangle. A source handler
that expects four vertices must receive them from its authored template or
initialization; constructor defaults and recovered shape data are separate
requirements.

The input runtime is single-player. The source-free `input-selftest` uses the
production browser controller selector and cursor policy in Chromium: an extra
controller cannot generate game/menu actions, menu presses remain consumed until
release, disconnect selects a replacement, and standard OS cursor roles suppress
the software glyph. Its gamepad samples are synthetic, so this is component
validation rather than physical gamepad, Android PointerIcon or original cursor
pixel qualification. Native input/compositor tests cover button release on
disconnect, absolute journey compatibility, one cursor's restored pixels, and
source bitmap/mask/invert fallback. Existing `pad ticks x y buttons` recordings
remain valid; indexed controller samples are rejected.

Mucklas KR's source direction helper returns a point that its driver negates.
Geometric unary negation now preserves point/rect identity and returns an
independent value. This agrees with ScummVM's array/geometry negation behavior
at clean revision `41ac2b31847622d0662d22c03fe6979e3b43cfbc`; no implementation
code was copied. A source-free compiler-to-VM regression executes the helper
call, negation and coordinate mutation with collection between small budgets
on D8 and D8.5. Numeric contracts check both coordinates, source independence
and rectangle identity under ASan/UBSan across all seven runtime profiles.
These are source/reference and runtime checks, not original-projector parity.

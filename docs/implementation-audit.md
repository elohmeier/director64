# Director implementation audit

## Repair status

The findings below are the **pre-fix baseline**, not the current support
inventory. The repair adds shared ink composition, mutable Imaging Lingo,
live text layout, shape opacity, mask sampling, linked child runtimes,
sound pause/resume, film stop/resume and recorded film-layer playback.
The Rust capability check now runs during normal packaging on native, Web
and Android paths. Rendered RPC probes invoke the compositor, and native
checks exercise the supported Director profiles. Unsupported conversion
losses fail with source locations instead of becoming successful packages.

Original Director 8.5 measurements also corrected the baseline: partially
blended Copy **does** preserve its rectangle when `useAlpha` is false.
Implementing the original behavior requires retaining that rectangle, not
applying the ScummVM-derived matte proposed in finding 6. Mask fallback,
mask positioning, ink arithmetic and shape blend rounding now have synthetic
original-projector observations; see [reference-capture.md](reference-capture.md).

The additional conversion work recovers PICT and rich-text raster members,
QuickTime-Xtra MPEG clips, linked movie paths, full Flash timelines and live
named Flash depths. A named object's property writes now transform its
rendered plane. Merely reading geometry leaves authored animation intact.
Live Flash text retains its local layout rectangle and per-frame affine
placement, so fractional translation, rotation, shear and independent scaling
survive conversion and live edits.
Linked movies retain independent score and interpreter state, but their
movie and frame scripts are suppressed. Enabled sprite/cast handlers remain
interactive, including while the child playhead is stopped. Host frame
opportunities and the host palette govern the child; its tempo, palette and
transition channels cannot replace them. Modern internal film loops likewise
retain sprite and sound channels while leaving other main channels inert.
These contracts follow Macromedia's
[MX2004 Using Director, p.202](https://dit.uoi.gr/files/director_mx_2004using_director_compressed-151-350.pdf)
and are covered by focused tests against recovered record categories. They
are distinct from an original-projector comparison of every imported film.
The shared conversion that both the Android and web players use transcodes
video, including MPEG clips, to VP9 and audio to WAV/Opus with FFmpeg. Missing or failed audio/video decoding
now reaches the player error UI and rendered probes.

Film references now retain each archive's `ccl ` cast relocation table.
Original-player controls showed that encoded cast zero can select a different
logical cast, and that the table applies to visual children as well as sound.
All 48 previously unresolved film sound references in the local corpus now
resolve to sound members. Scripts can also replace individual film children;
empty template children keep their authored channel and geometry.
Modern native films retain compact channel deltas and independent nested
playheads. Original-player experiments show why a member-reference cycle
need not recurse forever: successive nested instances start at later poses,
and a later pose can remove the recursive child. Reads do not advance those
playheads; presentation and explicit seeks have separate, tested effects.

Texture brushes retain palette indices, rather than baking a fixed Windows
RGB palette. Main-score commands and `puppetPalette` update their live colors,
and `member.pattern` selects from the owning cast's eight extracted textures.
Original Director 7 and 8.5 probes establish the palette dependence; a complete
RGB5 census plus independent unaligned samples establishes the brush quantizer.
Retail texture pixels still come from the selected engine's resources.

The repair validation converted fourteen recovered corpora containing 702 movie
files and serialized all fourteen code/scene packages in format 9. Shared
requirements passed for Web and Android after the normal D5 media
postprocessor. Four representative Wasm runtimes compiled. Fresh Android
exports then produced twelve complete ARM32 APKs, including converted media;
their embedded pack hashes and build receipts were verified. The two remaining
APK builds initially stopped at damaged source streams. A subsequent
[media repair](video.md#preparing-separate-repaired-images) enables both remaining
builds through separately pinned images, with missing content explicitly
reconstructed. All fourteen signed ARM32 APKs now pass source, package and
6,000-tick startup verification from clean commit `152f450`.
The fourteen code/scene packages alone contain asset references, so their
success does not certify those external streams or a complete playthrough.

These implementations are not a full-game qualification. Arbitrary AVM1,
unsupported SWF tags/effects and damaged source streams remain explicit
requirements. Rich-text rasterization
does not implement arbitrary rich-text editing. Software pixels, Wine
projector observations and device results must stay distinct. The new failure gates expose these gaps; they do not implement
them or turn a passing startup into proof that every activity works.

The subsequent [performance and correctness follow-up](performance-audit.md)
records a physical EF-62N Birthday slowdown and input timeout, excessive film
redrawing, Flash invalidation/lifetime defects, and Android media/input bugs.
It also maps recovered animation catalogs to a preprocessing and caching
design. Those findings remain open after the repair described here.

## Why these gaps survived

Several representations discarded information needed at playback. Flattened
film frames could not respond to changes in a child's text or geometry, and
sentinel images could not support pixel mutations. Linked movies needed their
own score state, event dispatch and clocks; a static picture could not supply
those services. The repairs preserve that state and connect it to both
renderers. These were implementation gaps in this project, rather than a limit
imposed by what another player implements.

Verification also missed visible output. Script-state journeys advanced the
game without invoking the compositor, deferred conversions could still be
packaged, and the main check skipped Rust tests when Cargo was absent. The
repair makes those failures explicit, runs rendering in the default RPC probe,
and exercises the versioned C runtimes. Asset receipts are invalidated before
generation starts and compare their original source/compiler/ABI identity
again before recording success, preventing mixed or failed builds from
certifying stale outputs.
Both Python module entrypoints now execute the command line and propagate
argument failures; invoking `python -m director64.cli` previously returned
success without doing any work.
Pacing assertions require enough measured intervals to judge their sustained
budget: an empty, partial or historical log without device timing cannot pass.
Historical logs remain readable for diagnostics. Android conversion caches
verify their tools, policy and asset hashes, and installation rejects APKs
whose build receipts no longer match the current source inputs.
Fresh APK export also exposed valid WebAssembly addresses above 2 GiB being
interpreted as negative JavaScript offsets. The shared importer now treats
memory addresses and lengths as unsigned wasm32 values, checks their ranges,
and refreshes views after memory growth. Synthetic tests cover both the high
address bit and actual memory growth.

Ambiguous behavior still needs evidence. The original-projector experiments
already overturned one matte assumption derived from ScummVM and exposed a
movie-context error in a sound probe. Contemporary documentation, reproducible
synthetic inputs and recovered game requirements are complementary evidence;
agreement with any one reimplementation cannot establish complete support.

## Baseline at revision 675811f

The audited runtime had rendering failures that successful conversion and
script-state replay do not detect. The highest-priority fixes are unsupported
bitmap inks, missing Imaging Lingo operations, text scrolling, and opaque
overlays. Add capability checks before packaging and make journeys render
frames before treating their success as visual evidence.

This baseline covers revision `675811f` on 2026-10-18, fourteen existing local
recovered corpora, both renderer implementations, and fresh host diagnostic
executions. Runtime behavior was not changed by that audit. Original-projector
comparisons and device validation remain separate work; the game
locations below identify authored requirements, not verified player routes.

The local ScummVM checkout was clean on `master` at
`41ac2b31847622d0662d22c03fe6979e3b43cfbc`. It supplied behavior and format
references only; no implementation was copied. Relevant references are
[bitmap masks and blend](https://github.com/scummvm/scummvm/blob/41ac2b31847622d0662d22c03fe6979e3b43cfbc/engines/director/channel.cpp),
[ink composition](https://github.com/scummvm/scummvm/blob/41ac2b31847622d0662d22c03fe6979e3b43cfbc/engines/director/graphics.cpp),
[text properties](https://github.com/scummvm/scummvm/blob/41ac2b31847622d0662d22c03fe6979e3b43cfbc/engines/director/castmember/text.cpp),
and [scrollByLine](https://github.com/scummvm/scummvm/blob/41ac2b31847622d0662d22c03fe6979e3b43cfbc/engines/director/lingo/lingo-builtins.cpp).
Agreement with these implementations is not an original-projector comparison.

## Reproduce the findings

Run the synthetic pixel and service diagnostics without game media:

```sh
bash tools/audit/visual-probe.sh
```

The baseline result was 40 mismatches across 41 checks: 11/11 for D8, 15/15
for D10, and 14/15 for D8.5. These are repeated checks of the defect categories
below, not forty separate bugs. The D8.5 shape-opacity check passes. Exit 1 is
intentional while mismatches remain. The probe uses the software compositor
at an unscaled stage size.

Run a conservative preflight on an existing normalized model:

```sh
uv run --locked python -m director64.audit "$MODEL" --target web --strict
# --target android selects the Android capability set.
```

It reports unsupported bitmap inks, linked movies, unresolved member counts,
conversion problems, and approximation counts. Strict mode exits 1 on the
first two categories or conversion problems; invalid input exits 2. It
resolves external casts and reports movie, frame and sprite for failures.
Records unresolved statically can be dynamic members or script placeholders,
so they are counted for review rather than automatically called defects.

At the baseline this command was opt-in and not wired into normal builds. It scans authored
score records; scripts can change their visibility and ink before rendering.
Conversely, script-created requirements can be absent from the score. Passing
this narrow preflight does not certify a game. Generated reports belong under
ignored `build/`; no game media or captures accompany this audit.

The existing `mise run test-native` suite passes. The seven new preflight tests
pass with `uv run --locked pytest tests/test_visual_audit.py`.

## Findings with game evidence

### 1 Unsupported authored bitmap inks halt rendering

Priority P1. The bitmap branch of
[the software compositor](../platforms/web/compositor.c) rejects inks outside
its small allowlist with `unsupported native bitmap ink`.

The synthetic probe confirms renderer failure for each of 2, 4, 7, 33, 40
and 41. Recovered score records contain the following requirements:

| Game | Unsupported ink | Records | First useful checkpoint |
| --- | --- | ---: | --- |
| Birthday, Director 8.5 | 7 | 43 | `R0041.DXR`, frame 2230, sprite 19, watering-can animation |
| Löwenzahn 6, Director 8.5 | 41 | 271 | `BAT.DXR`, frame 18, sprite 67 |
| Löwenzahn 7, Director 8.5 | 2, 4, 33, 40, 41 | 289 | `ABC__ABC.DXR`, frame 25, sprite 120; `FOTOLABOR__FOTOLADEN.DXR`, frames 95 and 505, sprites 20 and 2 |
| Löwenzahn 8, Director 8.5 | 41 | 3 | `TS6METAL.DXR`, frame 1, sprite 15 |
| Löwenzahn Advent, Director 8.5 | 41 | 175 | `PAINTBOX.DXR`, frame 3, sprite 38 |
| Löwenzahn Optik, Director 8.5 | 41 | 66 | `LABYRINTH__LICHTLAB.DXR`, frame 172, sprite 55 |

These 847 records are changed channel snapshots, not distinct sprites or
proven reachable failures. ScummVM names 2 Reverse, 4 Not Copy, 7 Not Ghost
and 33 Add Pin. Its ink enumeration does not establish the semantics of 40
or 41: those need version-specific investigation, not an invented fallback.

Fix acceptance: implement or explicitly reject each requirement before
packaging, then render the relevant scene with its actual palette, mask and
blend settings. A script-only journey cannot exercise this failure.

### 2 Imaging Lingo silently discards gameplay output

Priority P1. In [d10.inc](../runtime/director/d10.inc), `image()` and `crop()`
produce a blank sentinel, `duplicate()` aliases the original, `extractAlpha()`
returns the original image, `getPixel()` always returns black, and
`copyPixels`, `fill`, `draw`, `setPixel` and `setAlpha` return success without
changing pixels. Assigning a member's `picture` also does nothing.

The probe crops a 4×1 image to 2×1 and reads width 0. Setting a pixel red and
reading it back returns black without a script error. This is a missing
mutable-image implementation, not an isolated converter problem.

Concrete users include Ghosts' `HUEHNER.DXR`, script 24, `shootScreen`, which
crops the stage and displays the result; Circus' `ENGINE.CXT`, script 142,
`getImageOfText`, which copies and crops text pictures; and Löwenzahn 7's
`FOTOLABOR__FOTOLADEN.DXR`, script 1, `setBackPaintImages`, which duplicates,
composites and changes alpha for photo selections. The same API family occurs
in Birthday, Deutsch and the Löwenzahn painting activities.

Fix acceptance: independent image objects with preserved dimensions, correct
pixel/alpha mutations and renderer invalidation; crop and copy round trips
must pass before qualifying painting, photo or puzzle activities.

### 3 Browser and Android text ignores scrolling and live styles

Priority P1 for scrolling, P2 for style differences. The text branch in
[compositor.c](../platforms/web/compositor.c) reads changed text and color,
but never reads `field_scroll`, `field_fonts`, or `field_text_changed` when
choosing the insertion style.

The probe confirms that setting `scrollTop` updates runtime state but leaves
the displayed row unchanged. Replacing text also keeps a 12-point original
style instead of the fixture's 18-point insertion style.

Willy's `10.DXR`, script 63, `scroll` and `setText`, actively use
`scrollByLine`; Deutsch also writes `scrollTop`. Mucklas' `FA.DXR`, member 4
`fanga`, has an authored 16-point style and an 18-point insertion style in the
recovered data. Willy changes fonts in `08.DXR` and `CDDATA.CXT`.

Fix acceptance: render the live field state, include it in text cache keys,
and apply the same scroll/style transformation to text hit testing.

### 4 Translucent shapes become opaque outside the D8.5 software profile

Priority P1. `fill_shape` blends only under `DIRECTOR64_D85`; other software
profiles call an opaque fill. The
synthetic 50-percent red rectangle over blue produces red 255 instead of
approximately 127 in D8 and D10; D8.5 correctly produces 127.

Mucklas' `CREDITS.DXR`, frame 49, sprite 4 is a filled full-stage rectangle
at 80-percent opacity. Deutsch's `ROOM01.DXR`, frame 372, sprite 80 is a
filled full-stage rectangle at 10-percent opacity. Both can obscure the
scene instead of providing their intended overlay.

Fix acceptance: shared opacity behavior across shapes, bitmaps and text;
check the overlay over real scene content on each target.

### 5 Explicit masks leave stale coverage bounds and clip visible pixels

Priority P2. `wc_image` calculates `cover_l/t/r/b` from the decoded image,
then `apply_mask` changes its alpha without recomputing those bounds. The
optimized untransformed bitmap draw skips all pixels outside the old bounds.
An explicit mask can make pixels outside the original matte visible.

The synthetic image has one covered pixel and an all-black explicit mask.
Its newly opaque white pixels still show the blue stage instead. Local
Birthday images corroborate this condition:

- `R0051.DXR`, frame 57, sprite 76, member `R0051_HakenTorte_01`: original
  coverage starts at row 2; the explicit mask admits pixel (12, 0).
- `R0081.DXR`, frame 621, sprite 41, `field12_h`: coverage starts at row 1;
  the mask admits pixel (26, 0).
- `R0091.DXR`, frame 1061, sprite 37, `haeckchen`: the same top-edge condition.

Fix acceptance: compute coverage bounds after every operation that changes
coverage. Check the untransformed optimized draw against the complete masked
pixel plane. This specific optimization defect is in browser/Android code.

### 6 Blended Copy ink fills the matte and creates rectangular halos

Priority P2. [ink.h](../runtime/director/ink.h) forces Copy pixels opaque
without considering sprite blend. The renderer then blends the whole
rectangle. ScummVM's `Channel::getMask` requests a matte for blended Copy
bitmaps, with a separate one-bit-image exception.

The probe's transparent white pixel becomes pale blue instead of leaving the
blue destination untouched. Birthday has non-FollowAlpha bitmap data with
transparent pixels under partially blended Copy ink: `R0091.DXR`, frame
2204, sprite 79, `F_collision` at 70 percent; and `R0161.DXR`, frame 744,
sprite 15, `ms-intro_0252` at 81 percent.

Fix acceptance: preserve the distinction between opaque Copy and blended
Copy, including cache identity and the source bit depth. Compare the
transparent border against the original projector before generalizing to
all Director versions. Both backends use the shared ink helpers.

### 7 Linked Director movies are invisible and do not play inside sprites

Priority P1 for activities depending on them. The D8.5 software compositor
explicitly returns without drawing cast type 9. This was documented for
Löwenzahn 8's dog, but the recovered scores show broader use:

- Löwenzahn 7: `FABRIKSPIEL__FABRIKSP.DXR`, frame 1, sprite 120,
  `Intro.dir`; `LEXIKON__FABRIKSC.DXR`, frame 372, sprite 4, `dampf.dir`.
- Löwenzahn 8: `BERGWERK.DXR`, frame 695, sprite 106, `Levelwaage`;
  `HUNDEQUI.DXR`, frame 60, sprite 59, also `Levelwaage`.

The scan finds six changed records in Löwenzahn 7 and twelve in Löwenzahn 8.
Fix acceptance requires child-movie playback and composition, or an explicit
blocked capability for the affected activity. Treating the cast as an empty
widget hides the missing implementation from script-error checks.

### 8 Sound pause and resume are successful no-ops

Priority P2. The D10-family sound methods in
[d10.inc](../runtime/director/d10.inc) return success for `sound(n).pause()`
and argument-free `play()` without pausing or resuming audio. Only pause
emits `SOUND_PAUSE_APPROXIMATED` through the optional trace callback.

Ghosts' `GLOBPROG.CXT`, script 9, `pauseSound`, calls this method and marks
its own state paused. Director64's audio continues while the script believes
it has stopped. This finding follows from the recovered handler and service
implementation; it was not verified with an audio capture.

Fix acceptance: pause/resume must preserve the playback position, reported
channel state and synchronization with the authored activity.

## Additional semantic defects requiring route verification

### 9 Background Transparent hardcodes white

The ink helpers remove white without accepting the sprite's `backColor`.
ScummVM's color-image path compares against `backColor`; its one-bit path has
separate foreground-color behavior. The synthetic red background key draws
red and removes white, exactly the inverse of that fixture's requirement.

No nonwhite key was confirmed in the inspected Mucklas, Löwenzahn 6 or
Löwenzahn 8 score examples after resolving palette/RGB flags. Keep this as
a proven synthetic gap rather than attributing it to a particular visible
game defect. Script-written colors need a separate scan.

### 10 Sprite stop seeks to the final pose

The Flash-sprite `stop()` implementation sets `film_frame` to the last pose;
it has no paused-state flag. The probe's middle pose changes to the final
pose. The normal film tick can also continue advancing a looping member.

Deutsch has sprite stop calls in `SCRIPTS2.CXT`, script 79, `beginSprite`,
and `TIVOLAINTRO2.CXT`, scripts 1 and 2, `skipPlay`. Some authored sequences
immediately rewind or intentionally seek near the end. Those sequences must
be exercised before claiming an observed regression from this shortcut.

## Existing conversion losses that need explicit qualification

The model already records many losses as `approximations`, while
`problems` is empty in all fourteen inspected corpora. These counts are
diagnostic records, not independent defects. Particularly consequential
examples include:

- Circus: 243 `film-loop-unresolved-child` records.
- Ghosts: 148 unresolved children, 19 unflattened children and 15 missing
  film-loop masks.
- Löwenzahn 8: 30 deferred conversions, 31 film-loop sound deferrals and
  one ignored Flash-action record.
- Löwenzahn Optik: 88 deferred conversions and 27 unflattened film children.

Some unresolved film children are intentionally supplied by scripts. Others
can represent missing animation layers. A blanket failure on every count
would conflate these cases. Audit each exception against the live rendering
path, and retain exact scope and a regression test for accepted cases.

At the baseline the browser importer lacked MPEG-1 decoding for Löwenzahn 8's
DirectMedia clips while Android's FFmpeg export decoded them. Both players now
use the FFmpeg conversion, which closes that platform gap.

## Make failures visible before release

The current checks have three specific blind spots:

1. [scene.rs](../compiler/src/scene.rs) validates score record structure and
   preserves the ink number, but does not match requirements to renderer
   capabilities. Packages containing the unsupported inks above still build.
2. [probe.mjs](../platforms/web/node/probe.mjs) boots and executes RPC commands
   without invoking `d64_render`. Sprite-state parity therefore never reaches
   the failing compositor branch. The actual browser calls render and checks
   `d64_error`, so the player can stop where the probe passed.
3. [.mise-tasks/test-native](../.mise-tasks/test-native) builds the compositor
   contracts only in the default D6 profile. D8/D10/D8.5 rendering branches
   are outside that check. Runtime approximation traces are not failure
   counters; the browser sends them to `console.debug`.

Conversion does already reject unpinned fatal problems, and
[compile.rs](../compiler/src/convert/compile.rs) checks that deferred-conversion
pins are used rather than stale. Extend that discipline instead of adding
another blanket warning switch.

The next build-system change should implement these gates:

| Gate | Fail condition | Deliberate exception |
| --- | --- | --- |
| Shared compiler preflight | Target cannot render a required ink/cast or supply a required service | Exact game, edition, Director version, target, feature and member/handler scope; reason and test |
| Conversion loss inventory | New missing layer, mask, sound, ignored action or deferred output | Reviewed per-source baseline; changed and stale entries fail |
| Runtime diagnostics | Unimplemented operation or unexpected approximation is exercised | Structured capability diagnostic with scene, frame, sprite/member and handler; approved exceptions remain counted |
| Rendered journeys | Render error, missing asset, or changed reference checkpoint | Explicit checkpoint update with reviewable visual evidence |
| Compositor matrix | Pixel contract fails under a supported profile | None for a claimed implementation; keep unfinished profiles explicitly unqualified |
| Release receipt | Required target/evidence gate missing | Development output may be produced, but its missing qualification is retained in metadata |

Put the authoritative capability check in the Rust compiler used by native,
Wasm and Android conversion. The Python preflight here is a diagnostic
prototype; duplicating its allowlist independently across production build
scripts would create another source of drift. Promote the synthetic cases
into the normal profile matrix as their fixes land.

Runtime gap reporting should distinguish an unimplemented engine operation
from an original script alert. Failing every recovered Lingo alert would
change the intended alert-and-continue behavior and generate unrelated noise.

Prioritize the ink/build gate and rendered journeys first, then Imaging Lingo,
text scrolling and shape opacity. Mask bounds is a small, well-isolated pixel
fix. Linked movies need a larger playback design. Keep route and original
comparison work attached to each fix rather than inferring full-game support
from a passing startup.

## Edition identities

The examples use the repository's pinned editions: Birthday
`sha256-dadd038fff19`, Circus `sha256-40d3f559158e`, Ghosts
`sha256-05d7ad483501`, Mucklas `sha256-ad4c7afb4b19`, Willy
`sha256-938be20a50e2`, Deutsch `sha256-4c87e10dcc99`, Löwenzahn 6
`sha256-930556e23657`, Löwenzahn 7 `sha256-b1d146c2f036`, Löwenzahn 8
`sha256-ebdb5a53bb24`, Advent `sha256-00ae07ac7e3c`, and Optik
`sha256-221da0586845`. Recovered movie metadata agrees with the declared
D6 Willy, D8 Mucklas, D10 Deutsch and D8.5 newer Findus/Löwenzahn profiles;
Löwenzahn 6 additionally contains one D8 movie. The other scanned corpora are
Workshop D6, Christmas D7 and Löwenzahn 1 D5.

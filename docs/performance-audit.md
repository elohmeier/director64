# Performance and correctness follow-up

Audit date: 2026-10-18. Runtime baseline: `25ae3bd`.

This review combines a startup on the EF-62N, focused reproductions against
the current C/Java/JavaScript implementations, and recovered game requirements.
It identifies remaining defects; it does not qualify complete games. Runtime
code was left unchanged during the audit. Generated harnesses, game data and
captures remain under ignored build directories.

## Projector measurements

Birthday's `R0021.DXR` bedroom has an authored tempo of 12 frames/second.
On the installed ARM32 build, a steady diagnostic snapshot reported 9 service
ticks/second, 2 redraws/second, 14 MiB of cached images and 46.8 seconds of
accumulated dropped service time, with zero script alerts. The service target
is 60 ticks/second. A separate SurfaceFlinger sample of 127 presentations
averaged 2.98 presentations/second, with a median interval of 266.7 ms.
One game thread consumed about 94% of one CPU core in a thread snapshot.

The first diagnostic window after opening the menu was excluded from the
steady observation: the diagnostic denominator includes menu hold time.
Neither a successful package build nor the absence of script alerts detects
this performance failure.

With the diagnostic overlay enabled, a later menu key caused a recorded
Android input-dispatch timeout: the system waited 10,001 ms for Birthday.
The test startup was stopped and the launcher restored. The renderer/UI lock
sharing below is consistent with this failure; a native stack profile was
not available on the production device, so the exact blocked call is unproven.

An instrumented host replay used the production Birthday package and image/data
packs and reached the same `R0021.DXR` frame 470. It found seven live 800×600
`genericFilmloop` instances: 3,360,000 intermediate source pixels per redraw.
Their resolved visible child-bound unions total 224,949 pixels before clipping,
about 6.7% of that area. Tight bounds could therefore remove approximately
93.3% of this intermediate area at this checkpoint; that is not a predicted
frame-rate improvement.

One hundred forced redraws of this frozen scene requested 3.044 GB of temporary
compositor allocations, about 30.44 MB per redraw, with zero asset reads. The
desktop mean was 20.10 ms per redraw. Exclusive rendering time was 56.1% bitmap
work, 25.7% shape work, 7.0% masks, 6.8% coverage and 3.7% film setup/copies.
Film rendering, including its bitmap/mask children, occupied 49.9%; this overlaps
the exclusive categories and must not be added to them.

A separate 300-tick live phase took 23.8 ms in service and 6,016.6 ms rendering,
performing 300 redraws and requesting 9.137 GB of temporary allocations, again
with zero asset reads. This identifies rendering as the dominant work in the
matching scene. These timings describe the host, not the projector. The host
font callback was blank, so this replay does not qualify font output or full
scene parity.

The same live phase redrew 300 times, but exact comparisons of consecutive
cursor-free scene buffers found **271 of 299 redraws byte-identical to the
previous picture** (90.6%). There were 29 distinct scene hashes. All seven
template film playheads changed every tick even when their effective output
repeated. Film pose changes and setters invalidate globally without checking
whether the resolved visual content changed. Timing and sound should continue
advancing while equivalent visual poses reuse their output.

Of the 30.44 MB temporary allocation request per redraw, 26.88 MB comes from
the seven full film planes and stage backups. Recreating paired-mask coverage
contributes another 3.56 MB. Mask ink and partial opacity select the general
per-pixel bitmap path, so unused transparent areas still pay coordinate and
coverage work. This is why buffer reuse alone cannot remove the pixel cost.

## Confirmed findings

| Priority | Finding | Evidence and effect |
| --- | --- | --- |
| P1 | Native films repeatedly render full intermediate planes | Birthday reaches 2–3 presentations/second on the projector. The software film path allocates a film plane and stage backup, saves/clears/copies the stage, scans coverage and composites again on every redraw. See [compositor.c](../platforms/web/compositor.c#L1183). |
| P1 | Film playhead changes force visually redundant redraws | Exact comparison finds 271 of 299 consecutive rendered bedroom pictures unchanged. A different template pose invalidates the scene even when live overrides produce the same picture. [film_children.inc](../runtime/director/film_children.inc#L465), [setters](../runtime/director/film_children.inc#L336). |
| P1 | Pointer movement rebuilds an unchanged scene | Three synthetic 800×600 films allocate 11.52 MB per pointer redraw; 100 pointer moves cumulatively allocate 1.152 GB. Identical calls without pointer movement reuse the frame. The cache includes pointer state before rendering the whole scene: [compositor.c](../platforms/web/compositor.c#L1755). |
| P1 | Flash child changes can leave displayed pixels stale | A child position change and a child advancing under a paused root leave the visual revision unchanged. The full compositor skips rendering; forcing a revision renders the changed pixels. Actual Deutsch SoundControl interaction reproduces unchanged revisions. [flash_runtime.inc](../runtime/director/flash_runtime.inc#L124). |
| P1 | Flash placement churn exhausts a cumulative instance limit | Actual Deutsch SoundControl down-arrow handlers fail after 109 press/release cycles, despite a fixed small visible control. Removed instances remain allocated/rooted and counted against 2,048 entries. [flash_runtime.inc](../runtime/director/flash_runtime.inc#L284), [removal](../runtime/director/flash_runtime.inc#L384). |
| P1 | Video presentation can starve behind the playback target | Actual Android code receives nine valid frames one frame behind the moving target and displays zero; holding the target allows one to appear. The browser equivalent displays zero of twelve delayed frames during playback. Both require an exact match with the newest target. [Android video.c](../platforms/android/jni/video.c#L211), [browser video.js](../platforms/web/site/video.js#L65). |
| P1 | Remote/gamepad taps disappear between game-thread samples | A complete down/up pair leaves zero buttons for the next sample. Mouse clicks survive the same sequence because they have a press latch. Slow rendering widens the vulnerable interval. [main.c](../platforms/android/jni/main.c#L695). |
| P2 | Slow rendering blocks UI operations | The game holds `engine` through script service, composition and surface presentation. UI-thread keyboard, diagnostics and text operations acquire the same mutex. A slow frame can therefore block UI handling; the physical timeout above establishes the resulting symptom, without proving the exact blocked JNI call. [main.c](../platforms/android/jni/main.c#L429), [UI entry points](../platforms/android/jni/main.c#L728). |
| P2 | Global pauses count toward pending audio playback offsets | Actual mixer code completes a decoder job during a 30-second hold with zero game ticks: a 60-second sound starts 30 seconds in, and a shorter sound disappears. Suspension pauses output but is absent from pending-job elapsed-time accounting. [mixer.c](../platforms/android/jni/mixer.c#L196), [completion](../platforms/android/jni/mixer.c#L308). |
| P2 | Modified key releases leave keyboard keys held | A-down, then A-up with Alt held, then a fresh A-down forwards only the first down. The modifier filter runs before releasing the held key; the next press is suppressed. Pause/device removal also lacks held-key cleanup. [Keys.java](../platforms/android/java/org/director64/tv/Keys.java#L65). |
| P2 | Video decoder errors can silently stall | Injecting an Android codec output error leaves `video_error()` empty. Negative output results are handled like temporary lack of output. [video.c](../platforms/android/jni/video.c#L204). |
| P2 | Browser video duration queries repeatedly decode WAV samples | 120 duration queries on a synthetic 10-second stereo 48 kHz WAV allocate 460.8 MB in 240 temporary float arrays. Video playback asks for duration every update. This affects browser WAV packs; Android uses a different audio host. [audio.js](../platforms/web/site/audio.js#L337), [caller](../platforms/web/site/video.js#L151). |

Two additional pixel defects have synthetic evidence, with narrower current
game exposure still under review:

- Cropped flattened film poses use crop-adjusted destination bounds but the
  full sprite quad when flipped. An 8×8 pose with a 2×2 corner crop differs
  in eight pixels from its equivalent uncropped pose under horizontal flip.
  Current native-film conversion retains live tracks; this does not establish
  an affected current retail route. See [compositor.c](../platforms/web/compositor.c#L1373).
- A cached button image does not refresh after changing its text. The image
  cache refreshes text member type 3 but also needs to consider button type 7.
  Real text setters reproduce the stale image; authored button-picture usage
  has not been established. See [imaging.inc](../runtime/director/imaging.inc#L154).

## Preprocessing opportunity in Birthday

The bedroom's animation data is already finite and recoverable. Four script
members in `R0021ANI.CXT` return sparse animation catalogs:

| Catalog | Source frames |
| --- | ---: |
| `vorhang_ll` | 80 |
| `pettersson_ll` | 50 |
| `findus_schnarchen_ll` | 34 |
| `findus_aufrecht_ll` | 121 |

`ENGINE.CXT`'s animation manager expands these catalogs, resolves member names,
and writes each current child's member, position, ink and blend into the
800×600 `genericFilmloop` template on `prepareFrame`. Each instance has its
own effective children. A cache indexed only by the template member and its
own film frame would return incorrect pixels.

A useful conversion/runtime design is:

1. Normalize the sparse animation catalogs during conversion. Resolve source
   assets once and retain their timing, looping, markers and sound events.
2. Precompute immutable source coverage, mask alignment, affected bounds and
   sampling geometry. Store cropped source fragments with their original
   virtual coordinates and registration points. Coverage must account for
   the selected ink, including operations that affect transparent rectangles.
   Immutable paired masks can become reusable coverage variants; preserve the
   original source for other inks and for live member/mask edits.
3. Combine static, composition-compatible layer groups and optionally rasterize
   repeated immutable animation fragments. Preserve layer order and palette
   dependencies. Independent nested playheads remain separate dependencies;
   enumerating every combination of their phases would inflate the output.
4. Cache resolved local film planes per instance using effective child state
   and dependency revisions. Keep parent transformation and composition outside
   that cache where possible. Unchanged assignments should preserve the cached
   result; edits should invalidate the affected subtree and its ancestors.
5. Reuse retained buffers and render only affected bounds. Draw cursor changes
   over the retained scene so pointer movement does not rebuild film planes.
6. Specialize constant rectangle/opacity operations and reuse sampling maps.
   The full-scene dawn shape still blends over animated content, so its
   destination-dependent work remains even after film preprocessing.
7. Queue input independently of render completion and publish diagnostics
   without waiting for the render lock.

Local-plane dependencies include child member/geometry/ink/blend/visibility,
nested instance generation and pose, mutable image contents and registration,
text/font/layout/scroll, shape pattern/texture, effective palette and mask
contents/registration. Precomputed bounds must preserve fractional scaling,
flips, Mask ink and texture phase. The confirmed Flash invalidation defects
need correction before relying on subtree generations.

## Validation required for the repair

Existing tests cover many pixel operations and interpreter paths, but direct
Flash raster tests bypass the final scene cache. Decoder tests also bypass
the complete Android video pump, pending mixer jobs and Java key lifecycle.
Passing those suites therefore missed the reproductions above.

Add focused contracts for delayed decoder output, pause-during-decode, short
controller taps, modified key releases, repeated real Flash control events,
and final-compositor invalidation. Performance checks should distinguish
service, rendering, asset loading and presentation, record allocations and
high percentiles, and fail on sustained tick loss or input timeouts.

Compare preprocessed and live rendering through edits, palette changes,
independent nested clocks, masks, flips and fractional placement. Then rerun
Birthday's actual bedroom on the EF-62N with 60 Hz service and its authored
12 Hz animation as targets. Host timings and pixel equivalence alone cannot
establish projector performance or original-player parity. N64 needs its own
memory and pacing qualification.

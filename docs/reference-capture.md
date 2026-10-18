# Original projector reference

`director64 reference --game findus-workshop` captures the workshop's original
projector in an isolated Wine container using its selected source. Build the
container with `mise run reference-toolchain`. Optional `--replay`, `--output`,
`--source`, `--iso` and `--windows-version` arguments belong to this game's adapter.
Outputs stay under the selected source workspace's `reference/` directory.

The original-projector reference is independent of native and N64 evidence.
Its sandbox does not use normal user Wine or emulator save directories.

## Native film channels and playback

The Birthday original projector, using the **8.5.1r104 DIRAPI/IML32 engine and
MX9 launcher under Wine 8**, qualified imported-film cast relocation and live
film playback. Runtime DLL hashes, raw observations and local source-media
copies remain in the ignored audit workspace. These are original-projector
observations, separate from ScummVM agreement and N64 execution.

A film's owning cast library supplies its KEY-linked `ccl ` relocation table.
The encoded cast word indexes that table, including index zero; `0xffff`
selects the owning library. Independent original controls changed table entries
zero and one to a separate valid sound cast and selected that cast's sound.
An independently colored bitmap control confirmed the same relocation for
visual channels. The table also contains variable-length authored paths;
external entries resolve through declared source aliases rather than a guessed
member number or a search for the first sound. Earlier silent controls with
unsorted rebuilt KEY entries were invalid and were not used as evidence.

Film child getters expose authored coordinates. A measured film with source
rectangle `(112,363,341,489)` placed at stage rectangle `(0,0,229,126)` returned
child `loc=point(226,426)` and the authored source rectangle. Assigning a new
location returned that assigned location. A rectangle read before another
presentation retained the original rectangle in that experiment; the precise
rectangle-cache timing has not yet been independently qualified.

Fresh film instances were inspected in a closed movie window, with authored
scripts disabled. Before the first presentation the root reported frame 1 and
its child had no active member. The first explicit `updateStage` held the root
at frame 1 and initialized nested playheads to frames 2, 3 and 4. Each subsequent
presentation advanced the existing playheads once. Repeated getters without a
presentation did not advance them. Seeking the root back to frame 1 preserved
its existing children's independent playheads and advanced those children for
the seek's presentation.
The same initialization was measured for a distinct parent film referring to
the self-containing child, ruling out a special case for equal parent IDs.

The two selected recursive member references are finite in **member and pose**
space: each initially deeper instance reaches a later pose, and the self-reference
ends at frame 6. Omitting the child changed visible pixels in the original, so
conversion must retain the reference. The runtime uses bounded independent
instances and packed per-channel state changes; it does not flatten the cycle
away or reuse the parent's frame for every descendant. Permanently recursive
active graphs that exceed the runtime's explicit nesting/instance limits fail
with a diagnostic.

Source-free regressions in `tests/native/test_film_playback.c`,
`test_film_children.c` and `test_film_sounds.c` cover finite temporal recursion,
first presentation after non-advancing preparation, stable getters, parent
seek, generation reset (an implementation lifecycle regression), sparse state replay, editable empty slots, sibling
preservation, independent instances and nested sound selection. Sound priority
uses the clean ScummVM reference commit
`41ac2b31847622d0662d22c03fe6979e3b43cfbc` plus focused tests; the nested priority
matrix is not claimed as a separate original-projector comparison.

## Synthetic engine probes

The scripts in `tools/reference/*.lingo` create their own images and sprites,
measure pixels and write numerical observations. They contain no recovered
game scripts or media. Run them in an isolated original projector with its
Message window open:

```sh
python tools/reference/run-ink-probe.py CONTAINER tools/reference/ink-precision.lingo
```

The container needs Wine, Python, `xdotool`, the selected projector and its
FileIO Xtra. The runner uses its existing Wine prefix/display, submits
CR-terminated Lingo, requires a completion marker and writes results under
ignored `build/reference/ink-probes/`. It records hashes and PE version
strings of the executable and runtime DLLs actually mapped in the process.
Launching the projector and enabling its Message window are edition-specific;
any local diagnostic changes to recovered movies stay in ignored workspaces.

### Director 8.5 observations

Birthday was measured under Debian Wine 8.0, Xvfb at 24-bit colour, with
original `DIRAPI.DLL` and `IML32.DLL` **8.5.1r104**. Its thin launcher reports
**9.0r383**, and `PROJ.DLL` reports **9.0r371**. The launcher version alone
does not identify the pixel engine. The three runtime DLLs are byte-identical
on the selected Löwenzahn 6, 7, 8 and Optik editions. Advent's 8.5.1r102
runtime is different and was not measured in this experiment.

| Module | SHA-256 |
| --- | --- |
| DIRAPI.DLL | `b355ec36769ab2375c060cfe9797c1398bfbb340b5032c7dab0e10139b35d777` |
| IML32.DLL | `c21d2130a29aef1f3e606914dd187bd8b39481c01c964e705299e7ca6f5f13ba` |
| PROJ.DLL | `66527d87422569a5975a95920589452d4a9e8ceaa85f004d3de8bc1fd303dc9a` |

The probes cover component arithmetic, tint rounding, partial blend, exterior
matte, image depth and `useAlpha`. They compare `copyPixels` and stage
composition, which use different blend weights. Stage RGB tint uses
`sprite.color`/`sprite.bgColor`; legacy `foreColor`/`backColor` coerce palette
indices in this engine. Measured facts include:

- Subtract Pin clamps `destination - source` at zero. Lightest and Darkest
  compare each RGB component separately.
- For inks 40/41, let `k = background + (background == 255)` per component.
  Darken is `min(255, foreground + ((source * k) >> 8))`. Lighten adds
  `255 - ((255 * k) >> 8)` before clamping. Combining shifted terms changes
  the measured rounding.
- Partial stage blend overrides arithmetic ink. Its source weight is
  `(percent * 255 + 49) / 100`, with denominator 256. Inks 40/41 blend raw
  source; other tested inks first apply foreground/background bitwise tint.
- Partial `copyPixels` uses `blendLevel + (blendLevel >> 7)` out of 256 and
  applies that tint for all tested inks, including 40/41.
- Copy preserves its rectangle when `useAlpha` is false, including partial
  blend and one-bit images. The baseline audit's ScummVM-derived claim that
  blended Copy always needs an exterior matte was wrong for this engine.
- A missing or non-bitmap next member makes Mask ink fall back to Matte.
  A bitmap mask is used regardless of its name. A smaller mask is centered
  on the source; a grey value of 128 gives `(126, 0, 128)` for red over blue.
  `ink-mask.lingo` and `ink-mask-bitmap.lingo` exercise these cases separately.
- Filled shapes use the same partial-stage blend rounding as bitmaps:
  50-percent red over blue gives `(126, 0, 128)`. `ink-shape.lingo` covers
  eleven blend levels with inks 0, 8 and 32. Its explicit pattern 1 matters:
  a newly created shape with the default empty pattern draws nothing.
- The four-bit `#systemMac` image lookup differs from the classic Macintosh
  OS 16-color CLUT. `palette-mac-four.lingo` sets every palette index and
  copies the image into 32-bit RGB, measuring the conversion without a stage
  screenshot. The converter fixture retains all sixteen numerical results.

Native pixel fixtures retain numerical observations from these synthetic
inputs. Raw output, screenshots and recovered media remain ignored. Wine
observations are distinct from original Windows hardware measurements and
do not establish behavior for every Director version or game route.

### Texture color conversion

The eight default texture resources are read from the selected edition's
original engine DLL during local conversion. Their pixels are never stored
in the repository. In the measured 8.5.1r104 engine, an RGB color passes
through a five-bit-per-channel lookup into the system Windows palette.
Nearest-color searches do not reproduce it.

`pattern-grid.lingo` measures 32,768 generated RGB inputs in sixteen batches
of eight 256-color synthetic brush resources. The compressed numerical
lookup in `texture_palette_data.rs` represents that measured function; it
contains palette indices, not texture or capture pixels. Its generator is
`tools/reference/generate_texture_quantizer.py`. The inferred truncation of
the low three bits matches all 512 earlier independent colors and a further
2,048 deterministic unaligned RGB inputs (seed `0x64c0ffee`). Applying it to
texture 58 and preserving its stage phase matches 8,192 original-player
pixel comparisons at two origins. Raw inputs and receipts remain ignored.
This evidence qualifies that engine; other engine revisions need their own
comparison.

`tools/reference/generate_texture_probe.py ENGINE OUTPUT --sha256 SHA256`
creates the sixteen ignored DLL copies and their generated input ledger.
It changes bitmap palettes/pixels and dimensions only. Restart the isolated
engine for each copy, run `pattern-grid.lingo`, and restore the original DLL
in a `finally` block. `--unaligned` creates one independent 2,048-color batch.


### Authored textures and Director 7 colors

The original 7.0.2r85 Windows engine was tested independently under Wine 8,
using a separate prefix and the selected edition's unmodified engine code.
Its DIRAPI SHA-256 is
`a30b3bcc28b42d1b0491b46a9f29e33d4e99327387892b0141df94b695090155`;
IML32 is `054cdc6a2f49e68e0a6f819e2e0f4b8a2bdf1ee15f068ced727b72ec9af823ab`.

`texture-authored.lingo` creates synthetic rectangles using the loaded movie's
texture table. Authored eight-bit bitmap textures retain their source palette
indices and use the table rectangle's width/height as the repeat period;
its left/top do not crop or phase-shift the bitmap. Two valid textures match
all 8,192 pixel comparisons at origin (3,5), including global stage phase.
The third texture matches all 3,968 samples inside its source bitmap, but its
32-row period exceeds the bitmap's 31 rows. Pixels beyond that bitmap are not
recoverable source data. The converter retains this unused slot as unavailable
with an explicit source diagnostic; selecting it fails instead of synthesizing
an extra row.

Changing the active palette changes these brush colors on a 32-bit stage.
`palette-win-eight.lingo` also independently measures Windows palette entry 246
as RGB(166,202,240); the previous converter value used green 200. The shared
runtime palette now uses the measured value.

`palette-stage-colors.lingo` and six direct RGB shape samples distinguish
palette lookup from shape rendering. Original 7.0.2 Copy foreground reduces
RGB to five bits per channel and expands those bits on output: for example
RGB(18,52,86) becomes RGB(16,49,82), and RGB(128,128,128) becomes RGB(132,132,132).
Texture pixels retain their palette's full eight-bit values. In this engine,
assigning a number to `sprite.color` raises `Color expected`; numeric palette
indices use `sprite.foreColor` instead. Focused tests preserve both behaviors.

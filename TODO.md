# TODO

## Findus Geist: finish opening-scene performance work

**Passes hardware pacing qualification since the 2026-11-01 compositor round
(below); headroom is thin.** Originally measured on 2026-11-01 with
production revision `f9968ce` on the EPSON HB73 projector (Android 14,
`armeabi-v7a`). All 20 updated APKs were installed and checksum-verified,
preserving existing app data, before the test.

The translated-film clipping and idle-scheduling fixes improve render cost by
about 3×, but software compositing remains the bottleneck:

| Metric | Before fixes | Current |
| --- | ---: | ---: |
| Service ticks/s | 19.58 | 56.62 |
| Redraws/s | 4.90 | 14.16 |
| Render cost per redraw | 196.55 ms | 65.18 ms |
| Time spent rendering | 96.2% | 92.3% |
| Script alerts | 0 | 0 |

Both measurements cover 30 consecutive pacing windows in the rainy `LOGIN.DXR`
scene: 30.64 seconds before and 31.44 seconds after. The baseline held frame 75;
the current sample includes frames 75, 350, 362 and 370. The current run loses
about 1.83 seconds of service time and falls below 95% of the 60 Hz service
target for five consecutive windows. Service and presentation account for
4.2% and 3.5% of the measured time respectively; the worker saturates one CPU
core.

- [x] Profile native drawing costs (`tools/android/native_profile.py`, Android 14
  x86_64 emulator, 4 cores, 2026-11-01). Each login-scene redraw composites
  about 720k bitmap pixels, all Mask ink 9: a full 800×600 layer at partial
  opacity plus about 240k opaque-opacity pixels. 89% of samples were
  `draw_bitmap`'s general per-pixel path and the generic `dg_rgba_ink`.
- [x] Reduce that cost bit-exactly. Emulator render cost per redraw, same scene:

  | Change | ms/redraw |
  | --- | ---: |
  | Baseline (`f9968ce`) | ~4.5 |
  | Raster-aligned film stacks map columns/rows once (`dg_film_transform_x/y`) | ~4.5 |
  | Packed-channel Mask9 kernel (`dg_rgba_mask_ink`) | 3.8 |
  | Final per-axis source tables for independent axes | 2.7 |
  | Rows of large draws across 4 cores (OpenMP) | 2.0 |

  `test_ink_kernels.c` checks the kernel against `dg_rgba_ink` for every
  coverage and opacity; the film/compositor pixel contracts also run with four
  threads, and the `film-axis-phase` mutation must fail. Remaining emulator
  cost: source/destination loads of the full-stage layers (memory bandwidth)
  and serial scene copies. About 39 redraws/s are legitimate there (rain at
  10 FPS, ghosts at the 30 Hz idle rate), so the next algorithmic step is
  damage-rectangle redraws: most redraws only move the ghosts.
- [ ] Add a portable cost regression for the identified hot path. The existing
  `mise run test-performance` suite passes all 25 journeys across 20 games,
  including `rainy-opening`, but does not reject this remaining hardware cost.
- [x] Rebuild, update the projector and repeat the production opening test
  (2026-11-01, uncommitted working tree on `f9968ce`, EPSON HB73,
  `armeabi-v7a`, update install preserving app data, display awake, publisher
  reel run without input). The pacing gate passes:

  | Metric | `f9968ce` | Current |
  | --- | ---: | ---: |
  | Service ticks/s | 56.62 | 60.7 |
  | Redraws/s | 14.16 | 38.1 |
  | Render cost per redraw | 65.18 ms | 15.7–18.6 ms |
  | Minimum window delivery | <95% (5 consecutive) | 97.8% |
  | Dropped service time | 1.83 s | 0 |
  | Script alerts | 0 | 0 |

  40 uninterrupted login-scene windows without the profiler (frames 75, 313
  and 319); a separate 32-window run under simpleperf also passed (minimum
  98.7%). Render cost is wall time with rows split over the four cores. The
  worker is busy 69–97% of each window (render ≈60%, present ≈10%, service
  ≈6%), so headroom remains thin; frames 350–370 of the earlier sample were
  not reached in this run. Logs and `profile.json` under
  `build/profiling/ghosts-tv-20261101/`.

The authored animation runs at 10 FPS. Service delivery and redraw frequency
are separate measurements; the fix should preserve the authored cadence.
Keep logs, screenshots and APKs under ignored build/output directories.

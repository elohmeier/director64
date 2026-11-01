# TODO

## Findus Geist: finish opening-scene performance work

**Still fails hardware pacing qualification.** Measured on 2026-11-01 with
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
core. The precise remaining compositor hot path has not been profiled.

- [ ] Profile native drawing costs, including the full-screen rain/background
  passes and mask/ink blending, to identify the next optimization.
- [ ] Reduce that cost while preserving exact sampling, masks and blend rounding.
  Retain pixel-equivalence and source-free regression checks.
- [ ] Add a portable cost regression for the identified hot path. The existing
  `mise run test-performance` suite passes all 25 journeys across 20 games,
  including `rainy-opening`, but does not reject this remaining hardware cost.
- [ ] Rebuild, update the projector and repeat the production opening test:
  dismiss the screensaver, let the publisher reel finish without game input,
  then measure at least 30 uninterrupted mature login-scene windows with no
  uploads running. Sustain 60 Hz service and pass the existing pacing gate:
  no three consecutive windows below 95% delivery and zero script alerts.

The authored animation runs at 10 FPS. Service delivery and redraw frequency
are separate measurements; the fix should preserve the authored cadence.
Keep logs, screenshots and APKs under ignored build/output directories.

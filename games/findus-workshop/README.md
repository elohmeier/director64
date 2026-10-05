# Findus Workshop

The completed Pettersson und Findus port is Director64's first game adapter.

- Stick or D-pad moves the pointer. A is the mouse button.
- All four ports play together, each with its own coloured cursor
  ([shared pointer](../../docs/architecture.md#players-and-cursors)).
- The game provides seven profiles and its construction/music save files.
- The N64 ROM requires 8 MiB RDRAM and FlashRAM.

The adapter owns the Swedish source-global mappings, virtual `vakt1.txt` through
`vakt7.txt`, construction/music files, and the two-generation save journal. Its host code owns the startup equivalence audit and all original
input journeys and save/capture oracles.

Preserved runtime behavior includes rational score timing, retained elapsed time,
resumable `updateStage`/wait execution, score ownership of sprite properties,
trail/stage commits, and audio/field cleanup when movies change. These behaviors
are tested in the shared runtime; game-specific assertions live under `tests/`.

Old debugging videos and chronological fix reports were retired in the refactor.
Current build receipts and captures are generated under the selected source's
ignored build directory. The strict release command requires current-source
sanitized journeys, matching scenario captures, and cold/corrupt-save boot checks.

The renamed ROM was validated with 77 sanitized native journeys, 33 Gopher64
scenarios, and cold/corrupt-save boot checks. The release
receipt written under the selected build directory pins the ROM, SDK and supporting evidence. This validation did not rerun hardware
or certify original-projector presentation fidelity.

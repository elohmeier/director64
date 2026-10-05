# Source-accountability schema version 1

`director64 assets --game <slug>` verifies the supported ISO and every extracted ISO 9660
file, then writes ignored `build/<game>/<source-id>/analysis/source/manifest.json`,
`obligations.json`, `summary.json` and content-addressed raw score chunks.
This recovery schema is separate from the runtime IR. `runtime_ir_frozen` is false;
raw score recovery is not native implementation or reference matching.

The actual host parser is the npm-locked ProjectorRays 1.1.1 WASM, whose binary
SHA-256 is checked before extraction. `config/provenance.toml` distinguishes that
binary from the pinned upstream C++ checkout used for format work. Outputs omit
host paths and timestamps and sort objects and source records deterministically.

IDs use a kind prefix plus SHA-256 of a JSON locator array. File locators contain
the original filename and source SHA-256. Cast-library/member IDs retain original
Director library and member numbers. Resources retain FourCC and section ID;
scripts retain library/script ID and the linked raw Lscr hash. Handler ordinals
and bytecode instruction offsets distinguish sites even when names repeat or
decompiled text is reformatted. The ledger never assigns a reviewed disposition
automatically: all initial obligations are unresolved with empty evidence.

The generated records cover the workshop source denominator:

| Item | Recovered |
| --- | ---: |
| DXR/CXT source files | 38 |
| Cast members | 7,203 |
| Raw resources | 16,799 |
| Media script blocks | 1,524 |
| Media handlers | 1,576 |
| Media bytecode sites | 75,498 |
| Bitmap/sound member references | 4,552 / 955 |
| Discovered movie references | 63 |
| Raw score/label/info chunks | 107 (1,000,999 bytes) |

`KEY*` has capacity and used counts. Only the used entries are live resource
links; the remaining raw bytes are preserved by the resource hash. Treating all
capacity slots as live incorrectly reports 4,350 missing links on this disc.
All live media links now resolve. The remaining recovery flags are the 107 raw
score-related chunks. The five type-14 members are classified as transitions:
four type-23/2,000ms members and one type-52/350ms member, with raw flags retained.
The numeric layouts match the primary ScummVM definitions pinned by the score
recovery tool; this is structural classification, not visual conformance. Discovery edges
remain string references, not proof of executable transitions.

The disc also contains an embedded Director movie inside `ANNAT/PETT16.EXE`.
Its 10 script blocks and 13 handlers were outside the workshop's 1,524-script media
denominator. They are inventoried separately as `supplemental_projector` and are
included in the obligation ledger. Thus whole-disc script review must account for
1,534 blocks, while preserving the original 1,524-block media measurement.
The adapter copies only the embedded archive prologue into a temporary in-memory
parser view, keeping absolute mmap/chunk offsets and all source files unchanged.
Its path setup and GINTRO boot route informed the Wine reference runner.

Reference tolerances, source no-op/unreachable dispositions, native mappings and
runtime semantics require reviewed evidence. No generated discovery record can
raise those coverage dimensions on its own.

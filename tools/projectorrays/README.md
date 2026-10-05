# Director 5–8 and 10 structural recovery

This recovers bounded structures from `VWSC`, `VWLB` and
`VWFI`; every output explicitly keeps `runtime_ir_frozen`, `semantic_complete` and
`reference_verified` false. It does not resolve the source obligation ledger or
qualify a source scene.

`uv run --locked director64 assets --game <slug>` verifies/extracts the user's ISO, builds the native probe,
and writes ignored results to `build/<game>/<source-id>/analysis/score-recovery`. The manifest
records source and output hashes, native executable hash, implementation hashes and
the exact parser extension commit. Labels keep their source bytes; no text encoding
is assumed. Channel reconstruction includes a byte-validity mask, so absent initial
values are not mistaken for proven zero-valued Director defaults.

## Native boundary and reproducibility

The extension is commit `562c5690aafb3c35850b756df6c11591055afae9`, based on upstream
ProjectorRays `6f9bcebf626b43719abe2affcbbcb041d154d666`. `pin.json`,
`upstream.patch`, the two `score_recovery` overlay files and `extension.commit`
reconstruct that exact Git tree and commit without an external fork or push.
`mise run projectorrays-build` checks both hashes before compiling the shared
parser. Existing tracked edits in the isolated checkout cause a failure.

The default task builds a standard C++17 probe from the pinned checkout. The
conversion manifest called this interface `standalone-probe`. The pipeline
now runs the Rust port, `compiler/src/convert/score.rs`, which reproduces this
probe's JSON byte for byte and records itself as `director64-convert`. The optional
`mise run projectorrays-build -- --upstream` also builds the native ProjectorRays CLI;
it needs upstream's Boost headers, zlib and mpg123 development dependencies.
Use ordinary `CXX`, `CPPFLAGS` and `LDFLAGS` for their local paths. The libdragon
toolchain is unaffected. `PROJECTORRAYS_SOURCE` optionally selects a local upstream
clone; otherwise the task checks the conventional sibling checkout, then the
public pinned upstream repository.

The integration patch adds a score chunk class and JSON dispatch at the upstream
resource boundary. The native CLI was compiled and its `--dump-json` recovery
objects were compared with the probe for PINTRO, VEMORY, KONSTR01, GARDEN and SNIKBOD:
all fifteen objects matched. The complete local probe pass covers 110 resource
references including the embedded projector: 7,154 frames, 221 labels and 19,430
deltas. It preserves 466,474 unindexed tail bytes as unresolved data. Those bytes
must not be dropped from a source-faithful compiler.

## Format references and limits

The implementation is original MPL-2.0 code. These primary sources supply format
facts, not copied implementation, and are pinned to ScummVM commit
`41ac2b31847622d0662d22c03fe6979e3b43cfbc`:

- [score.cpp](https://github.com/scummvm/scummvm/blob/41ac2b31847622d0662d22c03fe6979e3b43cfbc/engines/director/score.cpp):
  D6 indexed resource envelope, active offset boundaries, record-zero score header,
  16-bit frame/delta lengths, destination byte offsets, and label index/sentinel
  records. Header frame counts can disagree with the actual stream. The reference
  sizes entries from consecutive offsets and never reads the final one; every
  recovered corpus allocates at least one slot beyond the entry count whose
  offset closes the last record, so this probe reads that fencepost and indexes
  the final record instead of stranding its bytes (Willy Werkel references it
  from behavior initializers) in the unresolved tail.
- [frame.cpp](https://github.com/scummvm/scummvm/blob/41ac2b31847622d0662d22c03fe6979e3b43cfbc/engines/director/frame.cpp)
  and [frame.h](https://github.com/scummvm/scummvm/blob/41ac2b31847622d0662d22c03fe6979e3b43cfbc/engines/director/frame.h):
  six 24-byte main channels followed by up to 120 sprite records, raw cast IDs,
  location/size, ink flags and tempo fields. The spike reports raw values and
  known bytes without implementing their runtime effects.
- [movie.cpp](https://github.com/scummvm/scummvm/blob/41ac2b31847622d0662d22c03fe6979e3b43cfbc/engines/director/movie.cpp):
  file-info offset tables and preload metadata. Unclassified header fields and
  extra entries remain recoverable from the preserved bytes.
- [types.h](https://github.com/scummvm/scummvm/blob/41ac2b31847622d0662d22c03fe6979e3b43cfbc/engines/director/types.h)
  and [transition.cpp](https://github.com/scummvm/scummvm/blob/41ac2b31847622d0662d22c03fe6979e3b43cfbc/engines/director/castmember/transition.cpp):
  cast type 14 is a transition, with six bytes describing transition time, chunk
  size, transition type, flags and a big-endian duration. This classification
  does not establish N64 transition rendering fidelity.

Malformed offsets, unsupported versions, invalid deltas and expansion-budget
violations fail closed. `uv run --locked pytest tests/test_score_recovery.py` runs
synthetic cases, including every truncation of a representative score and label
resource. The corpus probe also ran with Clang address/undefined-behavior
sanitizers. No original-runtime checkpoint comparison has passed; detail records,
unindexed data, initial defaults and runtime semantics remain explicit blockers.

The D8 extension additionally accepts the layout verified in Findus Mucklas:
format 13, 48-byte channels, and up to 1,006 allocated channels. It retains the
extended RGB/rotation/skew bytes without asserting runtime support. The local
Mucklas pass recovers all 99 resources including its embedded projector: 6,005
frames, 194 labels, 29,541 deltas, and no unindexed tail bytes. D6 and D8 headers
cannot be substituted for each other.

The D5 extension accepts Löwenzahn 1's format 7 score: a direct stream with
two 24-byte main records and 48 sprite records. Direct cast/script pairs and
the raw tempo byte retain their original ownership. The selected corpus,
including its embedded launcher, has 60 score resources, 1,705 frames,
100 labels, 7,969 deltas and no unindexed score tail. D5 main-channel and
sprite layouts normalize only after structural recovery; they are never
decoded as D6 records. Director 7 also accepts the verified format-13/48-byte layout: the Christmas
calendar has 1,006 allocated channels and displayed counts through 500. Its
selected media and launcher recover 1,244 frames and 137 labels without an
unindexed tail. Director 10 (MX 2004) accepts the same format-13/48-byte
layout: every `VWSC` and film-loop `SCVW` in the Lernerfolg Deutsch 1-2 corpus
declares 1,006 allocated channels, and its 138 score resources including the
embedded projector recover 18,113 frames, 783 labels and 69,062 deltas with no
unindexed tail. Other Director versions remain rejected.
The referenced ScummVM checkout was clean at the pinned revision on 2026-09-08.

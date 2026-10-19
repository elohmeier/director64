# Original Director fonts

The converter recovers each embedded PFR1 font member into lossless cubic
OpenType (`compiler/src/convert/font.rs`). Generated font data stays under
the ignored `build/`.

PFR1 is Director's variant of Bitstream's portable font resource. The
container follows the published PFR layout; the character table and glyph
programs use compact encodings the specification does not describe, which
`compiler/src/convert/pfr.rs` implements from the behavior of the Bitstream
TrueDoc player linked into Director's `Font Xtra.x32`. Every original glyph
must decode; nothing is substituted. `compiler/src/convert/cff.rs` writes the
OpenType font with cubic control points and advances intact.
`compiler/src/convert/paige.rs` decodes Paige font tables, styles and runs
(format sources pinned in `pin.json`).

Text sizes are rasterized with libdragon's `mkfont`, built as a host tool
from the pinned submodule; the renderer receives the original ascent,
descent, line height, alignment and color. PFR hint instructions are not
exported.

Tools:

- `uv run --with unicorn tools/fonts/pfr1-verify.py` emulates the original
  player binary (found in an extracted source by SHA-256) and compares its
  outlines with the converter's for every recovered glyph; `--fuzz N` also
  checks N mutated glyph programs. Run it after changing the decoder.
- `render-text.py` renders authored single-style fields from recovered fonts
  to PNG for inspection.

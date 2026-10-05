# Original Director fonts

The converter recovers each embedded PFR1 font member into lossless cubic
OpenType (`compiler/src/convert/font.rs`), natively and in the browser
importer alike. It keeps the source PFR, the decoded outlines, the OpenType
font and a report with source, decoder, writer and output hashes. Generated
commercial font data stays under ignored `build/`.

PFR1 is Director's variant of Bitstream's portable font resource. The
container follows the published PFR layout (header, logical and physical
font records); the character table and the glyph programs use compact
encodings the published specification does not describe.
`compiler/src/convert/pfr.rs` implements them from the behaviour of the
original Bitstream TrueDoc Character Shape Player, which Director links into
its `Font Xtra.x32`: nibble-packed control-coordinate tables, a sixteen-way
outline opcode set with curve shorthands whose unencoded control coordinates
snap to the control tables in the direction of travel, 12/20-bit values, and
compound glyphs whose elements are stored just before them. Every original
glyph must decode; nothing is substituted. The OpenType writer
(`compiler/src/convert/cff.rs`) preserves cubic control points and advances
directly with 16.16 font-unit precision. It does not fit quadratic curves or
substitute fonts.

`uv run --with unicorn tools/fonts/pfr1-verify.py` is the ground truth: it
emulates the pinned original player binary (located in an extracted source
by SHA-256) and compares its outlines with the converter's for every simple
glyph of every recovered font in the build tree. `--fuzz N` does the same
for N mutations of those glyph programs. Run it after changing the decoder.

`compiler/src/convert/paige.rs` independently decodes Paige 4.0001 font
tables, styles and runs from primary format sources listed in `pin.json`. It
retains raw numbers, selects font indices rather than scanning text strings,
and rejects malformed counts, ranges and truncated sections. Source fonts not
embedded in the media remain explicit. Insertion style and initial text style
can differ.

The converter's compile stage (`compiler/src/convert/compile.rs`) resolves
source names to recovered fonts and manifests all required sizes.
`full_assets.py` passes each size through the pinned SDK's `mkfont --range all`; packed cache identity includes original OpenType hash,
size/options, SDK revision and container identity. The renderer receives the
original ascent, descent, line height, alignment and color. `font-audit.json`
records score and literal-name references; absence of those references does
not prove arbitrary computed references unreachable.

`render-text.py --model director/model.json --output-dir analysis/font-raster`
produces static PNG fields and a separate accented-character specimen using
the same pinned FreeType source as `mkfont`. These are host raster evidence,
not original-projector comparisons. PFR hint instructions are retained in the
original payload but are not exported into CFF. FreeType rasterization and the
N64 font atlas's coverage format can differ from the original text Xtra.

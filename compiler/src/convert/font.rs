//! Embedded font recovery: an original PFR1 font cast member into lossless
//! cubic OpenType plus provenance (the source PFR, its decoded outlines and
//! a report the model records). The same code runs natively and in the
//! browser importer.

use serde_json::{json, Value};

use super::cff::opentype;
use super::files::{self, Files};
use super::js::stringify_pretty;
use super::pfr::{outlines_json, parse};
use super::source::sha256;

const DECODER: &str = include_str!("pfr.rs");
const WRITER: &str = include_str!("cff.rs");
const ADAPTER: &str = include_str!("font.rs");

/// Recovers `input` (a PFR1 font) into `output`, returning its report.
pub fn recover(fs: &mut dyn Files, input: &str, output: &str, aliases: &[String]) -> Result<Value, String> {
    let raw = fs.read(input)?;
    let font = parse(&raw)?;
    let converted = opentype(&font)?;
    fs.mkdir_all(output)?;
    let key = sha256(&raw);
    let outlines = outlines_json(&font);
    let names = [format!("{key}.pfr"), format!("{key}.outlines.json"), format!("{key}.otf")];
    fs.write(&files::join(output, &names[0]), &raw)?;
    fs.write(&files::join(output, &names[1]), outlines.as_bytes())?;
    fs.write(&files::join(output, &names[2]), &converted)?;
    let mut aliases = aliases.to_vec();
    aliases.sort();
    aliases.dedup();
    let report = json!({
        "id": format!("embedded-{}", &key[..24]),
        "sourceFormat": "PFR1",
        "sourceSha256": key,
        "sourceBytes": raw.len(),
        "aliases": aliases,
        "name": font.name,
        "asset": format!("fonts/{}", names[2]),
        "sha256": sha256(&converted),
        "opentypeBytes": converted.len(),
        "unitsPerEm": font.units_per_em,
        "ascent": font.ascender(),
        "descent": font.descender(),
        "codepoints": font.glyphs.iter().map(|g| g.codepoint).collect::<Vec<_>>(),
        "glyphCount": font.glyphs.len(),
        "outlineSha256": sha256(outlines.as_bytes()),
        "conversion": {
            "parser": {
                "implementation": "compiler/src/convert/pfr.rs",
                "sha256": sha256(DECODER.as_bytes()),
                "verifiedAgainst": "the original TrueDoc player in Director's Font Xtra, by emulation (tools/fonts/pfr1-verify.py)",
            },
            "writer": "original CFF/Type2 exporter",
            "writerSha256": sha256(WRITER.as_bytes()),
            "adapterSha256": sha256(ADAPTER.as_bytes()),
            "cubicOutlinesPreserved": true,
            "missingGlyphSubstitution": false,
            "coordinatePrecision": "16.16 font units",
            "sourceHinting": "not exported",
            "originalProjectorCompared": false,
        },
        "provenanceFiles": {"source": names[0], "outlines": names[1], "opentype": names[2]},
    });
    fs.write(&files::join(output, &format!("{key}.font.json")), (stringify_pretty(&report) + "\n").as_bytes())?;
    Ok(report)
}

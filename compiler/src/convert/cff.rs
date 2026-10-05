//! A minimal OpenType/CFF writer for recovered, unhinted cubic outlines
//! (ported from the former tools/fonts/cff_font.py). Format facts: Adobe Technical
//! Notes 5176 (CFF) and 5177 (Type 2 charstrings) and the OpenType spec.
//! No curve fitting, font substitution or cubic-to-quadratic conversion.

use std::collections::BTreeMap;

use super::pfr::{Command, Font};

type R<T> = Result<T, String>;

fn u16b(n: i64) -> R<[u8; 2]> {
    u16::try_from(n).map(u16::to_be_bytes).map_err(|_| format!("value {n} does not fit an unsigned 16-bit field"))
}
fn s16b(n: i64) -> R<[u8; 2]> {
    i16::try_from(n).map(i16::to_be_bytes).map_err(|_| format!("value {n} does not fit a signed 16-bit field"))
}
fn pad(b: &[u8]) -> Vec<u8> {
    let mut v = b.to_vec();
    v.resize(b.len().div_ceil(4) * 4, 0);
    v
}
fn checksum(b: &[u8]) -> u32 {
    pad(b).chunks(4).fold(0u32, |s, c| s.wrapping_add(u32::from_be_bytes(c.try_into().unwrap())))
}

fn index(items: &[Vec<u8>]) -> Vec<u8> {
    if items.is_empty() {
        return vec![0, 0];
    }
    let mut offsets = vec![1usize];
    for item in items {
        offsets.push(offsets.last().unwrap() + item.len());
    }
    let last = *offsets.last().unwrap();
    let size = (usize::BITS - last.leading_zeros()).div_ceil(8).max(1) as usize;
    let mut out = (items.len() as u16).to_be_bytes().to_vec();
    out.push(size as u8);
    for o in offsets {
        out.extend_from_slice(&(o as u64).to_be_bytes()[8 - size..]);
    }
    for item in items {
        out.extend_from_slice(item);
    }
    out
}

fn integer(n: i64) -> Vec<u8> {
    match n {
        -107..=107 => vec![(n + 139) as u8],
        108..=1131 => vec![((n - 108) / 256 + 247) as u8, ((n - 108) % 256) as u8],
        -1131..=-108 => vec![((-n - 108) / 256 + 251) as u8, ((-n - 108) % 256) as u8],
        -32768..=32767 => {
            let mut v = vec![0x1c];
            v.extend_from_slice(&(n as i16).to_be_bytes());
            v
        }
        _ => {
            let mut v = vec![0x1d];
            v.extend_from_slice(&(n as i32).to_be_bytes());
            v
        }
    }
}

fn real(value: f64) -> Vec<u8> {
    let text = format!("{value:.14}");
    let text = text.trim_end_matches('0').trim_end_matches('.');
    let mut nibbles: Vec<u8> = text
        .chars()
        .map(|c| match c {
            '.' => 10,
            '-' => 14,
            d => d.to_digit(10).unwrap() as u8,
        })
        .collect();
    nibbles.push(15);
    if nibbles.len() % 2 == 1 {
        nibbles.push(15);
    }
    let mut out = vec![0x1e];
    out.extend(nibbles.chunks(2).map(|p| p[0] << 4 | p[1]));
    out
}

fn coordinate(n: f64) -> R<Vec<u8>> {
    if !n.is_finite() || !(-32768.0..32768.0).contains(&n) {
        return Err("Type 2 coordinate outside representable range".into());
    }
    if n == n.trunc() {
        return Ok(integer(n as i64));
    }
    let mut v = vec![0xff];
    v.extend_from_slice(&((n * 65536.0).round_ties_even() as i32).to_be_bytes());
    Ok(v)
}

struct Glyph<'a> {
    codepoint: i64,
    advance: i64,
    contours: &'a [Vec<Command>],
}

fn charstring(glyph: &Glyph) -> R<Vec<u8>> {
    // nominalWidthX and defaultWidthX are zero: the first operator carries
    // the explicit original advance. Type 2 closes each contour implicitly.
    let mut out = coordinate(glyph.advance as f64)?;
    let (mut x, mut y) = (0.0, 0.0);
    for contour in glyph.contours {
        if !matches!(contour.first(), Some(Command::Move(..))) {
            return Err("PFR contour lacks initial moveto".into());
        }
        for command in contour {
            let (args, op, end) = match *command {
                Command::Move(ex, ey) => (vec![ex - x, ey - y], 21, (ex, ey)),
                Command::Line(ex, ey) => (vec![ex - x, ey - y], 5, (ex, ey)),
                Command::Curve(ex, ey, x1, y1, x2, y2) => {
                    (vec![x1 - x, y1 - y, x2 - x1, y2 - y1, ex - x2, ey - y2], 8, (ex, ey))
                }
            };
            for v in args {
                out.extend(coordinate(v)?);
            }
            out.push(op);
            (x, y) = end;
        }
    }
    out.push(14);
    Ok(out)
}

/// The name as a PostScript name, which the CFF name index and name ID 6
/// must be (OpenType `name` table): printable ASCII without spaces or
/// `[](){}<>/%`, each other character as a hyphen. Browsers' font
/// sanitizers refuse a font otherwise ("Souvenir Bold" in Lernerfolg).
fn postscript_name(name: &str) -> String {
    name.chars().map(|c| if c.is_ascii_graphic() && !"[](){}<>/%".contains(c) { c } else { '-' }).collect()
}

fn cff(font: &Font, glyphs: &[Glyph], bounds: [i64; 4]) -> R<Vec<u8>> {
    let names = index(&[postscript_name(&font.name).into_bytes()]);
    let strings = index(&glyphs[1..].iter().map(|g| format!("uni{:04X}", g.codepoint).into_bytes()).collect::<Vec<_>>());
    let subrs = index(&[]);
    let mut charset = vec![0u8];
    for i in 0..glyphs.len() - 1 {
        charset.extend_from_slice(&((391 + i) as u16).to_be_bytes());
    }
    let chars = index(&glyphs.iter().map(charstring).collect::<R<Vec<_>>>()?);
    let upem = font.units_per_em as f64;
    let dictionary = |charset_offset: u32, chars_offset: u32| {
        // Offsets use fixed-width DICT integers so the Top DICT INDEX keeps
        // its length when they are resolved.
        let mut d: Vec<u8> = bounds.iter().flat_map(|&v| integer(v)).collect();
        d.push(5);
        for v in [1.0 / upem, 0.0, 0.0, 1.0 / upem, 0.0, 0.0] {
            d.extend(real(v));
        }
        d.extend_from_slice(&[0x0c, 0x07, 0x1d]);
        d.extend_from_slice(&charset_offset.to_be_bytes());
        d.extend_from_slice(&[0x0f, 0x1d]);
        d.extend_from_slice(&chars_offset.to_be_bytes());
        d.push(0x11);
        d
    };
    let top = index(&[dictionary(0, 0)]);
    let offset = (4 + names.len() + top.len() + strings.len() + subrs.len()) as u32;
    let top = index(&[dictionary(offset, offset + charset.len() as u32)]);
    let mut out = vec![1, 0, 4, 4];
    for part in [names, top, strings, subrs, charset, chars] {
        out.extend(part);
    }
    Ok(out)
}

fn cmap(glyphs: &[Glyph]) -> Vec<u8> {
    let mut mapping: Vec<(i64, i64)> = glyphs.iter().enumerate().skip(1).map(|(i, g)| (g.codepoint, i as i64)).collect();
    mapping.sort();
    let mut groups: Vec<[i64; 3]> = Vec::new();
    for (code, glyph) in mapping {
        match groups.last_mut() {
            Some(g) if code == g[1] + 1 && glyph == g[2] + code - g[0] => g[1] = code,
            _ => groups.push([code, code, glyph]),
        }
    }
    // Format 12 covers all of Unicode; Unicode and Windows UCS-4 records
    // share the one subtable.
    let mut table = Vec::new();
    table.extend_from_slice(&12u16.to_be_bytes());
    table.extend_from_slice(&0u16.to_be_bytes());
    table.extend_from_slice(&(16 + 12 * groups.len() as u32).to_be_bytes());
    table.extend_from_slice(&0u32.to_be_bytes());
    table.extend_from_slice(&(groups.len() as u32).to_be_bytes());
    for g in &groups {
        for n in g {
            table.extend_from_slice(&(*n as u32).to_be_bytes());
        }
    }
    let mut out = Vec::new();
    for v in [0u16, 2, 0, 4] {
        out.extend_from_slice(&v.to_be_bytes());
    }
    out.extend_from_slice(&20u32.to_be_bytes());
    out.extend_from_slice(&3u16.to_be_bytes());
    out.extend_from_slice(&10u16.to_be_bytes());
    out.extend_from_slice(&20u32.to_be_bytes());
    out.extend(table);
    out
}

fn names(font: &Font) -> Vec<u8> {
    let values = [
        (1u16, font.name.clone()),
        (2, "Regular".to_string()),
        (3, format!("Recovered-{}", font.name)),
        (4, font.name.clone()),
        (5, "Version 1.000".to_string()),
        (6, postscript_name(&font.name)),
    ];
    let (mut data, mut records) = (Vec::new(), Vec::new());
    for (key, value) in &values {
        let text: Vec<u8> = value.encode_utf16().flat_map(u16::to_be_bytes).collect();
        for v in [3u16, 1, 0x0409, *key, text.len() as u16, data.len() as u16] {
            records.extend_from_slice(&v.to_be_bytes());
        }
        data.extend(text);
    }
    let mut out = Vec::new();
    for v in [0u16, values.len() as u16, 6 + 12 * values.len() as u16] {
        out.extend_from_slice(&v.to_be_bytes());
    }
    out.extend(records);
    out.extend(data);
    out
}

fn outline_bounds(glyph: &Glyph) -> [i64; 4] {
    let mut points = Vec::new();
    for contour in glyph.contours {
        for c in contour {
            match *c {
                Command::Move(x, y) | Command::Line(x, y) => points.push((x, y)),
                Command::Curve(x, y, a, b, c2, d) => points.extend([(x, y), (a, b), (c2, d)]),
            }
        }
    }
    if points.is_empty() {
        return [0; 4];
    }
    let fold = |f: fn(f64, f64) -> f64, init: f64, axis: usize| {
        points.iter().map(|p| if axis == 0 { p.0 } else { p.1 }).fold(init, f)
    };
    [
        fold(f64::min, f64::INFINITY, 0).floor() as i64,
        fold(f64::min, f64::INFINITY, 1).floor() as i64,
        fold(f64::max, f64::NEG_INFINITY, 0).ceil() as i64,
        fold(f64::max, f64::NEG_INFINITY, 1).ceil() as i64,
    ]
}

/// The OpenType (CFF-flavoured) font of a recovered PFR font.
pub fn opentype(font: &Font) -> R<Vec<u8>> {
    let upem = font.units_per_em;
    if !(16..=16384).contains(&upem) || font.metrics_resolution != upem {
        return Err("Unsupported PFR units/metrics scaling".into());
    }
    // PFR physical contours are positive-up, as CFF is. The D8 composer
    // writes a negative logical Y matrix, the MX 2004 one a positive matrix
    // around identically oriented contours (corpus-verified: ascenders
    // positive, descenders negative in both), so both export unchanged.
    if font.matrix != [256, 0, 0, -256] && font.matrix != [256, 0, 0, 256] {
        return Err("Unsupported PFR logical transform".into());
    }
    let mut glyphs = vec![Glyph { codepoint: -1, advance: 0, contours: &[] }];
    glyphs.extend(font.glyphs.iter().map(|g| Glyph {
        codepoint: g.codepoint as i64,
        advance: g.advance,
        contours: &g.contours,
    }));
    let codes: Vec<i64> = glyphs[1..].iter().map(|g| g.codepoint).collect();
    let mut unique = codes.clone();
    unique.sort();
    unique.dedup();
    if unique.len() != codes.len() || codes.iter().any(|c| !(0..=0x10ffff).contains(c)) {
        return Err("Invalid PFR Unicode mapping".into());
    }
    let boxes: Vec<[i64; 4]> = glyphs.iter().map(outline_bounds).collect();
    let bounds = [
        boxes.iter().map(|b| b[0]).min().unwrap(),
        boxes.iter().map(|b| b[1]).min().unwrap(),
        boxes.iter().map(|b| b[2]).max().unwrap(),
        boxes.iter().map(|b| b[3]).max().unwrap(),
    ];
    let advances: Vec<i64> = glyphs.iter().map(|g| g.advance).collect();
    let (asc, desc) = (font.ascender(), font.descender());
    let mut head = Vec::new();
    for v in [0x10000u32, 0x10000, 0, 0x5f0f3cf5] {
        head.extend_from_slice(&v.to_be_bytes());
    }
    head.extend(u16b(3)?);
    head.extend(u16b(upem)?);
    head.extend_from_slice(&[0; 16]);
    for v in bounds {
        head.extend(s16b(v)?);
    }
    head.extend(u16b(0)?);
    head.extend(u16b(8)?);
    for v in [2, 0, 0] {
        head.extend(s16b(v)?);
    }
    let mut hhea = 0x10000u32.to_be_bytes().to_vec();
    for v in [asc, desc, 0] {
        hhea.extend(s16b(v)?);
    }
    hhea.extend(u16b(*advances.iter().max().unwrap())?);
    let min_rsb = advances.iter().zip(&boxes).map(|(a, b)| a - b[2]).min().unwrap();
    for v in [boxes.iter().map(|b| b[0]).min().unwrap(), min_rsb, boxes.iter().map(|b| b[2]).max().unwrap(), 1, 0, 0, 0, 0, 0, 0, 0] {
        hhea.extend(s16b(v)?);
    }
    hhea.extend(u16b(glyphs.len() as i64)?);
    let mut os2 = vec![0u8; 96];
    let average = (advances.iter().sum::<i64>() as f64 / advances.len() as f64).round_ties_even() as i64;
    let mut put = |at: usize, bytes: [u8; 2]| os2[at..at + 2].copy_from_slice(&bytes);
    put(0, u16b(4)?);
    put(2, s16b(average)?);
    put(4, u16b(400)?);
    put(6, u16b(5)?);
    put(8, u16b(0)?);
    put(62, u16b(0x40)?);
    put(64, u16b(*codes.iter().min().unwrap())?);
    put(66, u16b((*codes.iter().max().unwrap()).min(65535))?);
    put(68, s16b(asc)?);
    put(70, s16b(desc)?);
    put(72, s16b(0)?);
    put(74, u16b(asc.max(bounds[3]))?);
    put(76, u16b((-desc).max(-bounds[1]))?);
    // The authored Paige ascent/descent drive field layout separately; these
    // metrics keep the physical font's extents, accents included.
    put(86, s16b(0)?);
    put(88, s16b(0)?);
    put(90, u16b(0)?);
    put(92, u16b(32)?);
    put(94, u16b(1)?);
    os2[58..62].copy_from_slice(b"D64 ");
    let mut hmtx = Vec::new();
    for (a, b) in advances.iter().zip(&boxes) {
        hmtx.extend(u16b(*a)?);
        hmtx.extend(s16b(b[0])?);
    }
    let mut maxp = 0x5000u32.to_be_bytes().to_vec();
    maxp.extend(u16b(glyphs.len() as i64)?);
    let mut post = 0x30000u32.to_be_bytes().to_vec();
    post.extend_from_slice(&[0; 28]);
    let tables: BTreeMap<&[u8; 4], Vec<u8>> = BTreeMap::from([
        (b"CFF ", cff(font, &glyphs, bounds)?),
        (b"OS/2", os2),
        (b"cmap", cmap(&glyphs)),
        (b"head", head),
        (b"hhea", hhea),
        (b"hmtx", hmtx),
        (b"maxp", maxp),
        (b"name", names(font)),
        (b"post", post),
    ]);
    let count = tables.len() as u16;
    let power = 1u16 << (15 - count.leading_zeros());
    let mut result = b"OTTO".to_vec();
    for v in [count, power * 16, power.trailing_zeros() as u16, count * 16 - power * 16] {
        result.extend_from_slice(&v.to_be_bytes());
    }
    let mut offset = 12 + 16 * tables.len();
    let mut head_offset = 0;
    for (tag, data) in &tables {
        result.extend_from_slice(*tag);
        result.extend_from_slice(&checksum(data).to_be_bytes());
        result.extend_from_slice(&(offset as u32).to_be_bytes());
        result.extend_from_slice(&(data.len() as u32).to_be_bytes());
        if *tag == b"head" {
            head_offset = offset;
        }
        offset += pad(data).len();
    }
    for data in tables.values() {
        result.extend(pad(data));
    }
    let adjustment = 0xb1b0afbau32.wrapping_sub(checksum(&result));
    result[head_offset + 8..head_offset + 12].copy_from_slice(&adjustment.to_be_bytes());
    Ok(result)
}

#[cfg(test)]
mod tests {

    #[test]
    fn postscript_names_are_printable_ascii_without_delimiters() {
        assert_eq!(postscript_name("Souvenir Bold_400_000"), "Souvenir-Bold_400_000");
        assert_eq!(postscript_name("Pettson_400_000"), "Pettson_400_000");
        assert_eq!(postscript_name("A(b)/c%ü"), "A-b--c--");
    }

    use super::*;
    use crate::convert::pfr::{Command::*, Glyph as PfrGlyph};

    fn fixture() -> Font {
        let glyph = |codepoint, advance, contours| PfrGlyph { codepoint, advance, source_offset: 0, source_length: 0, contours };
        Font {
            name: "SourceFont".into(),
            units_per_em: 2048,
            metrics_resolution: 2048,
            bbox: [0, -600, 900, 1800],
            matrix: [256, 0, 0, -256],
            glyphs: vec![
                glyph(65, 900, vec![vec![Move(0.0, 0.0), Curve(200.0, 0.0, 25.0, 100.0, 175.0, 100.0), Line(0.0, 0.0)]]),
                glyph(196, 940, vec![vec![Move(0.25, 0.0), Line(100.0, 500.0), Line(200.0, 0.0)]]),
                glyph(32, 400, vec![]),
            ],
        }
    }

    fn tables(data: &[u8]) -> BTreeMap<String, (u32, usize, usize)> {
        let count = u16::from_be_bytes([data[4], data[5]]) as usize;
        (0..count)
            .map(|i| {
                let p = 12 + 16 * i;
                let word = |at: usize| u32::from_be_bytes(data[p + at..p + at + 4].try_into().unwrap());
                (String::from_utf8(data[p..p + 4].to_vec()).unwrap(), (word(4), word(8) as usize, word(12) as usize))
            })
            .collect()
    }

    #[test]
    fn cff_preserves_cubic_operators_and_original_advances() {
        let font = fixture();
        let g = &font.glyphs[0];
        let glyph = Glyph { codepoint: 65, advance: g.advance, contours: &g.contours };
        // Width, move, six relative cubic deltas (25,100,150,0,25,-100), close line, endchar.
        assert_eq!(charstring(&glyph).unwrap(), [250, 24, 139, 139, 21, 164, 239, 247, 42, 139, 164, 39, 8, 251, 92, 139, 5, 14]);
        assert_eq!(coordinate(0.25).unwrap(), b"\xff\0\0\x40\0");
        assert!(coordinate(32768.0).unwrap_err().contains("coordinate"));
    }

    #[test]
    fn opentype_checksums_unicode_mapping_and_cff_profile() {
        let data = opentype(&fixture()).unwrap();
        assert_eq!(&data[..4], b"OTTO");
        assert_eq!(checksum(&data), 0xB1B0AFBA);
        let directory = tables(&data);
        assert!(directory.contains_key("CFF ") && !directory.contains_key("glyf") && !directory.contains_key("loca"));
        let (_, at, size) = directory["maxp"];
        assert_eq!(&data[at..at + size], b"\x00\x00\x50\x00\x00\x04");
        for (tag, &(sum, at, size)) in &directory {
            let mut table = data[at..at + size].to_vec();
            if tag == "head" {
                table[8..12].fill(0);
            }
            assert_eq!(checksum(&table), sum, "{tag}");
        }
        let at = directory["cmap"].1 + 20;
        let word = |p: usize| u32::from_be_bytes(data[p..p + 4].try_into().unwrap());
        assert_eq!(u16::from_be_bytes([data[at], data[at + 1]]), 12);
        let groups: Vec<[u32; 3]> =
            (0..word(at + 12) as usize).map(|i| [0, 4, 8].map(|o| word(at + 16 + i * 12 + o))).collect();
        assert_eq!(groups, [[32, 32, 3], [65, 65, 1], [196, 196, 2]]);
        assert_eq!(data, opentype(&fixture()).unwrap());
    }

    #[test]
    fn rejects_ambiguous_glyphs_and_unhandled_logical_transforms() {
        let mut font = fixture();
        font.glyphs[1].codepoint = 65;
        assert!(opentype(&font).unwrap_err().contains("mapping"));
        let mut font = fixture();
        font.matrix = [256, 1, 0, -256];
        assert!(opentype(&font).unwrap_err().contains("transform"));
    }

    #[test]
    fn positive_logical_matrix_exports_identical_outlines() {
        // The MX 2004 composer flips the logical matrix sign around
        // identically oriented physical contours.
        let mut font = fixture();
        font.matrix = [256, 0, 0, 256];
        assert_eq!(opentype(&font).unwrap(), opentype(&fixture()).unwrap());
        font.matrix = [256, 0, 0, -255];
        assert!(opentype(&font).is_err());
    }
}

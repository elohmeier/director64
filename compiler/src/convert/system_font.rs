//! Exact, explicitly supplied system fonts. Preserve original SFNT bytes;
//! never select a similar family or synthesize absent glyphs/metrics.
use super::{
    files::{self, Files},
    source::sha256,
};
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet};
type R<T> = Result<T, String>;
fn slice(b: &[u8], p: usize, n: usize) -> R<&[u8]> {
    b.get(p..p.checked_add(n).ok_or("SFNT offset overflow")?)
        .ok_or("truncated SFNT table".into())
}
fn u16b(b: &[u8], p: usize) -> R<u16> {
    Ok(u16::from_be_bytes(slice(b, p, 2)?.try_into().unwrap()))
}
fn i16b(b: &[u8], p: usize) -> R<i16> {
    Ok(i16::from_be_bytes(slice(b, p, 2)?.try_into().unwrap()))
}
fn u32b(b: &[u8], p: usize) -> R<u32> {
    Ok(u32::from_be_bytes(slice(b, p, 4)?.try_into().unwrap()))
}
fn cmap(b: &[u8], glyphs: usize) -> R<Vec<u32>> {
    let count = u16b(b, 2)? as usize;
    let mut selected = None;
    for n in 0..count {
        let p = 4 + n * 8;
        let platform = u16b(b, p)?;
        let encoding = u16b(b, p + 2)?;
        let offset = u32b(b, p + 4)? as usize;
        if platform != 0 && !(platform == 3 && matches!(encoding, 1 | 10)) {
            continue;
        }
        let format = u16b(b, offset)?;
        if matches!(format, 4 | 12) && selected.is_none_or(|(old, _)| format > old) {
            selected = Some((format, offset));
        }
    }
    let (format, offset) = selected.ok_or("SFNT font has no supported Unicode cmap")?;
    let length = if format == 12 {
        u32b(b, offset + 4)? as usize
    } else {
        u16b(b, offset + 2)? as usize
    };
    let b = slice(b, offset, length)?;
    let mut codes = BTreeSet::new();
    if format == 12 {
        let count = u32b(b, 12)? as usize;
        if count > 65536 {
            return Err("SFNT cmap group capacity exceeded".into());
        }
        let mut previous = None;
        for n in 0..count {
            let p = 16 + n * 12;
            let start = u32b(b, p)?;
            let end = u32b(b, p + 4)?;
            let first = u32b(b, p + 8)?;
            if end < start
                || end > 0x10ffff
                || previous.is_some_and(|last| start <= last)
                || end - start > 65535
                || first as u64 + (end - start) as u64 >= glyphs as u64
            {
                return Err("invalid SFNT Unicode range".into());
            }
            for code in start..=end {
                if first + code - start != 0 {
                    codes.insert(code);
                }
            }
            previous = Some(end);
        }
    } else {
        let doubled = u16b(b, 6)? as usize;
        let count = doubled / 2;
        if doubled == 0 || doubled % 2 != 0 {
            return Err("invalid SFNT cmap segments".into());
        }
        let ends = 14;
        let starts = ends + count * 2 + 2;
        let deltas = starts + count * 2;
        let offsets = deltas + count * 2;
        let mut previous = None;
        for n in 0..count {
            let start = u16b(b, starts + n * 2)?;
            let end = u16b(b, ends + n * 2)?;
            let delta = u16b(b, deltas + n * 2)?;
            let shift = u16b(b, offsets + n * 2)? as usize;
            if start > end || previous.is_some_and(|old| start <= old) {
                return Err("invalid SFNT cmap segment order".into());
            }
            for code in start..=end {
                if code == 65535 {
                    continue;
                }
                let glyph = if shift == 0 {
                    code.wrapping_add(delta)
                } else {
                    let id = u16b(b, offsets + n * 2 + shift + (code - start) as usize * 2)?;
                    if id == 0 { 0 } else { id.wrapping_add(delta) }
                } as usize;
                if glyph >= glyphs {
                    return Err("SFNT cmap references absent glyph".into());
                }
                if glyph != 0 {
                    codes.insert(code as u32);
                }
            }
            previous = Some(end);
        }
    }
    if codes.is_empty() || codes.len() > 131072 {
        return Err("SFNT Unicode coverage is empty or excessive".into());
    }
    Ok(codes.into_iter().collect())
}
pub fn metadata(raw: &[u8], aliases: &[String]) -> R<Value> {
    if raw.len() > 32 * 1024 * 1024 || !matches!(raw.get(..4), Some(b"\0\x01\0\0") | Some(b"OTTO"))
    {
        return Err("unsupported system font container".into());
    }
    let count = u16b(raw, 4)? as usize;
    if count == 0 || count > 128 {
        return Err("SFNT table count exceeds bounds".into());
    }
    let mut tables = BTreeMap::new();
    for n in 0..count {
        let p = 12 + n * 16;
        let name = slice(raw, p, 4)?;
        let offset = u32b(raw, p + 8)? as usize;
        let length = u32b(raw, p + 12)? as usize;
        if tables.insert(name, slice(raw, offset, length)?).is_some() {
            return Err("duplicate SFNT table".into());
        }
    }
    let table = |name: &[u8]| -> R<&[u8]> {
        tables.get(name).copied().ok_or(format!(
            "missing SFNT {} table",
            String::from_utf8_lossy(name)
        ))
    };
    let head = table(b"head")?;
    if u32b(head, 12)? != 0x5f0f3cf5 {
        return Err("invalid SFNT head magic".into());
    }
    let units = u16b(head, 18)?;
    if !(16..=16384).contains(&units) {
        return Err("invalid SFNT em size".into());
    }
    let hhea = table(b"hhea")?;
    let ascent = i16b(hhea, 4)?;
    let descent = i16b(hhea, 6)?;
    let glyphs = u16b(table(b"maxp")?, 4)? as usize;
    if glyphs == 0 {
        return Err("empty SFNT glyph table".into());
    }
    let codes = cmap(table(b"cmap")?, glyphs)?;
    let names = table(b"name")?;
    let count = u16b(names, 2)? as usize;
    let strings = u16b(names, 4)? as usize;
    let mut identities = BTreeSet::new();
    let mut family = None;
    for n in 0..count {
        let p = 6 + n * 12;
        let platform = u16b(names, p)?;
        let id = u16b(names, p + 6)?;
        if !matches!(id, 1 | 4 | 6 | 16) {
            continue;
        }
        let bytes = slice(
            names,
            strings + u16b(names, p + 10)? as usize,
            u16b(names, p + 8)? as usize,
        )?;
        let text = if platform == 0 || platform == 3 {
            if bytes.len() % 2 != 0 {
                return Err("invalid UTF16 SFNT name".into());
            }
            String::from_utf16(
                &bytes
                    .chunks_exact(2)
                    .map(|v| u16::from_be_bytes([v[0], v[1]]))
                    .collect::<Vec<_>>(),
            )
            .map_err(|_| "invalid UTF16 SFNT name")?
        } else if platform == 1 && bytes.is_ascii() {
            String::from_utf8(bytes.to_vec()).unwrap()
        } else {
            continue;
        };
        if id == 1 && family.is_none() {
            family = Some(text.clone());
        }
        identities.insert(text);
    }
    if aliases.is_empty()
        || aliases
            .iter()
            .any(|alias| !identities.iter().any(|s| s.eq_ignore_ascii_case(alias)))
    {
        return Err("system font alias does not match its original SFNT identity".into());
    }
    let hash = sha256(raw);
    Ok(
        json!({"id":format!("system-{}",&hash[..24]),"sourceFormat":"SFNT","sourceSha256":hash,
        "sourceBytes":raw.len(),"sha256":hash,"aliases":aliases,"name":family.unwrap_or_else(||aliases[0].clone()),
        "asset":format!("fonts/{hash}.ttf"),"opentypeBytes":raw.len(),"unitsPerEm":units,
        "ascent":ascent,"descent":descent,"codepoints":codes,"glyphCount":glyphs,"variants":[],
        "conversion":{"originalBytesPreserved":true,"missingGlyphSubstitution":false,"sourceHinting":"preserved"}}),
    )
}
pub fn bind(
    fs: &mut dyn Files,
    media: &str,
    output: &str,
    policy: &Value,
    fonts: &mut Vec<Value>,
) -> R<()> {
    for resource in policy["system_font_resources"]
        .as_array()
        .into_iter()
        .flatten()
    {
        let path = resource["path"]
            .as_str()
            .ok_or("system font resource path missing")?;
        let expected = resource["sha256"]
            .as_str()
            .ok_or("system font resource SHA256 missing")?;
        let aliases = resource["aliases"]
            .as_array()
            .ok_or("system font aliases missing")?
            .iter()
            .map(|v| {
                v.as_str()
                    .map(String::from)
                    .ok_or("system font alias is not a string".to_string())
            })
            .collect::<R<Vec<_>>>()?;
        let archive = fs.read(&files::join(media, path))?;
        if sha256(&archive) != expected {
            return Err(format!(
                "system font resource hash mismatch: {}",
                files::file_name(path)
            ));
        }
        let raw = if let Some(entry) = resource["entry"].as_str() {
            super::font_archive::extract(&archive, entry)?
        } else {
            archive
        };
        if resource["font_sha256"]
            .as_str()
            .is_some_and(|expected| sha256(&raw) != expected)
        {
            return Err(format!(
                "extracted system font hash mismatch: {}",
                files::file_name(path)
            ));
        }
        let mut report = metadata(&raw, &aliases)?;
        if fonts.iter().any(|f| f["sha256"] == report["sha256"]) {
            continue;
        }
        if fonts.iter().any(|f| {
            f["aliases"]
                .as_array()
                .into_iter()
                .flatten()
                .filter_map(Value::as_str)
                .any(|a| aliases.iter().any(|b| a.eq_ignore_ascii_case(b)))
        }) {
            return Err("ambiguous original system font aliases".into());
        }
        report["number"] = json!(fonts.len() + 1);
        report["sources"] = json!([{"resource":files::file_name(path),"sha256":expected,"entry":resource["entry"]}]);
        fs.mkdir_all(&files::join(output, "fonts"))?;
        fs.write(
            &files::join(output, report["asset"].as_str().unwrap()),
            &raw,
        )?;
        fonts.push(report);
    }
    Ok(())
}

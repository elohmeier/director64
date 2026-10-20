//! D7+ Xtra CASt envelopes (tools/director/xtra-model.mjs). Reference:
//! ScummVM 41ac2b31847622d0662d22c03fe6979e3b43cfbc,
//! engines/director/castmember/xtra.cpp. Payload interpretation is separate.

use serde_json::{Map, Value, json};

use super::js::{hex, latin1, mac_roman, windows_1252};
use super::paige::{paige_hyperlinks, paige_styles};

pub struct Xtra {
    pub symbol: String,
    pub payload_offset: usize,
    pub payload_length: usize,
}

fn be32(b: &[u8], at: usize) -> usize {
    u32::from_be_bytes(b[at..at + 4].try_into().unwrap()) as usize
}

pub fn xtra_model(b: &[u8]) -> Result<Xtra, String> {
    if b.len() < 12 {
        return Err("truncated Xtra cast header".into());
    }
    let start = 12 + be32(b, 4);
    let size = be32(b, 8);
    if start > b.len() || size > b.len() - start || size < 8 {
        return Err("invalid Xtra cast data range".into());
    }
    let specific = &b[start..start + size];
    let length = be32(specific, 0);
    if length == 0 || length > size - 8 {
        return Err("invalid Xtra symbol length".into());
    }
    let payload_length = be32(specific, 4 + length);
    if payload_length != size - 8 - length {
        return Err("invalid Xtra payload length".into());
    }
    Ok(Xtra {
        symbol: latin1(&specific[4..4 + length]),
        payload_offset: start + 8 + length,
        payload_length,
    })
}

fn bei32(b: &[u8], at: usize) -> i64 {
    i32::from_be_bytes(b[at..at + 4].try_into().unwrap()) as i64
}

fn hex_digits(bytes: &[u8]) -> Option<u64> {
    let text = std::str::from_utf8(bytes).ok()?;
    if text.is_empty()
        || !text
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'A'..=b'F').contains(&b))
    {
        return None;
    }
    u64::from_str_radix(text, 16).ok()
}

/// An unpopulated embedded-font slot has a version/size header and no
/// font names or PFR document. Original 8.5.1r104 returns empty font names.
pub fn empty_font_xtra(payload: &[u8]) -> bool {
    payload.len() == 16
        && be32(payload, 0) == 1
        && be32(payload, 4) == 16
        && payload[8..].iter().all(|b| *b == 0)
}

/// Exact default D8.5 text descriptor without a Paige document. The
/// original projector initializes Arial 12 plain, with an empty 14px line,
/// retaining the authored width. Other missing documents remain errors.
pub fn empty_text_xtra_model(payload: &[u8], version: i64) -> Result<Map<String, Value>, String> {
    if version != 850 || payload.len() != 432 {
        return Err("text Xtra has no XMED document".into());
    }
    let (width, height) = (bei32(payload, 40), bei32(payload, 36));
    if !(1..=16384).contains(&width) || !(1..=16384).contains(&height) {
        return Err("invalid empty text Xtra bounds".into());
    }
    let expected = empty_text_layout(width, height);
    if payload != expected {
        return Err("text Xtra has no XMED document".into());
    }
    let style = json!({"fontName":"Arial","fontId":0,"size":12,"align":0,
        "ascent":11,"descent":3,"leading":0,"lineHeight":14,"color":0});
    Ok(
        json!({"width":width,"height":14,"regX":0,"regY":0,"text":"","textHex":"",
        "textEncoding":"windows-1252","textPresentation":"native-target-font",
        "textStyle":style,"textInsertStyle":style,"emptyText":true})
        .as_object()
        .unwrap()
        .clone(),
    )
}

fn empty_text_layout(width: i64, height: i64) -> Vec<u8> {
    let mut expected = vec![0u8; 432];
    for (offset, value) in [
        (20, 1u32),
        (24, 14),
        (36, height as u32),
        (40, width as u32),
        (44, 1),
        (48, 14),
        (52, 1),
        (76, 0x33544558),
        (80, 356),
        (84, u32::MAX),
        (88, 0x00320000),
        (96, 0x00010000),
        (100, 1),
        (104, 5),
        (108, 1),
        (116, 2),
        (124, 30),
        (128, 0xffffff00),
        (132, 0xffffff00),
        (136, 0xffffff00),
        (152, 0x3f800000),
        (168, 0x3f800000),
    ] {
        expected[offset..offset + 4].copy_from_slice(&value.to_be_bytes());
    }
    expected[172..181].copy_from_slice(b"NoTexture");
    expected
}

/// A D8/D8.5/D10 text Xtra's payload and XMED (Paige) document. The payload
/// grew from 76 to 432 bytes with Director 8.5's 3D text properties.
pub fn text_xtra_model(
    payload: &[u8],
    xmed: &[u8],
    version: i64,
) -> Result<Map<String, Value>, String> {
    if payload.len() != if version >= 850 { 432 } else { 76 } {
        return Err("unsupported text Xtra layout".into());
    }
    let width = bei32(payload, 40);
    let height = bei32(payload, 36);
    if width < 1 || height < 1 || width > 16384 || height > 16384 {
        return Err("invalid text Xtra bounds".into());
    }
    let magic = b"FFFF0000000600040001\x0177AA";
    if xmed.len() < 25 || &xmed[..25] != magic {
        return Err("unsupported XMED document header".into());
    }
    let mut records: Vec<(u64, Vec<u8>)> = Vec::new();
    let mut style_records: std::collections::BTreeMap<u64, (Vec<u8>, u64)> = Default::default();
    let mut segments: Vec<(u64, Vec<u8>)> = Vec::new();
    let mut p = 25usize;
    let mut extension: Option<usize> = None;
    let mut native_cache = false;
    let mut truncated_tail = false;
    // Published D8 documents already use FFFE extensions; this is
    // independent of the larger descriptor introduced with Director 8.5.
    let enhanced = version >= 800;
    while p + 1 < xmed.len() {
        if version == 800 && matches!(xmed.get(p..p + 5), Some(b"\x03TXcp" | b"\x03TXcl")) {
            if xmed.len() - p < 64 { return Err("truncated XMED TXcp section".into()) }
            extension = Some(xmed.len() - p);
            native_cache = true;
            break;
        }
        if xmed.len() - p == 3 && xmed[p..] == [3, 70, 70] {
            break;
        }
        if enhanced && xmed.len() - p == 5 && xmed[p] == 3 && &xmed[p + 1..] == b"FFFE" {
            break;
        }
        if enhanced
            && xmed[p] == 3
            && xmed.get(p + 1..p + 26) == Some(&b"FFFE0000000600040001\x0177AA"[..])
        {
            extension = Some(xmed.len() - p);
            break;
        }
        let header = xmed
            .get(p + 1..p + 21)
            .ok_or("invalid XMED record header")?;
        if !enhanced && header.starts_with(b"FFFE") {
            return Err("invalid XMED record header".into());
        }
        if xmed[p] != 3
            || !header
                .iter()
                .all(|b| b.is_ascii_digit() || (b'A'..=b'F').contains(b))
        {
            return Err("invalid XMED record header".into());
        }
        let kind = hex_digits(&header[..4]).unwrap();
        let length = hex_digits(&header[4..12]).unwrap() as usize;
        let count = hex_digits(&header[12..]).unwrap();
        let continued = kind == 2;
        // Some Director 8.5 documents end two bytes short: the trailing style
        // record 297 without its last byte and the closing 0x03.
        let short_tail = version >= 700 && kind == 297 && length >= 2 && xmed.len() + 1 == p + 20 + length;
        if !short_tail
            && (length < 1 || length > xmed.len() - p - 20 || xmed.get(p + 20 + length) != Some(&3))
            || (records.iter().any(|r| r.0 == kind) && !continued)
            || (kind == 2 && !segments.is_empty() && !continued)
        {
            return Err("invalid XMED record range".into());
        }
        if short_tail {
            style_records.insert(kind, (xmed[p + 21..].to_vec(), count));
            records.push((kind, xmed[p + 21..].to_vec()));
            truncated_tail = true;
            p = xmed.len();
            break;
        }
        let body = xmed[p + 21..p + 20 + length].to_vec();
        if kind == 2 {
            segments.push((count, body));
        } else {
            style_records.insert(kind, (body.clone(), count));
            records.push((kind, body));
        }
        p += 20 + length;
    }
    let tail = xmed.len() - p;
    let closed = truncated_tail || (xmed.get(p) == Some(&3) && (tail == 1 || tail == 3));
    if (extension.is_none() && !closed && !(enhanced && tail == 5))
        || !records.iter().any(|r| r.0 == 0)
        || !records.iter().any(|r| r.0 == 1)
    {
        return Err("incomplete XMED document".into());
    }
    // Record 19 lists Paige's special characters (curly quotes, ellipsis,
    // bullet) in the document's own character set: Windows-1252 has them at
    // 0x91-0x94, Mac Roman at 0xD2-0xD5. A document without one is decoded
    // by the version's default. Director 8.5 movies mix both.
    let windows = match style_records.get(&19).map(|(body, _)| body) {
        Some(table) if table.contains(&0x91) || table.contains(&0x93) => true,
        Some(table) if table.contains(&0xd2) || table.contains(&0xd3) => false,
        _ => version >= 1000,
    };
    let (mut text, mut text_bytes) = (String::new(), Vec::new());
    for (offset, bytes) in &segments {
        let comma = bytes.iter().position(|&b| b == 44);
        let valid = bytes.first() == Some(&0)
            && comma.is_some_and(|c| c >= 2 && c <= 7)
            && hex_digits(&bytes[1..comma.unwrap()])
                .is_some_and(|n| n as usize == bytes.len() - comma.unwrap() - 1);
        if !valid {
            return Err("invalid XMED text length".into());
        }
        if enhanced && *offset as usize != text_bytes.len() {
            return Err("XMED text segments out of order".into());
        }
        let raw = &bytes[comma.unwrap() + 1..];
        text.push_str(&if windows {
            windows_1252(raw)
        } else {
            mac_roman(raw)
        });
        text_bytes.extend_from_slice(raw);
    }
    let typography = match style_records.get(&6) {
        Some((_, count)) if *count != 0 => Some(paige_styles(&style_records, text_bytes.len())?),
        _ => None,
    };
    let mut out = Map::new();
    if truncated_tail {
        out.insert("truncatedTail".into(), json!(true));
    }
    out.insert("width".into(), json!(width));
    out.insert("height".into(), json!(height));
    out.insert("regX".into(), json!(0));
    out.insert("regY".into(), json!(0));
    out.insert("text".into(), json!(text));
    out.insert("textHex".into(), json!(hex(&text_bytes)));
    out.insert(
        "textEncoding".into(),
        json!(if windows { "windows-1252" } else { "macintosh" }),
    );
    if let Some(t) = typography {
        out.insert("typography".into(), t);
    }
    let hyperlinks = paige_hyperlinks(&style_records, &text)?;
    if !hyperlinks.is_empty() {
        out.insert("hyperlinks".into(), Value::Array(hyperlinks));
    }
    out.insert("textPresentation".into(), json!("native-target-font"));
    out.insert(
        "authoredStyleRecords".into(),
        json!(records.iter().map(|r| r.0).collect::<Vec<_>>()),
    );
    if let Some(bytes) = extension {
        out.insert("mediaExtension".into(), json!({"bytes": bytes, "note": if native_cache {
            "D8 TXcp section retained undecoded in mediaHex; text and styles recovered from Paige records"
        } else { "D10 FFFE layout section retained undecoded in mediaHex" }}));
    }
    Ok(out)
}

#[cfg(test)]
mod empty_tests {
    use super::*;
    #[test]
    fn empty_font_requires_the_complete_nameless_header() {
        let p = [0, 0, 0, 1, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 0];
        assert!(empty_font_xtra(&p));
        for n in 0..p.len() {
            assert!(!empty_font_xtra(&p[..n]));
        }
        let mut named = p.to_vec();
        named.push(b'A');
        assert!(!empty_font_xtra(&named));
        let mut version = p;
        version[3] = 2;
        assert!(!empty_font_xtra(&version));
    }
    #[test]
    fn unknown_missing_text_documents_remain_errors() {
        let mut payload = empty_text_layout(120, 99);
        let model = empty_text_xtra_model(&payload, 850).unwrap();
        assert_eq!(model["width"], 120);
        assert_eq!(model["height"], 14);
        assert_eq!(model["text"], "");
        assert_eq!(model["textStyle"]["fontName"], "Arial");
        assert_eq!(model["textStyle"]["size"], 12);
        payload[0] = 1;
        assert!(empty_text_xtra_model(&payload, 850).is_err());
        assert!(empty_text_xtra_model(&[0; 432], 850).is_err());
        assert!(empty_text_xtra_model(&[0; 76], 800).is_err());
    }
}

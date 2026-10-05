//! D7+ Xtra CASt envelopes (tools/director/xtra-model.mjs). Reference:
//! ScummVM 41ac2b31847622d0662d22c03fe6979e3b43cfbc,
//! engines/director/castmember/xtra.cpp. Payload interpretation is separate.

use serde_json::{json, Map, Value};

use super::js::{hex, latin1, mac_roman, windows_1252};
use super::paige::paige_styles;

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
    if text.is_empty() || !text.bytes().all(|b| b.is_ascii_digit() || (b'A'..=b'F').contains(&b)) {
        return None;
    }
    u64::from_str_radix(text, 16).ok()
}

/// A D8/D10 text Xtra's payload and XMED (Paige) document.
pub fn text_xtra_model(payload: &[u8], xmed: &[u8], version: i64) -> Result<Map<String, Value>, String> {
    if payload.len() != if version >= 1000 { 432 } else { 76 } {
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
    let d10 = version >= 1000;
    while p + 1 < xmed.len() {
        if xmed.len() - p == 3 && xmed[p..] == [3, 70, 70] {
            break;
        }
        if d10 && xmed.len() - p == 5 && xmed[p] == 3 && &xmed[p + 1..] == b"FFFE" {
            break;
        }
        if d10 && xmed[p] == 3 && xmed.get(p + 1..p + 26) == Some(&b"FFFE0000000600040001\x0177AA"[..]) {
            extension = Some(xmed.len() - p);
            break;
        }
        let header = xmed.get(p + 1..p + 21).ok_or("invalid XMED record header")?;
        if xmed[p] != 3 || !header.iter().all(|b| b.is_ascii_digit() || (b'A'..=b'F').contains(b)) {
            return Err("invalid XMED record header".into());
        }
        let kind = hex_digits(&header[..4]).unwrap();
        let length = hex_digits(&header[4..12]).unwrap() as usize;
        let count = hex_digits(&header[12..]).unwrap();
        let continued = kind == 2 && d10;
        if length < 1 || length > xmed.len() - p - 20 || xmed.get(p + 20 + length) != Some(&3)
            || (records.iter().any(|r| r.0 == kind) && !continued) || (kind == 2 && !segments.is_empty() && !continued)
        {
            return Err("invalid XMED record range".into());
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
    let closed = xmed.get(p) == Some(&3) && (tail == 1 || tail == 3);
    if (extension.is_none() && !closed && !(d10 && tail == 5)) || !records.iter().any(|r| r.0 == 0)
        || !records.iter().any(|r| r.0 == 1)
    {
        return Err("incomplete XMED document".into());
    }
    let (mut text, mut text_bytes) = (String::new(), Vec::new());
    for (offset, bytes) in &segments {
        let comma = bytes.iter().position(|&b| b == 44);
        let valid = bytes.first() == Some(&0)
            && comma.is_some_and(|c| c >= 2 && c <= 7)
            && hex_digits(&bytes[1..comma.unwrap()]).is_some_and(|n| n as usize == bytes.len() - comma.unwrap() - 1);
        if !valid {
            return Err("invalid XMED text length".into());
        }
        if d10 && *offset as usize != text_bytes.len() {
            return Err("XMED text segments out of order".into());
        }
        let raw = &bytes[comma.unwrap() + 1..];
        text.push_str(&if d10 { windows_1252(raw) } else { mac_roman(raw) });
        text_bytes.extend_from_slice(raw);
    }
    let typography = match style_records.get(&6) {
        Some((_, count)) if *count != 0 => Some(paige_styles(&style_records, text_bytes.len())?),
        _ => None,
    };
    let mut out = Map::new();
    out.insert("width".into(), json!(width));
    out.insert("height".into(), json!(height));
    out.insert("regX".into(), json!(0));
    out.insert("regY".into(), json!(0));
    out.insert("text".into(), json!(text));
    out.insert("textHex".into(), json!(hex(&text_bytes)));
    out.insert("textEncoding".into(), json!(if d10 { "windows-1252" } else { "macintosh" }));
    if let Some(t) = typography {
        out.insert("typography".into(), t);
    }
    out.insert("textPresentation".into(), json!("native-target-font"));
    out.insert("authoredStyleRecords".into(), json!(records.iter().map(|r| r.0).collect::<Vec<_>>()));
    if let Some(bytes) = extension {
        out.insert("mediaExtension".into(), json!({"bytes": bytes, "note": "D10 FFFE layout section retained undecoded in mediaHex"}));
    }
    Ok(out)
}

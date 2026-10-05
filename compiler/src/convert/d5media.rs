//! Director resource layouts (tools/director/d5-media.mjs), independently
//! implemented from the pinned local ScummVM engines/director/{sound.cpp,
//! cast.cpp,fonts.cpp,castmember/digitalvideo.cpp}.

use serde_json::{json, Map, Value};

use super::js::{hex, latin1, mac_roman};

fn be16(b: &[u8], at: usize) -> Result<usize, String> {
    b.get(at..at + 2).map(|s| u16::from_be_bytes([s[0], s[1]]) as usize).ok_or_else(|| "read outside resource".into())
}
fn be32(b: &[u8], at: usize) -> Result<usize, String> {
    b.get(at..at + 4)
        .map(|s| u32::from_be_bytes(s.try_into().unwrap()) as usize)
        .ok_or_else(|| "read outside resource".into())
}

pub fn cast_strings(cast: &[u8]) -> Result<Vec<String>, String> {
    let length = be32(cast, 4)?;
    let info = cast.get(12..12 + length).unwrap_or(&cast[cast.len().min(12)..]);
    if info.len() != length || length < 22 {
        return Err("truncated cast info".into());
    }
    let table = be32(info, 0)?;
    if table < 20 || table + 2 > length {
        return Err("invalid cast info table".into());
    }
    let count = be16(info, table)?;
    let base = table + 2 + (count + 1) * 4;
    if count > 64 || base > length {
        return Err("invalid cast info string count".into());
    }
    let mut strings = Vec::new();
    for i in 0..count {
        let start = base + be32(info, table + 2 + i * 4)?;
        let end = base + be32(info, table + 6 + i * 4)?;
        if start > end || end > length {
            return Err("cast string outside info".into());
        }
        if start == end {
            strings.push(String::new());
            continue;
        }
        let n = info[start] as usize;
        if n > end - start - 1 {
            return Err("truncated cast Pascal string".into());
        }
        strings.push(latin1(&info[start + 1..start + 1 + n]));
    }
    Ok(strings)
}

pub struct Sound {
    pub rate: u32,
    pub frames: u32,
    pub channels: u32,
    pub bits: u32,
    pub loop_start: u32,
    pub loop_end: u32,
    pub samples: Vec<u8>,
}

pub fn sound_resource(b: &[u8]) -> Result<Sound, String> {
    if b.len() < 14 {
        return Err("truncated snd resource".into());
    }
    let format = be16(b, 0)?;
    let mut channels = 1u32;
    let p = if format == 2 {
        4
    } else if format == 1 && b.len() >= 20 && be16(b, 2)? == 1 && be16(b, 4)? == 5 {
        channels = if be32(b, 6)? & 128 != 0 { 1 } else { 2 };
        10
    } else {
        return Err("unsupported snd format".into());
    };
    if be16(b, p)? != 1 || ![0x8050, 0x8051].contains(&be16(b, p + 2)?) {
        return Err("unsupported snd commands".into());
    }
    let start = be32(b, p + 6)?;
    if start != p + 10 || start + 22 > b.len() {
        return Err("invalid snd header offset".into());
    }
    let h = &b[start..];
    let param = be32(h, 4)? as u64;
    let rate = be16(h, 8)? as u32;
    let loop_start = be32(h, 12)? as u32;
    let loop_end = be32(h, 16)? as u32;
    let encoding = h[20];
    if be32(h, 0)? != 0 || be16(h, 10)? != 0 || h[21] != 60 {
        return Err("unsupported snd pointer/rate/pitch".into());
    }
    // JS divides as doubles: a fractional frame count fails the bounds below.
    let mut frames = param as f64 / channels as f64;
    let mut bits = 8u32;
    let mut data_offset = 22usize;
    if encoding == 255 {
        if h.len() < 64 {
            return Err("truncated extended snd header".into());
        }
        channels = param as u32;
        frames = be32(h, 22)? as f64;
        bits = be16(h, 48)? as u32;
        data_offset = 64;
    } else if encoding != 0 {
        return Err("unsupported snd compression".into());
    }
    let bytes = frames * channels as f64 * bits as f64 / 8.0;
    if ![1, 2].contains(&channels) || ![8, 16].contains(&bits) || rate == 0 || frames.fract() != 0.0
        || bytes != (h.len() - data_offset) as f64 || loop_start > loop_end || loop_end as f64 > frames
    {
        return Err("invalid snd sample bounds".into());
    }
    Ok(Sound { rate, frames: frames as u32, channels, bits, loop_start, loop_end, samples: h[data_offset..].to_vec() })
}

/// STXT: a 12-byte header and bounded 20-byte FontStyle records.
pub fn styled_text(b: &[u8]) -> Result<Map<String, Value>, String> {
    if b.len() < 14 || be32(b, 0)? != 12 {
        return Err("invalid STXT header".into());
    }
    let length = be32(b, 4)?;
    let data_length = be32(b, 8)?;
    let end = 12 + length;
    if end + 2 > b.len() {
        return Err("truncated STXT text".into());
    }
    let count = be16(b, end)?;
    if count == 0 || count > 256 || data_length != count * 20 + 2 || end + data_length != b.len() {
        return Err("invalid STXT style extent".into());
    }
    let mut styles: Vec<Value> = Vec::new();
    let mut previous = 0usize;
    for i in 0..count {
        let p = end + 2 + i * 20;
        let offset = be32(b, p)?;
        if offset > length || (i == 0 && offset != 0) || (i > 0 && offset < previous) {
            return Err("invalid STXT style offset".into());
        }
        previous = offset;
        styles.push(json!({
            "offset": offset, "lineHeight": be16(b, p + 4)?, "ascent": be16(b, p + 6)?,
            "sourceFontId": be16(b, p + 8)?, "face": b[p + 10], "size": be16(b, p + 12)?,
            "color": (b[p + 14] as u32) << 16 | (b[p + 16] as u32) << 8 | b[p + 18] as u32,
        }));
    }
    let raw = &b[12..end];
    let mut out = Map::new();
    out.insert("text".into(), json!(mac_roman(raw)));
    out.insert("textHex".into(), json!(hex(raw)));
    out.insert("textEncoding".into(), json!("macintosh"));
    out.insert("sourceTextStyles".into(), Value::Array(styles));
    Ok(out)
}

/// Fmap: a cast's font IDs to authored names; a Windows entry wins a duplicate.
pub fn font_map(b: &[u8]) -> Result<Map<String, Value>, String> {
    if b.len() < 36 {
        return Err("truncated Fmap".into());
    }
    let map_length = be32(b, 0)?;
    let names_length = be32(b, 4)?;
    let names_start = 8 + map_length;
    if names_start + names_length > b.len() {
        return Err("invalid Fmap extents".into());
    }
    let count = be32(b, 16)?;
    if count > 1024 || 36 + count * 8 > names_start {
        return Err("invalid Fmap entry count".into());
    }
    let mut map = Map::new();
    let mut platforms: std::collections::HashMap<usize, usize> = Default::default();
    for i in 0..count {
        let p = 36 + i * 8;
        let name_offset = be32(b, p)?;
        let platform = be16(b, p + 4)?;
        let id = be16(b, p + 6)?;
        let at = names_start + name_offset;
        if at + 4 > names_start + names_length {
            return Err("Fmap name outside chunk".into());
        }
        let length = be32(b, at)?;
        if at + 4 + length > names_start + names_length {
            return Err("truncated Fmap name".into());
        }
        let key = id.to_string();
        if !map.contains_key(&key) || platforms.get(&id) != Some(&2) {
            map.insert(key, json!(latin1(&b[at + 4..at + 4 + length])));
            platforms.insert(id, platform);
        }
    }
    Ok(map)
}

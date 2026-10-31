//! Per-run field/text formatting, in UTF-16 code-unit offsets.
use serde_json::Value;

pub struct TextFormat {
    pub offset: u32,
    pub font: u8,
    pub size: u8,
    pub face: u8,
    pub align: u8,
    pub ascent: i16,
    pub line_height: i16,
    pub char_spacing: i16,
    pub rgb: u32,
}

pub fn read(value: &Value, text: &str, modern: bool, kind: u16) -> Result<Vec<TextFormat>, String> {
    if value.is_null() {
        return Ok(Vec::new());
    }
    let entries = value.as_array().ok_or("textFormats must be an array")?;
    if entries.is_empty() {
        return Ok(Vec::new());
    }
    if !modern || !matches!(kind, 3 | 7) || entries.len() > 4096 {
        return Err("textFormats require a modern field/text member and at most 4096 runs".into());
    }
    let extent = text.encode_utf16().count() as u64;
    let mut previous = None;
    let mut out = Vec::with_capacity(entries.len());
    for entry in entries {
        let uint = |name: &str, maximum: u64| {
            entry[name]
                .as_u64()
                .filter(|n| *n <= maximum)
                .ok_or_else(|| format!("invalid textFormats {name}"))
        };
        let signed = |name: &str| {
            entry[name]
                .as_i64()
                .filter(|n| i16::try_from(*n).is_ok())
                .ok_or_else(|| format!("invalid textFormats {name}"))
        };
        let offset = uint("offset", extent)? as u32;
        if (previous.is_none() && offset != 0) || previous.is_some_and(|p| offset <= p) {
            return Err("textFormats offsets must begin at zero and strictly increase".into());
        }
        previous = Some(offset);
        let size = uint("size", 255)? as u8;
        out.push(TextFormat {
            offset,
            size,
            font: uint("fontId", 255)? as u8,
            face: uint("face", 15)? as u8,
            align: uint("align", 3)? as u8,
            ascent: signed("ascent")? as i16,
            line_height: uint("lineHeight", 32767)? as i16,
            char_spacing: if entry.get("charSpacing").is_none() {
                0
            } else {
                signed("charSpacing")? as i16
            },
            rgb: uint("color", 0xffffff)? as u32,
        });
    }
    Ok(out)
}

//! The Paige 4.0001 serialization in D8 XMED documents
//! (tools/director/paige-model.mjs). Format reference: Hermes-Paige
//! cddd9543709db505077b7b0426962fadd85b343d, PGSOURCE/PGREAD.C and
//! PGHEADER/PAIGE.H. All numeric fields are preserved.

use std::collections::BTreeMap;

use serde_json::{json, Map, Value};

use super::js::{mac_roman, num};

/// Paige's numeric and byte-value stream.
pub struct Reader<'a> {
    pub bytes: &'a [u8],
    pub position: usize,
    previous: Option<i64>,
    repeat: u32,
}

impl<'a> Reader<'a> {
    pub fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, position: 0, previous: None, repeat: 0 }
    }
    fn next_byte(&mut self) -> Option<u8> {
        let b = self.bytes.get(self.position).copied();
        self.position += 1;
        b
    }
    pub fn number(&mut self) -> Result<i64, String> {
        if self.repeat > 0 {
            self.repeat -= 1;
            return Ok(self.previous.unwrap());
        }
        let marker = self.next_byte();
        let marker = marker.filter(|m| [1, 2, 0x81, 0x82, 0xc1, 0xc2].contains(m)).ok_or("invalid Paige number marker")?;
        if marker & 0x80 != 0 {
            let previous = self.previous.ok_or("Paige repeat before first number")?;
            if marker & 0x40 != 0 {
                let count = self.next_byte().unwrap_or(0);
                if count == 0 {
                    return Err("invalid Paige repeat count".into());
                }
                self.repeat = count as u32 - 1;
            }
            return Ok(previous);
        }
        let start = self.position;
        while self.position < self.bytes.len() {
            let b = self.bytes[self.position];
            if b == 45 || b.is_ascii_digit() || (b'A'..=b'F').contains(&b) {
                self.position += 1;
            } else {
                break;
            }
        }
        let raw = std::str::from_utf8(&self.bytes[start..self.position]).unwrap_or("");
        let (negative, digits) = match raw.strip_prefix('-') {
            Some(d) => (true, d),
            None => (false, raw),
        };
        if digits.is_empty() || digits.len() > 8 || !digits.bytes().all(|b| b.is_ascii_digit() || (b'A'..=b'F').contains(&b)) {
            return Err("invalid Paige numeric value".into());
        }
        let value = i64::from_str_radix(digits, 16).unwrap() * if negative { -1 } else { 1 };
        if value < -0x8000_0000 || value > 0xffff_ffff {
            return Err("Paige number out of range".into());
        }
        self.previous = Some(value);
        Ok(value)
    }
    pub fn numbers(&mut self, count: i64) -> Result<Vec<i64>, String> {
        if !(0..=100000).contains(&count) {
            return Err("invalid Paige element count".into());
        }
        (0..count).map(|_| self.number()).collect()
    }
    fn bytes_value(&mut self) -> Result<&'a [u8], String> {
        if self.repeat > 0 || self.next_byte() != Some(0) {
            return Err("invalid Paige byte marker".into());
        }
        let comma = self.bytes[self.position.min(self.bytes.len())..].iter().position(|&b| b == 44).map(|c| c + self.position);
        let raw = comma.map(|c| &self.bytes[self.position..c]).unwrap_or(&[]);
        let valid = comma.is_some() && !raw.is_empty() && raw.len() <= 6 && raw.iter().all(|b| b.is_ascii_digit() || (b'A'..=b'F').contains(b));
        if !valid {
            return Err("invalid Paige byte length".into());
        }
        let length = usize::from_str_radix(std::str::from_utf8(raw).unwrap(), 16).unwrap();
        self.position = comma.unwrap() + 1;
        if length > self.bytes.len() - self.position {
            return Err("truncated Paige byte value".into());
        }
        let result = &self.bytes[self.position..self.position + length];
        self.position += length;
        Ok(result)
    }
    pub fn end(&self) -> Result<(), String> {
        if self.repeat > 0 || self.position != self.bytes.len() {
            return Err("unconsumed Paige record data".into());
        }
        Ok(())
    }
}

const NAMES: [&str; 32] = [
    "bold", "italic", "underline", "outline", "shadow", "condense", "extend", "doubleUnderline", "wordUnderline",
    "dottedUnderline", "hidden", "strikeout", "superscript", "subscript", "rotation", "allCaps", "allLower",
    "smallCaps", "overline", "boxed", "relativePoint", "superImpose", "revision", "nestedSubset", "blink", "index",
    "toc", "custom27", "custom28", "custom29", "custom30", "custom31",
];

fn font_name(b: &[u8]) -> Result<String, String> {
    if b.len() != 64 || b[0] > 63 {
        return Err("invalid Paige Pascal font name".into());
    }
    Ok(mac_roman(&b[1..1 + b[0] as usize]))
}

/// Section records by kind: (bytes, element count).
pub type Records = BTreeMap<u64, (Vec<u8>, u64)>;

fn record_reader(records: &Records, kind: u64) -> Result<(Reader<'_>, u64), String> {
    let (bytes, count) = records
        .get(&kind)
        .filter(|r| r.1 <= 4096)
        .ok_or_else(|| format!("missing or excessive Paige section {kind}"))?;
    Ok((Reader::new(bytes), *count))
}

fn ints(values: &[i64]) -> Value {
    Value::Array(values.iter().map(|v| json!(v)).collect())
}

pub fn paige_styles(records: &Records, text_length: usize) -> Result<Value, String> {
    let (mut version, _) = record_reader(records, 0)?;
    if version.number()? != 0x40001 {
        return Err("unsupported Paige style version".into());
    }
    let (mut fr, font_count) = record_reader(records, 8)?;
    let mut fonts = Vec::new();
    for _ in 0..font_count {
        let name = font_name(fr.bytes_value()?)?;
        let alternate = font_name(fr.bytes_value()?)?;
        let raw = fr.numbers(17)?;
        fonts.push(json!({
            "name": name, "alternateName": alternate, "environs": raw[0], "typeface": raw[1], "familyId": raw[2],
            "characterType": raw[3], "codePage": raw[4], "language": raw[5], "platform": raw[15], "raw": ints(&raw),
        }));
    }
    fr.end()?;
    let (mut sr, style_count) = record_reader(records, 6)?;
    let insert_style = sr.number()?;
    let mut styles = Vec::new();
    for _ in 0..style_count {
        let raw = sr.numbers(77)?;
        let font_index = raw[0];
        if font_index < 0 || font_index as usize >= fonts.len() {
            return Err("invalid Paige style font index".into());
        }
        let mut face = Map::new();
        for (j, name) in NAMES.iter().enumerate() {
            if raw[39 + j] != 0 {
                face.insert(name.to_string(), json!(raw[39 + j]));
            }
        }
        styles.push(json!({
            "fontIndex": font_index, "font": fonts[font_index as usize]["name"], "pointSize": num(raw[18] as f64 / 65536.0),
            "ascent": raw[3], "descent": raw[4], "leading": raw[5], "foreground": ints(&raw[9..13]),
            "background": ints(&raw[13..17]), "characterWidth": raw[17], "leftOverhang": raw[19],
            "rightOverhang": raw[20], "topExtra": raw[21], "bottomExtra": raw[22], "spaceExtra": raw[23],
            "characterExtra": raw[24], "face": face, "raw": ints(&raw),
        }));
    }
    sr.end()?;
    if insert_style < 0 || insert_style as u64 >= style_count {
        return Err("invalid Paige insertion style".into());
    }
    let (mut pr, paragraph_count) = record_reader(records, 7)?;
    let mut paragraphs = Vec::new();
    for _ in 0..paragraph_count {
        let prefix = pr.numbers(30)?;
        let tab_count = prefix[29];
        if !(0..=32).contains(&tab_count) {
            return Err("unsupported Paige tab count".into());
        }
        let tabs: Vec<Vec<i64>> = (0..tab_count).map(|_| pr.numbers(4)).collect::<Result<_, _>>()?;
        let tail = pr.numbers(24)?;
        let mut raw = prefix.clone();
        raw.extend(tabs.iter().flatten());
        raw.extend(&tail);
        paragraphs.push(json!({
            "justification": prefix[0], "direction": prefix[1], "leftIndent": prefix[3], "rightIndent": prefix[4],
            "firstIndent": prefix[5], "spacing": prefix[6], "leadingExtra": prefix[7], "leadingFixed": prefix[8],
            "leadingVariable": prefix[9], "topExtra": prefix[10], "bottomExtra": prefix[11], "leftExtra": prefix[12],
            "rightExtra": prefix[13], "tabs": tabs.iter().map(|t| ints(t)).collect::<Vec<_>>(),
            "defaultTabSpace": tail[0], "raw": ints(&raw),
        }));
    }
    pr.end()?;
    let runs = |kind: u64, limit: usize| -> Result<Vec<Value>, String> {
        let (mut r, count) = record_reader(records, kind)?;
        let mut result = Vec::new();
        let mut previous = -1i64;
        for _ in 0..count {
            let pair = r.numbers(2)?;
            let (offset, index) = (pair[0], pair[1]);
            if offset < previous || offset > text_length as i64 + 2 || index < 0 || index as usize >= limit {
                return Err("invalid Paige style run".into());
            }
            result.push(json!({"offset": offset, "index": index}));
            previous = offset;
        }
        r.end()?;
        if result.is_empty() || result[0]["offset"] != 0 {
            return Err("missing Paige initial style run".into());
        }
        Ok(result)
    };
    let style_runs = runs(4, styles.len())?;
    let paragraph_runs = runs(5, paragraphs.len())?;
    Ok(json!({
        "version": 0x40001, "insertStyle": insert_style, "fonts": fonts, "styles": styles, "paragraphs": paragraphs,
        "styleRuns": style_runs, "paragraphRuns": paragraph_runs,
    }))
}

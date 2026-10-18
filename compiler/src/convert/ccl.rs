//! A cast library's film-loop cast-reference table (`ccl `).
//!
//! The table is independent of the main movie's cast-library numbering.
//! Original Director 8.5.1 tests established that a film's encoded cast zero
//! selects entry zero: changing that entry changes the film's sound library.
//! Layout and bounds are corroborated by the recovered D5–D10 resources.

use super::js;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Reference {
    pub cast: i16,
    pub path: String,
}

pub fn decode(bytes: &[u8]) -> Result<Vec<Reference>, String> {
    let word = |at: usize| {
        bytes
            .get(at..at + 2)
            .map(|b| u16::from_be_bytes(b.try_into().unwrap()) as usize)
            .ok_or_else(|| "truncated ccl cast-reference table".to_string())
    };
    let long = |at: usize| {
        bytes
            .get(at..at + 4)
            .map(|b| u32::from_be_bytes(b.try_into().unwrap()) as usize)
            .ok_or_else(|| "truncated ccl cast-reference table".to_string())
    };
    let table = long(0)?;
    if table != 4 {
        return Err(format!("unsupported ccl table offset {table}"));
    }
    let count = word(table)?;
    let offsets = table + 2;
    let base = offsets + (count + 1) * 4;
    if base > bytes.len() || long(offsets)? != 0 || long(offsets + count * 4)? != bytes.len() - base
    {
        return Err("invalid ccl entry offsets".into());
    }
    let mut result = Vec::with_capacity(count);
    for index in 0..count {
        let start = long(offsets + index * 4)?;
        let end = long(offsets + (index + 1) * 4)?;
        if start > end || end > bytes.len() - base {
            return Err(format!("invalid ccl entry {index} bounds"));
        }
        let record = &bytes[base + start..base + end];
        if record.len() < 4 || record.len() != record[2] as usize + 4 || record.last() != Some(&0) {
            return Err(format!("invalid ccl entry {index} path"));
        }
        let path = &record[3..record.len() - 1];
        if path.contains(&0) {
            return Err(format!("embedded NUL in ccl entry {index} path"));
        }
        // Classic Mac paths carry MacRoman; Windows paths carry ANSI. The
        // source paths in this format retain their authored path syntax.
        let path = if path.contains(&b':') && !path.contains(&b'\\') && !path.contains(&b'/') {
            js::mac_roman(path)
        } else {
            js::latin1(path)
        };
        result.push(Reference {
            cast: i16::from_be_bytes(record[..2].try_into().unwrap()),
            path,
        });
    }
    Ok(result)
}

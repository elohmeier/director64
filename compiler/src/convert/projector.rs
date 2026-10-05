//! tools/director/projector-model.mjs: adapt an embedded, uncompressed
//! Director movie to the parser. Only an in-memory view changes, never the
//! executable.

use serde_json::{json, Value};

use super::source::sha256;

pub struct View {
    pub bytes: Vec<u8>,
    pub offset: usize,
    pub archives: Value,
}

fn le32(b: &[u8], at: usize) -> usize {
    u32::from_le_bytes(b[at..at + 4].try_into().unwrap()) as usize
}

pub fn projector_view(input: &[u8], selection: Option<&Value>) -> Result<View, String> {
    let mut candidates = Vec::new();
    if input.len() >= 44 {
        for offset in 0..=input.len() - 44 {
            if &input[offset..offset + 4] != b"XFIR" || &input[offset + 8..offset + 16] != b"39VMpami" {
                continue;
            }
            let length = le32(input, offset + 4);
            let imap_length = le32(input, offset + 16);
            let map_offset = le32(input, offset + 24);
            if length < 36 || length > input.len() - offset - 8 || imap_length != 24
                || map_offset < offset + 44 || map_offset > offset + length - 24
                || input.get(map_offset..map_offset + 4) != Some(b"pamm")
            {
                return Err("invalid embedded projector archive bounds".into());
            }
            candidates.push(offset);
        }
    }
    let archives: Vec<Value> = candidates
        .iter()
        .map(|&offset| {
            let size = le32(input, offset + 4) + 8;
            json!({"offset": offset, "bytes": size, "sha256": sha256(&input[offset..offset + size])})
        })
        .collect();
    let archives = Value::Array(archives);
    let offset = match selection {
        Some(selection) => {
            let chosen = selection["offset"].as_u64().map(|o| o as usize);
            if super::js::stringify(&archives) != super::js::stringify(&selection["archives"])
                || !chosen.is_some_and(|o| candidates.contains(&o))
            {
                return Err("embedded projector archive selection changed".into());
            }
            chosen.unwrap()
        }
        None => {
            if candidates.len() != 1 {
                return Err(format!("expected one embedded MV93 movie, got {}", candidates.len()));
            }
            candidates[0]
        }
    };
    // Embedded mmap offsets are absolute executable positions. Relocating only
    // the RIFX/imap prologue keeps every source chunk at its original offset.
    let mut bytes = input.to_vec();
    bytes.copy_within(offset..offset + 44, 0);
    Ok(View { bytes, offset, archives })
}

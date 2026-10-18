//! Linked Director movie cast metadata and exact source-file resolution.
//! Reference: ScummVM 41ac2b31847622d0662d22c03fe6979e3b43cfbc,
//! engines/director/castmember/movie.cpp. Format facts only; no copied code.

use serde_json::{Map, Value, json};

use super::{files, source};

pub fn metadata(bytes: &[u8], directory: &str, file: &str) -> Result<Value, String> {
    if bytes.len() != 14 || file.is_empty() {
        return Err("unsupported linked movie metadata".into());
    }
    let word = |at: usize| i16::from_be_bytes([bytes[at], bytes[at + 1]]) as i32;
    let (top, left, bottom, right) = (word(0), word(2), word(4), word(6));
    let (width, height) = (right - left, bottom - top);
    if width <= 0 || height <= 0 || width > u16::MAX as i32 || height > u16::MAX as i32 {
        return Err("invalid linked movie bounds".into());
    }
    let flags = u32::from_be_bytes(bytes[8..12].try_into().unwrap());
    Ok(
        json!({"linkedDirectory":directory,"linkedFile":file,"linkedLeft":left,"linkedTop":top,
        "linkedFlags":flags,"linkedUnknown":u16::from_be_bytes(bytes[12..14].try_into().unwrap()),
        "width":width,"height":height,"regX":width / 2,"regY":height / 2}),
    )
}

fn path(value: &str) -> String {
    files::normalize(&value.replace(['\\', ':'], "/")).to_uppercase()
}

fn canonical_path(value: &str) -> String {
    let normalized = path(value);
    let name = |file: &str| {
        if file.contains('.') {
            source::canonical_name(file)
        } else {
            format!("{file}.DXR")
        }
    };
    if let Some((folder, file)) = normalized.rsplit_once('/') {
        format!("{folder}/{}", name(file))
    } else {
        name(&normalized)
    }
}

/// Resolve only filenames and paths present in the recovered source manifest.
/// Folder/file aliases come from the edition policy; no DOS truncation or
/// fuzzy basename guesses can bind a child to a different authored movie.
pub fn resolve(
    directory: &str,
    file: &str,
    owner_path: &str,
    sources: &Map<String, Value>,
    policy: &Value,
) -> Result<String, String> {
    if file.is_empty() {
        return Err("linked movie has no filename".into());
    }
    let owner = path(owner_path);
    let parent = owner.rsplit_once('/').map_or("", |(p, _)| p);
    let authored = format!("{}/{}", directory.trim_end_matches(['/', '\\', ':']), file);
    let authored = authored.trim_start_matches('/');
    let relative = authored
        .strip_prefix("@/")
        .or_else(|| authored.strip_prefix("@\\"))
        .or_else(|| authored.strip_prefix("@:"))
        .unwrap_or(authored);
    let mut candidates = vec![
        canonical_path(&files::join(parent, &path(relative))),
        canonical_path(relative),
    ];
    candidates.dedup();
    for candidate in candidates {
        let mut physical = vec![candidate.clone()];
        for (long, short) in policy["source_path_aliases"]
            .as_object()
            .into_iter()
            .flatten()
        {
            let canonical = canonical_path(long);
            if canonical == candidate || candidate.ends_with(&format!("/{canonical}")) {
                let short = short.as_str().ok_or("invalid source path alias")?;
                physical.push(canonical_path(short));
            }
        }
        let matches: Vec<&String> = sources
            .iter()
            .filter_map(|(name, value)| {
                let actual = canonical_path(value.as_str().unwrap_or(name));
                physical.contains(&actual).then_some(name)
            })
            .collect();
        if matches.len() == 1 {
            return Ok(matches[0].clone());
        }
        if matches.len() > 1 {
            return Err(format!("ambiguous linked movie path {directory}{file}"));
        }
    }
    // Match the canonical name supplied by exact ISO/Joliet file aliases and
    // by the canonical namespace of folder-qualified ports.
    let filename = canonical_path(file);
    let mut folders = Vec::new();
    let normalized = path(directory);
    if let Some(last) = normalized
        .split('/')
        .rev()
        .find(|p| !p.is_empty() && *p != "." && *p != "@")
    {
        folders.push(source::qualifier_name(policy, last));
    }
    if let Some(last) = parent.split('/').next_back().filter(|p| !p.is_empty()) {
        let name = source::qualifier_name(policy, last);
        if !folders.contains(&name) {
            folders.push(name);
        }
    }
    for folder in folders {
        let key = format!("{folder}__{filename}");
        if sources.contains_key(&key) {
            return Ok(key);
        }
    }
    if sources.contains_key(&filename) {
        return Ok(filename);
    }
    Err(format!("unresolved linked movie {directory}{file}"))
}

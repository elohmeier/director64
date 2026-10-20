//! QuickTime Asset Xtra metadata. Format flags checked against clean ScummVM
//! 41ac2b31847622d0662d22c03fe6979e3b43cfbc digitalvideo.cpp; serialization
//! layout corroborated by the 160 recovered Director8.5 members. No reference
//! implementation code is included here.
use serde_json::{Value, json};

fn path(s: &str) -> String {
    s.replace(['\\', ':'], "/").trim_matches('/').to_uppercase()
}

/// Disc-proven candidates, in order: the cast's own directory and exact
/// authored path aliases. No synthetic DOS8.3 truncation is permitted.
pub fn media_candidates(directory: &str, file: &str, owner: &str, policy: &Value) -> Vec<String> {
    let file = path(file);
    let owner = path(owner);
    let folder = owner.rsplit_once('/').map_or("", |(folder, _)| folder);
    let authored = path(&format!("{directory}/{file}"));
    let relative = if folder.is_empty() {
        file.clone()
    } else {
        format!("{folder}/{file}")
    };
    let mut candidates = vec![relative.clone(), authored.clone()];
    for (long, short) in policy["source_path_aliases"]
        .as_object()
        .into_iter()
        .flatten()
    {
        let long = path(long);
        let Some(short) = short.as_str() else {
            continue;
        };
        let short = path(short);
        let exact_basename_in_owner = long.rsplit('/').next() == Some(file.as_str())
            && short.rsplit_once('/').map_or("", |(p, _)| p) == folder;
        if long == relative
            || authored == long
            || authored.ends_with(&format!("/{long}"))
            || exact_basename_in_owner
        {
            candidates.push(short);
        }
    }
    let mut unique = Vec::new();
    for candidate in candidates {
        if !unique.contains(&candidate) {
            unique.push(candidate);
        }
    }
    unique
}

pub fn metadata(
    payload: &[u8],
    directory: &str,
    file: &str,
    owner: &str,
    policy: &Value,
) -> Result<Value, String> {
    if payload.get(..4) == Some(&[0x10, 0, 0x10, 7]) {
        if payload.len() < 76 || ![[0x3a, 0x50, 0, 0], [0x1a, 0x50, 0, 0]].contains(&payload[4..8].try_into().unwrap()) || payload[8..16] != [0; 8] {
            return Err("unsupported Director 6 QuickTime Xtra controls".into());
        }
        let n = u32::from_be_bytes(payload[16..20].try_into().unwrap()) as usize;
        if n == 0 || payload.len() != 20 + n + 56 { return Err("invalid Director 6 QuickTime path extent".into()) }
        let mut identity = [0u8; 56]; identity[7] = 1; identity[15] = 1;
        if payload[20 + n..] != identity { return Err("unsupported Director 6 QuickTime transform".into()) }
        let authored = super::js::latin1(&payload[20..20 + n]);
        let filename = authored.rsplit([':', '\\', '/']).next().unwrap_or("");
        if filename.is_empty() { return Err("empty Director 6 QuickTime filename".into()) }
        let folder = authored.strip_suffix(filename).unwrap_or("").trim_end_matches([':', '\\', '/']);
        return Ok(json!({"type":10,"sourceType":15,"quickTimeSerialization":6,
            "linkedDirectory":folder,"linkedFile":filename,
            "linkedSourceCandidates":media_candidates(folder,filename,owner,policy),
            "width":0,"height":0,"regX":0,"regY":0,"videoBoundsFromMedia":true,"videoFlags":8}));
    }
    if payload.len() != 54 || payload[..4] != [0x11, 0, 0x11, 4] || file.is_empty() {
        return Err("unsupported QuickTime Xtra serialization".into());
    }
    let flags = u32::from_be_bytes(payload[4..8].try_into().unwrap());
    let width = u16::from_be_bytes(payload[48..50].try_into().unwrap());
    let height = u16::from_be_bytes(payload[50..52].try_into().unwrap());
    if (width == 0) != (height == 0) {
        return Err("empty QuickTime Xtra bounds".into());
    }
    // These authored controls need additional geometry or native controller
    // support; refuse them rather than discarding their visible effect.
    if flags & (1 | 2 | 4 | 32 | 0x400 | 0x800 | 0x1000 | 0x2000 | 0x4000) != 0
        || flags & !0x1ffff != 0
    {
        return Err("unsupported QuickTime Xtra controller/transform flags".into());
    }
    let video_flags = (if flags & 0x200 != 0 { 8 } else { 0 })
        | (if flags & 0x40 != 0 { 16 } else { 0 })
        | (if flags & 0x100 != 0 { 256 } else { 0 })
        | (if flags & 8 == 0 { 512 } else { 0 });
    Ok(json!({"type":10,"sourceType":15,"quickTimeFlags":flags,
        "linkedDirectory":directory,"linkedFile":file,
        "linkedSourceCandidates":media_candidates(directory,file,owner,policy),
        "width":width,"height":height,"regX":width/2,"regY":height/2,
        "videoBoundsFromMedia":width == 0 && height == 0,
        "videoFlags":video_flags}))
}

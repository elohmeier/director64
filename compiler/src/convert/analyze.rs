//! tools/director/analyze-director.mjs: every Director file's scripts,
//! bitmaps and sounds into an inventory, and one .lingo file per movie.

use std::collections::{BTreeSet, HashMap, HashSet};

use serde_json::{json, Map, Value};

use super::d5media::sound_resource;
use super::dump::Dump;
use super::files::{self, Files};
use super::js;
use super::plan::{directories, file_key, read_dump};
use super::source::{active_key_entries, normalized_scripts, quoted_strings, sha256, source_files};

fn be_i16(b: &[u8], at: usize) -> Result<i64, String> {
    b.get(at..at + 2).map(|s| i16::from_be_bytes([s[0], s[1]]) as i64).ok_or_else(|| "offset is out of range".into())
}
fn be_u16(b: &[u8], at: usize) -> Result<i64, String> {
    b.get(at..at + 2).map(|s| u16::from_be_bytes([s[0], s[1]]) as i64).ok_or_else(|| "offset is out of range".into())
}
fn be_u32(b: &[u8], at: usize) -> Result<u64, String> {
    b.get(at..at + 4).map(|s| u32::from_be_bytes(s.try_into().unwrap()) as u64).ok_or_else(|| "offset is out of range".into())
}
fn be_i32(b: &[u8], at: usize) -> Result<i64, String> {
    b.get(at..at + 4).map(|s| i32::from_be_bytes(s.try_into().unwrap()) as i64).ok_or_else(|| "offset is out of range".into())
}

/// The CASt's type-specific data, as a DataView over it.
fn specific(chunk: &[u8]) -> Result<&[u8], String> {
    let info = be_u32(chunk, 4)? as usize;
    let length = be_u32(chunk, 8)? as usize;
    chunk.get(12 + info..12 + info + length).ok_or_else(|| "Invalid DataView length".into())
}

fn parse_bitmap(chunk: &[u8], version: i64) -> Result<Map<String, Value>, String> {
    let v = specific(chunk)?;
    let pitch_raw = be_u16(v, 0)?;
    let top = be_i16(v, 2)?;
    let left = be_i16(v, 4)?;
    let bottom = be_i16(v, 6)?;
    let right = be_i16(v, 8)?;
    let mut offset = 10 + 2; // D6 padding
    let edit_version = be_u16(v, offset)?;
    offset += 2;
    let scroll_y = be_i16(v, offset)?;
    let scroll_x = be_i16(v, offset + 2)?;
    offset += 4;
    let reg_y = be_i16(v, offset)?;
    let reg_x = be_i16(v, offset + 2)?;
    offset += 4;
    let update_flags = if version < 600 || v.len() == 22 {
        0
    } else {
        *v.get(offset).ok_or("offset is out of range")? as i64
    };
    offset += 1;
    let mut bits = 1i64;
    let (mut palette_lib, mut palette_id) = (Value::Null, Value::Null);
    let mut pitch = pitch_raw;
    if if version < 600 { v.len() > 22 } else { pitch_raw & 0x8000 != 0 } {
        pitch = pitch_raw & if version < 600 { 0xfff } else { 0x3fff };
        bits = *v.get(offset).ok_or("offset is out of range")? as i64;
        offset += 1;
        palette_lib = json!(be_i16(v, offset)?);
        palette_id = json!(be_i16(v, offset + 2)?);
    }
    let mut out = Map::new();
    out.insert("width".into(), json!(right - left));
    out.insert("height".into(), json!(bottom - top));
    out.insert("rect".into(), json!({"top": top, "left": left, "bottom": bottom, "right": right}));
    out.insert("pitch".into(), json!(pitch));
    out.insert("bitsPerPixel".into(), json!(bits));
    out.insert("paletteCastLib".into(), palette_lib);
    out.insert("paletteCastId".into(), palette_id);
    out.insert("registration".into(), json!({"x": reg_x, "y": reg_y}));
    out.insert("scroll".into(), json!({"x": scroll_x, "y": scroll_y}));
    out.insert("editVersion".into(), json!(edit_version));
    out.insert("updateFlags".into(), json!(update_flags));
    Ok(out)
}

fn parse_sound_header(chunk: &[u8]) -> Result<Map<String, Value>, String> {
    let mut out = Map::new();
    let names = [
        "offset", "size", "playbackStart", "playbackStartFrame", "loopStart", "loopStartFrame", "loopEnd",
        "loopEndFrame", "playbackEnd", "playbackEndFrame", "numFrames", "sampleRate", "byteRate",
    ];
    let mut values = HashMap::new();
    for (i, name) in names.iter().enumerate() {
        let value = be_i32(chunk, i * 4)?;
        values.insert(*name, value);
        out.insert(name.to_string(), json!(value));
    }
    let raw = chunk.get(52..68).ok_or("offset is out of range")?;
    let compression: String = js::windows_1252(raw).replace('\0', "");
    out.insert("compressionType".into(), json!(compression.trim_matches(super::source::js_space)));
    for (i, name) in ["bitsPerSample", "bytesPerSample", "channels", "bytesPerFrame"].iter().enumerate() {
        out.insert(name.to_string(), json!(be_i32(chunk, 68 + i * 4)?));
    }
    let rate = values["sampleRate"];
    out.insert(
        "durationSeconds".into(),
        if rate > 0 { js::num(values["numFrames"] as f64 / rate as f64) } else { Value::Null },
    );
    Ok(out)
}

/// `path.parse(p).name` (POSIX).
pub fn parse_name(p: &str) -> String {
    let trimmed = if p.len() > 1 { p.trim_end_matches('/') } else { p };
    let base = trimmed.rsplit('/').next().unwrap_or("");
    let ext = if base == ".." {
        "."
    } else {
        match base.rfind('.') {
            Some(at) if at > 0 => &base[at..],
            _ => "",
        }
    };
    base[..base.len() - ext.len()].to_string()
}

/// `[...lingo.matchAll(/^on\s+([^\s]+).*$/gim)].map(m => m[1])`
fn handlers(lingo: &str) -> Vec<String> {
    let chars: Vec<(usize, char)> = lingo.char_indices().collect();
    let terminator = |c: char| matches!(c, '\n' | '\r' | '\u{2028}' | '\u{2029}');
    let mut out = Vec::new();
    let mut i = 0;
    while i < chars.len() {
        let line_start = i == 0 || terminator(chars[i - 1].1);
        if line_start && i + 1 < chars.len() && chars[i].1.eq_ignore_ascii_case(&'o') && chars[i + 1].1.eq_ignore_ascii_case(&'n') {
            let mut j = i + 2;
            let spaces = j;
            while j < chars.len() && super::source::js_space(chars[j].1) {
                j += 1;
            }
            let word = j;
            while j < chars.len() && !super::source::js_space(chars[j].1) {
                j += 1;
            }
            if j > spaces && word > spaces && j > word {
                let start = chars[word].0;
                let end = chars.get(j).map(|c| c.0).unwrap_or(lingo.len());
                out.push(lingo[start..end].to_string());
                // `.*$` consumes the rest of the line.
                while j < chars.len() && !terminator(chars[j].1) {
                    j += 1;
                }
                i = j.max(i + 1);
                continue;
            }
        }
        i += 1;
    }
    out
}

/// `/\b(?:openXLib|fileio)\b/i`
fn uses_file_io(lingo: &str) -> bool {
    let lower = lingo.to_ascii_lowercase();
    let bytes = lower.as_bytes();
    let word = |b: u8| b.is_ascii_alphanumeric() || b == b'_';
    for needle in ["openxlib", "fileio"] {
        let mut from = 0;
        while let Some(at) = lower[from..].find(needle) {
            let start = from + at;
            let end = start + needle.len();
            if (start == 0 || !word(bytes[start - 1])) && (end == bytes.len() || !word(bytes[end])) {
                return true;
            }
            from = start + 1;
        }
    }
    false
}

pub fn analyze(fs: &mut dyn Files, media: &str, dumps: &str, output: &str, policy: &Value, now: &str) -> Result<Value, String> {
    let sources = source_files(fs, media, &directories(policy))?;
    let names: Vec<String> = sources.iter().map(|s| s.name.clone()).collect();
    let movie_names: HashSet<String> = names
        .iter()
        .filter(|n| n.to_ascii_lowercase().ends_with(".dxr"))
        .map(|n| parse_name(n).to_lowercase())
        .collect();
    let lingo_dir = files::join(output, "lingo");
    fs.mkdir_all(&lingo_dir)?;
    for (existing, _) in fs.list(&lingo_dir)? {
        if existing.ends_with(".lingo") && !names.contains(&existing[..existing.len() - 6].to_string()) {
            return Err(format!("unmanifested script in recovery directory: {existing}"));
        }
    }
    let mut records = Vec::new();
    for source in &sources {
        let input = files::join(media, &source.path);
        let size = fs.size(&input)?;
        let dump: Dump = read_dump(fs, dumps, &file_key(&source.path))?;
        let script_dump = normalized_scripts(&dump.json, &dump.scripts)?;
        let version = script_dump["version"].as_i64().unwrap_or(0);
        let config = dump.parsed(if version < 600 { "VWCF" } else { "DRCF" });
        let empty = Vec::new();
        let cast_list = dump.parsed("CAS*").and_then(|c| c["memberIDs"].as_array()).unwrap_or(&empty);
        let minimum = config.and_then(|c| c["minMember"].as_i64()).unwrap_or(1);
        let section_to_member: HashMap<i64, i64> = cast_list
            .iter()
            .enumerate()
            .map(|(i, s)| (s.as_i64().unwrap_or(0), minimum + i as i64))
            .collect();
        let key_entries = active_key_entries(dump.parsed("KEY*"))?;
        let mut children: HashMap<i64, Vec<(String, i64)>> = HashMap::new();
        for entry in &key_entries {
            let (Some(cast), Some(section)) = (entry["castID"].as_i64(), entry["sectionID"].as_i64()) else { continue };
            children.entry(cast).or_default().push((entry["fourCC"].as_str().unwrap_or("").to_string(), section));
        }
        let casts: Vec<&Value> = dump.json.iter().filter(|c| c["fourCC"] == "CASt").collect();
        let mut type_counts = Map::new();
        for cast in &casts {
            let key = match &cast["data"]["type"] {
                Value::Null => "undefined".to_string(),
                v => v.to_string(),
            };
            let n = type_counts.get(&key).and_then(Value::as_u64).unwrap_or(0);
            type_counts.insert(key, json!(n + 1));
        }
        let mut bitmaps = Vec::new();
        let mut sounds = Vec::new();
        for cast in &casts {
            let id = cast["id"].as_i64().unwrap_or(0);
            let resources = children.get(&id).cloned().unwrap_or_default();
            let find = |fourcc: &str| resources.iter().find(|r| r.0 == fourcc).map(|r| r.1);
            let member_id = section_to_member.get(&id).map(|m| json!(m)).unwrap_or(Value::Null);
            let name = cast["data"]["info"].get("name").filter(|v| !v.is_null()).cloned().unwrap_or(json!(""));
            match cast["data"]["type"].as_i64() {
                Some(1) => {
                    let Some(raw) = dump.chunk("CASt", id) else { continue };
                    let bitmap = parse_bitmap(&raw.data, version)?;
                    let bitd = find("BITD").and_then(|s| dump.chunk("BITD", s));
                    let mut entry = Map::new();
                    entry.insert("memberId".into(), member_id);
                    entry.insert("sectionId".into(), json!(id));
                    entry.insert("name".into(), name);
                    let decoded = bitmap["pitch"].as_i64().unwrap() * bitmap["height"].as_i64().unwrap();
                    entry.extend(bitmap);
                    entry.insert("compressedBytes".into(), json!(bitd.map(|b| b.data.len()).unwrap_or(0)));
                    entry.insert("decodedBytes".into(), json!(decoded));
                    entry.insert("contentSha256".into(), bitd.map(|b| json!(sha256(&b.data))).unwrap_or(Value::Null));
                    bitmaps.push(Value::Object(entry));
                }
                Some(6) => {
                    let header = find("sndH").and_then(|s| dump.chunk("sndH", s));
                    let mut samples: Option<Vec<u8>> = find("sndS").and_then(|s| dump.chunk("sndS", s)).map(|c| c.data.clone());
                    let mut legacy = None;
                    if version < 600 {
                        if let Some(section) = find("snd ") {
                            let pcm = sound_resource(&dump.chunk("snd ", section).ok_or("missing snd chunk")?.data)?;
                            legacy = Some(json!({
                                "sampleRate": pcm.rate, "sampleFrames": pcm.frames, "channels": pcm.channels,
                                "bitsPerSample": pcm.bits, "bytesPerFrame": pcm.channels * pcm.bits / 8,
                                "durationSeconds": js::num(pcm.frames as f64 / pcm.rate as f64),
                                "loopStart": pcm.loop_start, "loopEnd": pcm.loop_end,
                            }));
                            samples = Some(pcm.samples);
                        }
                    }
                    let mut entry = Map::new();
                    entry.insert("memberId".into(), member_id);
                    entry.insert("sectionId".into(), json!(id));
                    entry.insert("name".into(), name);
                    let flags = cast["data"]["info"]["flags"].as_i64().unwrap_or(0);
                    entry.insert("looping".into(), json!(flags & 16 == 0));
                    match legacy {
                        Some(Value::Object(fields)) => entry.extend(fields),
                        _ => {
                            if let Some(h) = header {
                                entry.extend(parse_sound_header(&h.data)?);
                            }
                        }
                    }
                    entry.insert("sampleBytes".into(), json!(samples.as_ref().map(|s| s.len()).unwrap_or(0)));
                    entry.insert("contentSha256".into(), samples.as_ref().map(|s| json!(sha256(s))).unwrap_or(Value::Null));
                    sounds.push(Value::Object(entry));
                }
                _ => {}
            }
        }
        let mut lingo_parts = Vec::new();
        let mut handler_count = 0usize;
        for cast in script_dump["casts"].as_array().into_iter().flatten() {
            let cast_name = cast["name"].as_str().filter(|s| !s.is_empty()).unwrap_or("Internal");
            for script in cast["scripts"].as_array().into_iter().flatten() {
                handler_count += 1;
                let member_name = script["memberName"].as_str().filter(|s| !s.is_empty()).unwrap_or("(unnamed)");
                lingo_parts.push(format!(
                    "-- cast: {cast_name}; member: {}; type: {}; name: {member_name}\n{}",
                    js_string(&script["memberId"]),
                    js_string(&script["scriptType"]),
                    script["lingo"].as_str().unwrap_or("")
                ));
            }
        }
        let lingo = lingo_parts.join("\n");
        fs.write(&files::join(&lingo_dir, &format!("{}.lingo", source.name)), lingo.as_bytes())?;
        let current = parse_name(&source.name).to_lowercase();
        let references: BTreeSet<String> = quoted_strings(&lingo)
            .into_iter()
            .map(|q| parse_name(&q.replace('\\', "/")).to_lowercase())
            .filter(|c| *c != current && movie_names.contains(c))
            .collect();
        let mut chunk_sums = Map::new();
        for chunk in &dump.chunks {
            let entry = chunk_sums.entry(chunk.fourcc.clone()).or_insert_with(|| json!({"count": 0, "bytes": 0}));
            entry["count"] = json!(entry["count"].as_u64().unwrap() + 1);
            entry["bytes"] = json!(entry["bytes"].as_u64().unwrap() + chunk.data.len() as u64);
        }
        let stage = match config {
            Some(c) => {
                let n = |k: &str| c[k].as_i64().unwrap_or(0);
                json!({
                    "width": n("movieRight") - n("movieLeft"),
                    "height": n("movieBottom") - n("movieTop"),
                    "bitDepth": c["bitDepth"], "frameRate": c["frameRate"], "platform": c["platform"],
                })
            }
            None => Value::Null,
        };
        records.push(json!({
            "name": source.name,
            "sourcePath": source.path,
            "bytes": size,
            "kind": if source.name.ends_with(".CXT") { "cast" } else { "movie" },
            "directorVersion": script_dump["version"],
            "stage": stage,
            "chunks": chunk_sums,
            "castTypeCounts": type_counts,
            "bitmaps": bitmaps,
            "sounds": sounds,
            "scripts": {"count": handler_count, "sourceBytes": lingo.len(), "handlers": handlers(&lingo)},
            "movieReferences": references.into_iter().collect::<Vec<_>>(),
            "usesFileIo": uses_file_io(&lingo),
        }));
    }
    let sum = |f: &dyn Fn(&Value) -> f64| records.iter().map(f).sum::<f64>();
    let inner = |file: &Value, list: &str, key: &str| -> f64 {
        file[list].as_array().into_iter().flatten().map(|x| x[key].as_f64().unwrap_or(0.0)).sum()
    };
    let totals = json!({
        "files": records.len(),
        "movies": records.iter().filter(|f| f["kind"] == "movie").count(),
        "casts": records.iter().filter(|f| f["kind"] == "cast").count(),
        "bytes": js::num(sum(&|f| f["bytes"].as_f64().unwrap_or(0.0))),
        "scripts": js::num(sum(&|f| f["scripts"]["count"].as_f64().unwrap_or(0.0))),
        "scriptSourceBytes": js::num(sum(&|f| f["scripts"]["sourceBytes"].as_f64().unwrap_or(0.0))),
        "bitmaps": js::num(sum(&|f| f["bitmaps"].as_array().map_or(0.0, |b| b.len() as f64))),
        "bitmapCompressedBytes": js::num(sum(&|f| inner(f, "bitmaps", "compressedBytes"))),
        "bitmapDecodedBytes": js::num(sum(&|f| inner(f, "bitmaps", "decodedBytes"))),
        "sounds": js::num(sum(&|f| f["sounds"].as_array().map_or(0.0, |b| b.len() as f64))),
        "soundSampleBytes": js::num(sum(&|f| inner(f, "sounds", "sampleBytes"))),
        "soundDurationSeconds": js::num(sum(&|f| inner(f, "sounds", "durationSeconds"))),
    });
    let inventory = json!({"generatedAt": now, "mediaDir": media, "totals": totals, "files": records});
    fs.write(&files::join(output, "inventory.json"), (js::stringify_pretty(&inventory) + "\n").as_bytes())?;
    Ok(totals)
}

/// A value interpolated into a template literal.
fn js_string(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        Value::Null => "null".into(),
        Value::Number(n) => n.as_f64().map(js::number).unwrap_or_default(),
        other => other.to_string(),
    }
}

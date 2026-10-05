//! The recovery/accountability schema: tools/director/source-files.mjs and
//! source-model.mjs, ported.

use std::collections::{HashMap, HashSet};

use serde_json::{json, Map, Value};
use sha2::{Digest, Sha256};

use super::dump::Dump;
use super::files::{self, Files};
use super::js;
use super::xtra;

pub const SCHEMA_VERSION: u32 = 1;
pub const SCORE_TYPES: [&str; 3] = ["VWSC", "VWLB", "VWFI"];

pub fn cast_type(kind: i64) -> Option<&'static str> {
    Some(match kind {
        1 => "bitmap",
        2 => "film-loop",
        3 => "text",
        4 => "palette",
        5 => "picture",
        6 => "sound",
        7 => "button",
        8 => "shape",
        9 => "movie",
        10 => "digital-video",
        11 => "script",
        12 => "rte",
        14 => "transition",
        15 => "xtra",
        _ => return None,
    })
}

pub fn sha256(data: &[u8]) -> String {
    js::hex(&Sha256::digest(data))
}

pub fn stable_id(kind: &str, locator: &[Value]) -> String {
    format!("{kind}:{}", sha256(js::stringify(&Value::Array(locator.to_vec())).as_bytes()))
}

fn require(condition: bool, message: impl Into<String>) -> Result<(), String> {
    if condition { Ok(()) } else { Err(message.into()) }
}

/// `localeCompare` (ICU root collation) for the ASCII names Director
/// resources have: primary weights ignore case and put punctuation before
/// digits before letters; equal primaries then put lower case first.
pub fn locale_order(a: &str, b: &str) -> std::cmp::Ordering {
    const PUNCTUATION: &str = " _-,;:!?.'\"()[]{}@*/\\&#%`^+<=>|~$";
    let primary = |c: char| -> u32 {
        if let Some(i) = PUNCTUATION.find(c) {
            1 + i as u32
        } else if c.is_ascii_digit() {
            100 + c as u32 - '0' as u32
        } else if c.is_ascii_alphabetic() {
            200 + c.to_ascii_lowercase() as u32 - 'a' as u32
        } else {
            1000 + c as u32
        }
    };
    a.chars()
        .map(primary)
        .cmp(b.chars().map(primary))
        .then_with(|| a.chars().map(|c| c.is_ascii_uppercase()).cmp(b.chars().map(|c| c.is_ascii_uppercase())))
}

pub struct SourceFile {
    pub name: String,
    pub path: String,
}

/// source-files.mjs `sourceFiles`: the audited directories' Director files,
/// names folded to their runtime namespace, collisions rejected.
pub fn source_files(fs: &dyn Files, root: &str, directories: &[String]) -> Result<Vec<SourceFile>, String> {
    let base = files::normalize(root);
    let mut out = Vec::new();
    let mut names = HashSet::new();
    for directory in directories {
        let folder = files::join(&base, directory);
        if within(&base, &folder).is_none() {
            return Err("Director source directory escapes media root".into());
        }
        for (item, irregular) in fs.list(&folder)? {
            let lower = item.to_ascii_lowercase();
            if !(lower.ends_with(".dxr") || lower.ends_with(".cxt") || lower.ends_with(".cst")) {
                continue;
            }
            if irregular {
                return Err(format!("Director source is not a regular file: {item}"));
            }
            let mut name = item.to_uppercase();
            if name.ends_with(".CST") {
                name = format!("{}.CXT", &name[..name.len() - 4]);
            }
            let (stem, ext) = name.rsplit_once('.').unwrap();
            if stem.is_empty() || !stem.bytes().all(|b| b.is_ascii_uppercase() || b.is_ascii_digit() || b == b'_')
                || !(ext == "DXR" || ext == "CXT")
            {
                return Err(format!("unsupported source name: {item}"));
            }
            if !names.insert(name.clone()) {
                return Err(format!("duplicate Director source name: {name}"));
            }
            let path = within(&base, &files::join(&folder, &item)).unwrap();
            out.push(SourceFile { name, path });
        }
    }
    out.sort_by(|a, b| locale_order(&a.name, &b.name));
    Ok(out)
}

/// `path` relative to `base` (both normalized), or None when it lies outside.
fn within(base: &str, path: &str) -> Option<String> {
    let rest = match base {
        "." => (path != ".." && !path.starts_with("../") && !path.starts_with('/')).then_some(path)?,
        "/" => path.strip_prefix('/')?,
        _ if path == base => "",
        _ => path.strip_prefix(base)?.strip_prefix('/')?,
    };
    Some(if rest == "." { String::new() } else { rest.to_string() })
}

pub fn active_key_entries(table: Option<&Value>) -> Result<Vec<Value>, String> {
    let table = table.ok_or("missing KEY* table")?;
    let entries = table["entries"].as_array().ok_or("missing KEY* table")?;
    let used = table["usedCount"].as_i64();
    let count = table["entryCount"].as_i64();
    require(
        matches!((used, count), (Some(u), Some(c)) if u >= 0 && u <= c && c as usize == entries.len()),
        "invalid KEY* used/capacity counts",
    )?;
    Ok(entries[..used.unwrap() as usize].to_vec())
}

/// D7 external MCsL entries can reuse the internal resource ID; restore the
/// owning local namespace only when all copies agree.
pub fn normalized_scripts(parsed: &[Value], scripts: &Value) -> Result<Value, String> {
    let empty = Vec::new();
    let entries = parsed
        .iter()
        .find(|c| c["fourCC"] == "MCsL")
        .and_then(|c| c["data"]["entries"].as_array())
        .unwrap_or(&empty);
    let ids: Vec<&Value> = entries.iter().map(|e| &e["id"]).collect();
    let unique: HashSet<String> = ids.iter().map(|v| v.to_string()).collect();
    if scripts["version"] != 700 || unique.len() == ids.len() {
        return Ok(scripts.clone());
    }
    let local_duplicate = entries.iter().any(|e| {
        e["filePath"].as_str().is_none_or(str::is_empty)
            && entries.iter().filter(|o| o["id"] == e["id"]).count() > 1
    });
    if !local_duplicate {
        return Ok(scripts.clone());
    }
    let local: Vec<&Value> = entries.iter().filter(|e| e["filePath"].as_str().is_none_or(str::is_empty)).collect();
    require(
        local.len() == 1 && entries.iter().all(|e| e["id"] == local[0]["id"]),
        "unsupported D7 shared cast resource IDs",
    )?;
    let casts = scripts["casts"].as_array().ok_or("scripts without casts")?;
    require(
        casts.len() == entries.len()
            && casts.iter().all(|c| js::stringify(&c["scripts"]) == js::stringify(&casts[0]["scripts"])),
        "D7 duplicated script namespaces disagree",
    )?;
    let mut out = scripts.as_object().cloned().unwrap_or_default();
    out.insert("casts".into(), json!([{"name": local[0]["name"], "scripts": casts[0]["scripts"]}]));
    Ok(Value::Object(out))
}

fn text(v: &Value) -> &str {
    v.as_str().unwrap_or("")
}

/// Bytecode handlers and instruction offsets: the stable semantic-site
/// denominator of a script.
pub fn script_sites(script: &Value, file_id: &str, library_id: &Value) -> Result<(String, Vec<Value>, Vec<Value>), String> {
    let id = stable_id("script", &[json!(file_id), library_id.clone(), script["scriptId"].clone()]);
    let mut handlers: Vec<Value> = Vec::new();
    let mut sites: Vec<Value> = Vec::new();
    let mut handler: Option<String> = None;
    for line in split_lines(text(&script["bytecode"])) {
        if let Some((name, _)) = handler_declaration(line) {
            let hid = stable_id("handler", &[json!(id), json!(handlers.len())]);
            handlers.push(json!({"id": hid, "ordinal": handlers.len(), "name": name, "declaration": line}));
            handler = Some(hid);
        }
        let Some((offset, opcode, disassembly)) = instruction(line) else { continue };
        let hid = handler.clone().ok_or_else(|| format!("instruction before handler in {id}"))?;
        sites.push(json!({
            "id": stable_id("site", &[json!(hid), json!(offset)]),
            "handler_id": hid,
            "bytecode_offset": offset,
            "opcode": opcode,
            "disassembly": disassembly,
        }));
    }
    require(!handlers.is_empty() || text(&script["lingo"]).trim().is_empty(), format!("missing bytecode handlers: {id}"))?;
    let unique: HashSet<&str> = sites.iter().map(|s| s["id"].as_str().unwrap()).collect();
    require(unique.len() == sites.len(), format!("duplicate sites: {id}"))?;
    Ok((id, handlers, sites))
}

/// `split(/\r?\n/)`
pub fn split_lines(text: &str) -> Vec<&str> {
    text.split('\n').map(|l| l.strip_suffix('\r').unwrap_or(l)).collect()
}

fn is_word(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '_'
}

/// `/^on\s+([\w]+)\b(.*)$/i`
fn handler_declaration(line: &str) -> Option<(&str, &str)> {
    let rest = line.get(..2).filter(|p| p.eq_ignore_ascii_case("on")).map(|_| &line[2..])?;
    let trimmed = rest.trim_start_matches(|c: char| js_space(c));
    if trimmed.len() == rest.len() {
        return None;
    }
    let end = trimmed.find(|c: char| !is_word(c)).unwrap_or(trimmed.len());
    if end == 0 {
        return None;
    }
    // `\b` after a maximal \w+ run always holds.
    Some((&trimmed[..end], &trimmed[end..]))
}

/// JavaScript `\s`.
pub fn js_space(c: char) -> bool {
    matches!(c, '\t' | '\n' | '\u{b}' | '\u{c}' | '\r' | ' ' | '\u{a0}' | '\u{1680}' | '\u{2028}' | '\u{2029}'
        | '\u{202f}' | '\u{205f}' | '\u{3000}' | '\u{feff}') || ('\u{2000}'..='\u{200a}').contains(&c)
}

/// `/^\s*\[\s*(\d+)\]\s+(\w+)(?:\s+(.*))?$/`
fn instruction(line: &str) -> Option<(u64, &str, &str)> {
    let rest = line.trim_start_matches(js_space).strip_prefix('[')?;
    let rest = rest.trim_start_matches(js_space);
    let digits = rest.find(|c: char| !c.is_ascii_digit()).unwrap_or(rest.len());
    if digits == 0 {
        return None;
    }
    let offset: u64 = rest[..digits].parse().ok()?;
    let rest = rest[digits..].strip_prefix(']')?;
    let after = rest.trim_start_matches(js_space);
    if after.len() == rest.len() {
        return None;
    }
    let word = after.find(|c: char| !is_word(c)).unwrap_or(after.len());
    if word == 0 {
        return None;
    }
    let opcode = &after[..word];
    let tail = &after[word..];
    if tail.is_empty() {
        return Some((offset, opcode, ""));
    }
    // (?:\s+(.*))?$ — `.` excludes line terminators.
    let body = tail.trim_start_matches(js_space);
    if body.len() == tail.len() || body.contains(['\n', '\r', '\u{2028}', '\u{2029}']) {
        // The group must consume the whole rest; whitespace-only tails match with an empty group.
        return if tail.chars().all(js_space) && !tail.contains(['\n', '\r']) { Some((offset, opcode, "")) } else { None };
    }
    Some((offset, opcode, body))
}

pub struct FileInput<'a> {
    pub name: &'a str,
    pub bytes: &'a [u8],
    pub dump: &'a Dump,
}

fn object(pairs: Vec<(&str, Value)>) -> Value {
    Value::Object(pairs.into_iter().map(|(k, v)| (k.to_string(), v)).collect::<Map<String, Value>>())
}

/// source-model's `fileModel`.
pub fn file_model(input: FileInput, movie_names: &HashSet<String>) -> Result<Value, String> {
    let FileInput { name, bytes, dump } = input;
    let scripts = normalized_scripts(&dump.json, &dump.scripts)?;
    let valid = name.rsplit_once('.').is_some_and(|(stem, ext)| {
        !stem.is_empty()
            && stem.bytes().all(|b| b.is_ascii_uppercase() || b.is_ascii_digit() || b == b'_')
            && matches!(ext, "DXR" | "CXT" | "EXE" | "DAT")
    });
    require(valid, format!("unsupported source name: {name}"))?;
    let content_sha256 = sha256(bytes);
    let id = stable_id("file", &[json!(name), json!(content_sha256)]);
    let mut unresolved: Vec<Value> = Vec::new();
    let mut chunk_map: HashMap<String, Value> = HashMap::new();
    let mut order: Vec<usize> = (0..dump.chunks.len()).collect();
    order.sort_by(|&a, &b| {
        let (x, y) = (&dump.chunks[a], &dump.chunks[b]);
        x.id.cmp(&y.id).then_with(|| locale_order(&x.fourcc, &y.fourcc))
    });
    let mut resources = Vec::new();
    for index in order {
        let chunk = &dump.chunks[index];
        let key = format!("{}:{}", chunk.fourcc, chunk.id);
        require(!chunk_map.contains_key(&key), format!("duplicate chunk {name}/{key}"))?;
        let resource = object(vec![
            ("id", json!(stable_id("resource", &[json!(id), json!(chunk.fourcc), json!(chunk.id)]))),
            ("fourcc", json!(chunk.fourcc)),
            ("section_id", json!(chunk.id)),
            ("length", json!(chunk.data.len())),
            ("content_sha256", json!(sha256(&chunk.data))),
        ]);
        chunk_map.insert(key, resource.clone());
        resources.push(resource);
    }
    let parsed_map: HashMap<String, &Value> =
        dump.json.iter().map(|c| (format!("{}:{}", text(&c["fourCC"]), c["id"]), &c["data"])).collect();
    let key_table = active_key_entries(dump.parsed("KEY*"))?;
    let mut links = Vec::new();
    for (index, link) in key_table.iter().enumerate() {
        let resource = chunk_map.get(&format!("{}:{}", text(&link["fourCC"]), link["sectionID"]));
        let lid = stable_id("link", &[json!(id), json!(index)]);
        let entry = object(vec![
            ("id", json!(lid)),
            ("index", json!(index)),
            ("owner_section_id", link["castID"].clone()),
            ("fourcc", link["fourCC"].clone()),
            ("section_id", link["sectionID"].clone()),
            ("resource_id", resource.map(|r| r["id"].clone()).unwrap_or(Value::Null)),
        ]);
        if resource.is_none() {
            unresolved.push(json!({"id": lid, "reason": "unresolved-resource-link"}));
        }
        links.push(entry);
    }
    let version = scripts["version"].as_i64().unwrap_or(0);
    let config = dump.parsed(if version < 600 { "VWCF" } else { "DRCF" }).cloned();
    let cast_entries = dump.parsed("MCsL").map(|m| m["entries"].clone()).unwrap_or(json!([]));
    let mut libraries = Vec::new();
    for cast in dump.parsed_all("CAS*") {
        let section = &cast["id"];
        let mut owners: Vec<Value> = Vec::new();
        for link in &key_table {
            if link["fourCC"] == "CAS*" && &link["sectionID"] == section && !owners.contains(&link["castID"]) {
                owners.push(link["castID"].clone());
            }
        }
        require(owners.len() == 1, format!("{name}: ambiguous CAS* owner {section}"))?;
        let library_id = owners.remove(0);
        let description = cast_entries.as_array().and_then(|e| e.iter().find(|e| e["id"] == library_id));
        let minimum = description
            .and_then(|d| d.get("minMember"))
            .filter(|v| !v.is_null())
            .or_else(|| config.as_ref().and_then(|c| c.get("minMember")))
            .cloned()
            .unwrap_or(Value::Null);
        require(minimum.is_i64() || minimum.is_u64(), format!("{name}: missing minimum cast member"))?;
        let library_name = description
            .and_then(|d| d.get("name"))
            .filter(|v| !v.is_null())
            .cloned()
            .unwrap_or_else(|| json!(if name.ends_with(".CXT") { "External" } else { "Internal" }));
        libraries.push(object(vec![
            ("id", json!(stable_id("library", &[json!(id), library_id.clone()]))),
            ("library_id", library_id),
            ("name", library_name),
            ("section_id", section.clone()),
            ("minimum_member", minimum),
            ("members", parsed_map[&format!("CAS*:{section}")]["memberIDs"].clone()),
        ]));
    }
    let mut member_locators: HashMap<i64, (Value, i64)> = HashMap::new();
    for library in &libraries {
        let minimum = library["minimum_member"].as_i64().unwrap();
        for (index, section) in library["members"].as_array().into_iter().flatten().enumerate() {
            let section = section.as_i64().unwrap_or(0);
            if section <= 0 {
                continue;
            }
            require(!member_locators.contains_key(&section), format!("{name}: ambiguous member section {section}"))?;
            member_locators.insert(section, (library["library_id"].clone(), minimum + index as i64));
        }
    }
    let mut casts: Vec<&Value> = dump.json.iter().filter(|c| c["fourCC"] == "CASt").collect();
    casts.sort_by_key(|c| c["id"].as_i64().unwrap_or(0));
    let mut members = Vec::new();
    for cast in casts {
        let section = cast["id"].as_i64().unwrap_or(0);
        let (library_id, member_id) = member_locators
            .get(&section)
            .cloned()
            .ok_or_else(|| format!("{name}: CASt {section} absent from all libraries"))?;
        let mid = stable_id("member", &[json!(id), library_id.clone(), json!(member_id)]);
        let data = &cast["data"];
        let kind = data["type"].as_i64().unwrap_or(-1);
        if cast_type(kind).is_none() {
            unresolved.push(json!({"id": mid, "reason": "unknown-cast-type", "type": data["type"]}));
        }
        let raw = dump.chunk("CASt", section).map(|c| c.data.as_slice());
        let mut transition = Value::Null;
        let mut xtra_value = None;
        if kind == 15 {
            let model = xtra::xtra_model(raw.ok_or("missing CASt bytes")?)?;
            unresolved.push(json!({"id": mid, "reason": "xtra-runtime-not-implemented", "symbol": model.symbol}));
            xtra_value = Some(json!({"symbol": model.symbol, "payload_offset": model.payload_offset,
                "payload_length": model.payload_length}));
        }
        if kind == 14 {
            let raw = raw.ok_or("missing CASt bytes")?;
            let start = 12 + data["infoLen"].as_u64().unwrap_or(0) as usize;
            require(data["specificDataLen"] == 6 && start + 6 <= raw.len(), format!("{name}: unsupported transition member layout"))?;
            let s = &raw[start..start + 6];
            transition = json!({"time_raw": s[0], "chunk_size": s[1], "type": s[2], "flags_raw": s[3],
                "duration_ms": (s[4] as u32) << 8 | s[5] as u32, "reference_verified": false});
        }
        let resource_ids: Vec<Value> = {
            let mut seen: Vec<Value> = Vec::new();
            for link in &links {
                if link["owner_section_id"] == section && !link["resource_id"].is_null() && !seen.contains(&link["resource_id"]) {
                    seen.push(link["resource_id"].clone());
                }
            }
            seen
        };
        let mut member = object(vec![
            ("id", json!(mid)),
            ("library_id", library_id),
            ("member_id", json!(member_id)),
            ("section_id", json!(section)),
            ("type", data["type"].clone()),
            ("kind", json!(cast_type(kind).unwrap_or("unclassified"))),
            ("name", data["info"].get("name").filter(|v| !v.is_null()).cloned().unwrap_or(json!(""))),
            ("content_sha256", chunk_map[&format!("CASt:{section}")]["content_sha256"].clone()),
            ("script_id", data["info"].get("scriptId").filter(|v| !v.is_null()).cloned().unwrap_or(json!(0))),
            ("resource_ids", Value::Array(resource_ids)),
            ("metadata", data.clone()),
            ("transition", transition),
        ]);
        if let Some(x) = xtra_value {
            member.as_object_mut().unwrap().insert("xtra".into(), x);
        }
        members.push(member);
    }
    let mut script_records = Vec::new();
    for cast in scripts["casts"].as_array().into_iter().flatten() {
        let library = libraries.iter().find(|l| l["name"] == cast["name"]);
        let list = cast["scripts"].as_array().cloned().unwrap_or_default();
        require(library.is_some() || list.is_empty(), format!("{name}: unknown script cast {}", text(&cast["name"])))?;
        for script in list {
            let library = library.unwrap();
            let member = members
                .iter()
                .find(|m| m["library_id"] == library["library_id"] && m["member_id"] == script["memberId"])
                .ok_or_else(|| format!("{name}: script {} missing member", script["scriptId"]))?;
            let context_link = key_table.iter().find(|l| {
                l["castID"] == library["library_id"] && (l["fourCC"] == "Lctx" || l["fourCC"] == "LctX")
            });
            let context = context_link.and_then(|l| parsed_map.get(&format!("{}:{}", text(&l["fourCC"]), l["sectionID"])));
            let section = context
                .and_then(|c| c["sectionMap"].get((script["scriptId"].as_i64().unwrap_or(0) - 1) as usize))
                .map(|s| s["sectionID"].clone())
                .unwrap_or(Value::Null);
            let raw = chunk_map
                .get(&format!("Lscr:{}", if section.is_null() { "undefined".into() } else { section.to_string() }))
                .ok_or_else(|| format!("{name}: script {} missing Lscr", script["scriptId"]))?;
            let (sid, handlers, sites) = script_sites(&script, &id, &library["library_id"])?;
            script_records.push(object(vec![
                ("id", json!(sid)),
                ("handlers", Value::Array(handlers)),
                ("sites", Value::Array(sites)),
                ("member_id", member["id"].clone()),
                ("library_id", library["library_id"].clone()),
                ("source_script_id", script["scriptId"].clone()),
                ("resource_id", raw["id"].clone()),
                ("content_sha256", raw["content_sha256"].clone()),
                ("script_type", script["scriptType"].clone()),
                ("lingo", script["lingo"].clone()),
            ]));
        }
    }
    let mut score = Vec::new();
    for resource in &resources {
        let fourcc = text(&resource["fourcc"]);
        if !SCORE_TYPES.contains(&fourcc) {
            continue;
        }
        unresolved.push(json!({"id": resource["id"], "reason": "score-semantics-not-decoded", "fourcc": fourcc}));
        let mut entry = resource.as_object().unwrap().clone();
        entry.insert("decoded".into(), json!(false));
        entry.insert("blob".into(), json!(format!("score/{}.bin", text(&resource["content_sha256"]))));
        score.push(Value::Object(entry));
    }
    let mut destinations: std::collections::BTreeSet<String> = Default::default();
    let own = &name[..name.len() - 4];
    for script in &script_records {
        for quoted in quoted_strings(text(&script["lingo"])) {
            let target = quoted.replace('\\', "/");
            let target = target.rsplit('/').next().unwrap_or("");
            let target = strip_suffix_ignore_case(target, ".dxr").to_uppercase();
            if target != own && movie_names.contains(&target) {
                destinations.insert(target);
            }
        }
    }
    let edges: Vec<Value> = destinations
        .iter()
        .map(|target| {
            json!({"id": stable_id("edge", &[json!(id), json!(format!("{target}.DXR"))]),
                "target": format!("{target}.DXR"), "discovery": "string-reference", "semantics_verified": false})
        })
        .collect();
    let mut model = object(vec![
        ("id", json!(id)),
        ("name", json!(name)),
        ("content_sha256", json!(content_sha256)),
        ("bytes", json!(bytes.len())),
        ("director_version", scripts["version"].clone()),
        ("kind", json!(if name.ends_with(".CXT") { "cast" } else { "movie" })),
        ("config", config.clone().unwrap_or(Value::Null)),
        ("libraries", Value::Array(libraries)),
        ("cast_links", cast_entries),
        ("resources", Value::Array(resources)),
        ("links", Value::Array(links)),
        ("members", Value::Array(members)),
        ("scripts", Value::Array(script_records)),
        ("score", Value::Array(score)),
        ("edges", Value::Array(edges)),
        ("unresolved", Value::Array(unresolved)),
    ]);
    // JSON.stringify drops an undefined config rather than writing null.
    if config.is_none() {
        model.as_object_mut().unwrap().remove("config");
    }
    Ok(model)
}

fn strip_suffix_ignore_case<'a>(text: &'a str, suffix: &str) -> &'a str {
    if text.len() >= suffix.len() && text.is_char_boundary(text.len() - suffix.len())
        && text[text.len() - suffix.len()..].eq_ignore_ascii_case(suffix)
    {
        &text[..text.len() - suffix.len()]
    } else {
        text
    }
}

/// `matchAll(/"([^"\r\n]+)"/g)`
pub fn quoted_strings(text: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let mut rest = text;
    while let Some(start) = rest.find('"') {
        let after = &rest[start + 1..];
        let end = after.find(['"', '\r', '\n']).unwrap_or(after.len());
        if end > 0 && after[end..].starts_with('"') {
            out.push(&after[..end]);
            rest = &after[end + 1..];
        } else {
            rest = after;
        }
    }
    out
}

pub fn obligation_ledger(files: &[&Value]) -> Result<Value, String> {
    let mut obligations = Vec::new();
    let mut add = |id: &Value, kind: &str, source: &Value| {
        obligations.push(json!({"id": id, "kind": kind, "source_id": source, "disposition": "unresolved", "evidence": []}));
    };
    for file in files {
        for script in file["scripts"].as_array().into_iter().flatten() {
            add(&script["id"], "script", &file["id"]);
            for handler in script["handlers"].as_array().into_iter().flatten() {
                add(&handler["id"], "handler", &script["id"]);
            }
            for site in script["sites"].as_array().into_iter().flatten() {
                add(&site["id"], "bytecode-site", &script["id"]);
            }
        }
        for score in file["score"].as_array().into_iter().flatten() {
            add(&score["id"], "raw-score-chunk", &file["id"]);
        }
        for edge in file["edges"].as_array().into_iter().flatten() {
            add(&edge["id"], "movie-reference", &file["id"]);
        }
    }
    let unique: HashSet<String> = obligations.iter().map(|o| o["id"].to_string()).collect();
    require(unique.len() == obligations.len(), "duplicate obligation IDs")?;
    Ok(json!({"schema_version": SCHEMA_VERSION, "review_complete": false, "obligations": obligations}))
}

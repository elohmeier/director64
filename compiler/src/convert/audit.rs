//! tools/director/source-audit.mjs: account for every Director source — the
//! parsed file models, the score chunks structural recovery reads, and the
//! policy's pinned denominators.

use std::collections::HashSet;

use serde_json::{json, Value};

use super::files::{self, Files};
use super::js::{serialize, stringify};
use super::plan::{directories, file_key, read_dump, view_key};
use super::projector::projector_view;
use super::source::{file_model, normalized_scripts, obligation_ledger, sha256, source_files, FileInput, SCHEMA_VERSION, SCORE_TYPES};

fn script_records(parsed: &[Value], scripts: &Value) -> Result<Vec<Value>, String> {
    let normalized = normalized_scripts(parsed, scripts)?;
    let mut records = Vec::new();
    for cast in normalized["casts"].as_array().into_iter().flatten() {
        for script in cast["scripts"].as_array().into_iter().flatten() {
            let mut record = serde_json::Map::new();
            record.insert("cast".into(), cast["name"].clone());
            for (k, v) in script.as_object().into_iter().flatten() {
                record.insert(k.clone(), v.clone());
            }
            records.push(Value::Object(record));
        }
    }
    records.sort_by_key(|r| r["scriptId"].as_i64().unwrap_or(0));
    Ok(records)
}

fn write_scores(fs: &mut dyn Files, output: &str, dump: &super::dump::Dump) -> Result<(), String> {
    for chunk in dump.chunks.iter().filter(|c| SCORE_TYPES.contains(&c.fourcc.as_str())) {
        fs.write(&files::join(output, &format!("score/{}.bin", sha256(&chunk.data))), &chunk.data)?;
    }
    Ok(())
}

pub fn audit(fs: &mut dyn Files, media: &str, dumps: &str, output: &str, policy: &Value, parser_sha256: &str) -> Result<Value, String> {
    let sources = source_files(fs, media, &directories(policy))?;
    let expected = policy["director_files"].as_u64();
    if Some(sources.len() as u64) != expected {
        return Err(format!("expected {} Director files, found {}", policy["director_files"], sources.len()));
    }
    let movie_names: HashSet<String> =
        sources.iter().filter(|s| s.name.ends_with(".DXR")).map(|s| s.name[..s.name.len() - 4].to_string()).collect();
    fs.mkdir_all(&files::join(output, "score"))?;
    let mut models = Vec::new();
    let mut script_views = std::collections::HashMap::new();
    for source in &sources {
        let input = files::join(media, &source.path);
        let bytes = fs.read(&input)?;
        let dump = read_dump(fs, dumps, &file_key(&source.path))?;
        script_views.insert(source.name.clone(), script_records(&dump.json, &dump.scripts)?);
        if let Some(version) = policy.get("director_version").filter(|v| !v.is_null() && *v != 0) {
            if dump.scripts["version"] != *version {
                return Err(format!("{}: Director version {} differs from policy", source.name, dump.scripts["version"]));
            }
        }
        let mut model = file_model(FileInput { name: &source.name, bytes: &bytes, dump: &dump }, &movie_names)?;
        model.as_object_mut().unwrap().insert("source_path".into(), json!(source.path));
        models.push(model);
        write_scores(fs, output, &dump)?;
    }
    let total = |f: &dyn Fn(&Value) -> u64| -> u64 { models.iter().map(f).sum() };
    let count = |v: &Value| v.as_array().map_or(0, |a| a.len() as u64);
    let summary = json!({
        "files": models.len(),
        "scripts": total(&|f| count(&f["scripts"])),
        "handlers": total(&|f| f["scripts"].as_array().into_iter().flatten().map(|s| count(&s["handlers"])).sum()),
        "bytecode_sites": total(&|f| f["scripts"].as_array().into_iter().flatten().map(|s| count(&s["sites"])).sum()),
        "members": total(&|f| count(&f["members"])),
        "resources": total(&|f| count(&f["resources"])),
        "bitmap_references": total(&|f| f["members"].as_array().into_iter().flatten().filter(|m| m["type"] == 1).count() as u64),
        "sound_references": total(&|f| f["members"].as_array().into_iter().flatten().filter(|m| m["type"] == 6).count() as u64),
        "score_chunks": total(&|f| count(&f["score"])),
        "score_bytes": total(&|f| f["score"].as_array().into_iter().flatten().map(|c| c["length"].as_u64().unwrap_or(0)).sum()),
        "movie_references": total(&|f| count(&f["edges"])),
        "unresolved_recovery_records": total(&|f| count(&f["unresolved"])),
    });
    for (key, expected) in policy["counts"].as_object().into_iter().flatten() {
        if summary.get(key) != Some(expected) {
            return Err(format!(
                "source denominator changed: {key}={} (expected {expected})",
                summary.get(key).map(|v| v.to_string()).unwrap_or("undefined".into())
            ));
        }
    }
    let projector = policy["projector"].as_str().ok_or("policy names no projector")?;
    let projector_path = files::join(media, projector);
    let projector_bytes = fs.read(&projector_path)?;
    let selection = policy.get("projector_selection").filter(|v| !v.is_null());
    let view = projector_view(&projector_bytes, selection)?;
    let mut dump = read_dump(fs, dumps, &view_key(projector, view.offset))?;
    if let Some(version) = policy.get("director_version").filter(|v| !v.is_null() && *v != 0) {
        if dump.scripts["version"] != *version {
            return Err("projector Director version differs from policy".into());
        }
        // The version check decompiled the projector before its records were
        // read, which rewrites a protected movie's config (the JS audit's order).
        if let Some(scripted) = dump.json_after_scripts.take() {
            dump.json = scripted;
        }
    }
    let projector_name = files::file_name(projector).to_uppercase();
    let mut supplemental = file_model(FileInput { name: &projector_name, bytes: &projector_bytes, dump: &dump }, &movie_names)?;
    {
        let object = supplemental.as_object_mut().unwrap();
        object.insert("kind".into(), json!("embedded-projector-movie"));
        object.insert("archive_offset".into(), json!(view.offset));
        object.insert("archives".into(), view.archives.clone());
    }
    write_scores(fs, output, &dump)?;
    let mut dispositions = Vec::new();
    if let Some(copies) = policy["projector_copies"].as_array() {
        let archives = view.archives.as_array().map_or(0, Vec::len);
        let offsets: HashSet<String> = copies.iter().map(|c| c["offset"].to_string()).collect();
        if copies.len() + 1 != archives || offsets.len() != copies.len() {
            return Err("incomplete projector copy dispositions".into());
        }
        for copy in copies {
            let offset = copy["offset"].as_u64().unwrap_or(0) as usize;
            let movie = copy["movie"].as_str().unwrap_or("");
            if offset == view.offset || !script_views.contains_key(movie) {
                return Err("invalid projector copy target".into());
            }
            let embedded = read_dump(fs, dumps, &view_key(projector, offset))?;
            let records = script_records(&embedded.json, &embedded.scripts)?;
            let target = &script_views[movie];
            let different: Vec<Value> = records
                .iter()
                .enumerate()
                .filter(|(i, s)| serialize(s) != target.get(*i).map(serialize).unwrap_or_default())
                .map(|(_, s)| s["memberId"].clone())
                .collect();
            let digest = sha256(serialize(&Value::Array(records.clone())).as_bytes());
            if json!(digest) != copy["script_sha256"] || target.len() != records.len()
                || serialize(&Value::Array(different)) != serialize(&copy["different_members"])
            {
                return Err(format!("projector copy changed: {movie}"));
            }
            let handlers: usize = records
                .iter()
                .map(|s| s["bytecode"].as_str().unwrap_or("").split('\n').filter(|l| l.starts_with("on ")).count())
                .sum();
            let mut entry = copy.as_object().cloned().unwrap_or_default();
            entry.insert("scripts".into(), json!(records.len()));
            entry.insert("handlers".into(), json!(handlers));
            dispositions.push(Value::Object(entry));
        }
    }
    let mut manifest = json!({
        "schema_version": SCHEMA_VERSION, "stage": "source-accountability", "runtime_ir_frozen": false,
        "iso_sha256": policy["source_sha256"],
        "parser": {"name": "projectorrays", "version": "1.1.1", "wasm_sha256": parser_sha256},
        "summary": summary, "files": models, "supplemental_projector": supplemental,
        "projector_archive_dispositions": dispositions,
    });
    if policy.get("source_sha256").is_none() {
        manifest.as_object_mut().unwrap().remove("iso_sha256");
    }
    let mut all: Vec<&Value> = manifest["files"].as_array().unwrap().iter().collect();
    all.push(&manifest["supplemental_projector"]);
    let ledger = obligation_ledger(&all)?;
    fs.write(&files::join(output, "manifest.json"), serialize(&manifest).as_bytes())?;
    fs.write(&files::join(output, "obligations.json"), serialize(&ledger).as_bytes())?;
    fs.write(&files::join(output, "summary.json"), serialize(&summary).as_bytes())?;
    let _ = stringify;
    Ok(summary)
}

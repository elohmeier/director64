//! tools/projectorrays/recover.mjs: run structural score recovery over the
//! source audit's score resources.

use serde_json::{json, Value};

use super::files::{self, Files};
use super::js::stringify_pretty;
use super::score::decode;
use super::source::sha256;

const PIN: &str = include_str!("../../../tools/projectorrays/pin.json");
const DECODER: &str = include_str!("score.rs");

/// The pinned extension this decoder ports, and the decoder itself.
pub fn provenance() -> Value {
    let pin: Value = serde_json::from_str(PIN).expect("pin.json");
    json!({
        "interface": "director64-convert",
        "upstream_commit": pin["upstream_commit"],
        "extension_commit": pin["extension_commit"],
        "implementation": {"compiler/src/convert/score.rs": sha256(DECODER.as_bytes())},
        "ported_from": ["score_recovery.cpp", "score_recovery.h", "probe.cpp"],
        "format_reference": pin["format_reference"],
    })
}

pub fn recover(fs: &mut dyn Files, manifest_path: &str, output: &str, parser: &Value) -> Result<Value, String> {
    let manifest_bytes = fs.read(manifest_path)?;
    let source: Value = serde_json::from_slice(&manifest_bytes).map_err(|e| e.to_string())?;
    let source_dir = files::parent(manifest_path);
    fs.mkdir_all(&files::join(output, "chunks"))?;
    let mut summary = json!({"chunks": 0, "frames": 0, "labels": 0, "deltas": 0, "unindexed_tail_bytes": 0});
    let add = |summary: &mut Value, key: &str, n: u64| summary[key] = json!(summary[key].as_u64().unwrap() + n);
    let mut recovered_files = Vec::new();
    let mut all: Vec<&Value> = source["files"].as_array().ok_or("manifest without files")?.iter().collect();
    if !source["supplemental_projector"].is_null() {
        all.push(&source["supplemental_projector"]);
    }
    for file in all {
        let mut resources = Vec::new();
        for resource in file["score"].as_array().into_iter().flatten() {
            let blob = resource["blob"].as_str().unwrap_or("");
            let input = files::join(&source_dir, blob);
            if !input.starts_with(&format!("{source_dir}/")) {
                return Err("score blob escapes source directory".into());
            }
            let bytes = fs.read(&input)?;
            if resource["content_sha256"] != sha256(&bytes).as_str() || resource["length"] != bytes.len() {
                return Err(format!("score resource changed: {}", resource["id"].as_str().unwrap_or("")));
            }
            let fourcc = resource["fourcc"].as_str().unwrap_or("");
            let version = file["director_version"].as_u64().unwrap_or(0) as u32;
            let encoded = decode(fourcc, &bytes, version).map_err(|e| {
                format!("score recovery failed for {}/{fourcc}: {e}", file["name"].as_str().unwrap_or(""))
            })? + "\n";
            let model: Value = serde_json::from_str(&encoded).map_err(|e| e.to_string())?;
            let content = resource["content_sha256"].as_str().unwrap_or("");
            let blob = format!("chunks/{content}.json");
            fs.write(&files::join(output, &blob), encoded.as_bytes())?;
            resources.push(json!({
                "id": resource["id"], "fourcc": fourcc, "source_sha256": content, "blob": blob,
                "output_sha256": sha256(encoded.as_bytes()), "fields": model["fields"], "unresolved": model["unresolved"],
            }));
            let frames = model["frames"].as_array().unwrap();
            add(&mut summary, "chunks", 1);
            add(&mut summary, "frames", frames.len() as u64);
            add(&mut summary, "labels", model["labels"].as_array().unwrap().len() as u64);
            add(&mut summary, "deltas", frames.iter().map(|f| f["deltas"].as_array().unwrap().len() as u64).sum());
            add(&mut summary, "unindexed_tail_bytes", model["fields"]["unindexed_tail_length"].as_u64().unwrap_or(0));
        }
        recovered_files.push(json!({
            "id": file["id"], "name": file["name"],
            "source_path": if file["source_path"].is_null() { file["name"].clone() } else { file["source_path"].clone() },
            "director_version": file["director_version"], "resources": resources,
        }));
    }
    let mut out = json!({
        "schema_version": 1, "stage": "structural-score-spike", "runtime_ir_frozen": false,
        "semantic_complete": false, "reference_verified": false,
        "source_manifest_sha256": sha256(&manifest_bytes), "iso_sha256": source["iso_sha256"],
        "parser": parser, "summary": summary, "files": recovered_files,
    });
    if source.get("iso_sha256").is_none() {
        out.as_object_mut().unwrap().remove("iso_sha256");
    }
    fs.write(&files::join(output, "manifest.json"), (stringify_pretty(&out) + "\n").as_bytes())?;
    Ok(out["summary"].clone())
}

//! Which Director files ProjectorRays must parse for a game, and where each
//! parse lands. The plan names every media source and every embedded
//! projector movie the policy selects; tools/director/dump.mjs writes one
//! dump per entry, and the stages read them back by key.

use serde_json::{json, Value};

use super::dump::Dump;
use super::files::{self, Files};
use super::projector::projector_view;
use super::source::{sha256, source_files};

/// A media file's dump key, from its path relative to the media root, so a
/// plan does not depend on how that root was spelled.
pub fn file_key(relative: &str) -> String {
    format!("file:{}", files::normalize(relative))
}
/// An embedded projector movie's dump key (projector path relative to the media root).
pub fn view_key(relative: &str, offset: usize) -> String {
    format!("view:{}@{offset}", files::normalize(relative))
}
pub fn dump_path(dumps: &str, key: &str) -> String {
    files::join(dumps, &format!("{}.dump", &sha256(key.as_bytes())[..24]))
}

/// A policy's list of audited directories (default: the media root).
pub fn directories(policy: &Value) -> Vec<String> {
    policy["directories"]
        .as_array()
        .map(|d| d.iter().filter_map(|v| v.as_str().map(str::to_string)).collect())
        .unwrap_or_else(|| vec![".".to_string()])
}

/// Writes `<dumps>/plan.json` (and the projector views it needs).
pub fn write_plan(fs: &mut dyn Files, media: &str, policy: &Value, dumps: &str) -> Result<Value, String> {
    fs.mkdir_all(dumps)?;
    let mut entries = Vec::new();
    for source in source_files(fs, media, &directories(policy))? {
        let input = files::join(media, &source.path);
        let key = file_key(&source.path);
        entries.push(json!({"key": key, "input": input, "output": dump_path(dumps, &key)}));
    }
    if let Some(projector) = policy["projector"].as_str() {
        let path = files::join(media, projector);
        let bytes = fs.read(&path)?;
        let selection = policy.get("projector_selection").filter(|v| !v.is_null());
        let mut offsets = Vec::new();
        let main = projector_view(&bytes, selection)?;
        offsets.push(main.offset);
        for copy in policy["projector_copies"].as_array().into_iter().flatten() {
            offsets.push(copy["offset"].as_u64().ok_or("projector copy without offset")? as usize);
        }
        for offset in offsets {
            let view = projector_view(&bytes, Some(&json!({
                "offset": offset,
                "archives": main.archives,
            })))?;
            let key = view_key(projector, offset);
            let input = dump_path(dumps, &key).replace(".dump", ".view");
            fs.write(&input, &view.bytes)?;
            entries.push(json!({"key": key, "input": input, "output": dump_path(dumps, &key)}));
        }
    }
    let plan = json!({"dumps": entries});
    fs.write(&files::join(dumps, "plan.json"), (super::js::stringify_pretty(&plan) + "\n").as_bytes())?;
    Ok(plan)
}

pub fn read_dump(fs: &dyn Files, dumps: &str, key: &str) -> Result<Dump, String> {
    Dump::parse(&fs.read(&dump_path(dumps, key)).map_err(|e| format!("{key} was not parsed: {e}"))?)
}

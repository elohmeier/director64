//! One movie's member conversion (tools/director/convert-movie.mjs): every
//! cast member of a recovered Director file into the model's member records
//! and their content-addressed assets. The movie comes from its
//! ProjectorRays dump; property order and error text follow the reference,
//! because both reach model.json.

use std::collections::HashMap;

use serde_json::{json, Map, Value};

use super::bitmap::{bitmap_metadata, bitmap_pixels, bitmap_source_alpha, builtin_palette, decode_palette, score_model,
    verify_alfa_plane, windows_palette};
use super::d5media::{cast_strings, font_map, sound_resource, styled_text};
use super::dump::Dump;
use super::fdi::{fdi_image, FdiMeta};
use super::files::{self, Files};
use super::js;
use super::plan;
use super::projector::projector_view;
use super::source::{active_key_entries, sha256};
use super::swf::{flash_frames, vector_shape_pixels};
use super::xtra::{text_xtra_model, xtra_model};

type R<T> = Result<T, String>;

fn be16(b: &[u8], at: usize) -> R<i64> {
    b.get(at..at + 2).map(|s| u16::from_be_bytes([s[0], s[1]]) as i64).ok_or_else(|| out_of_range(at, 2, b.len()))
}
fn bei16(b: &[u8], at: usize) -> R<i64> {
    b.get(at..at + 2).map(|s| i16::from_be_bytes([s[0], s[1]]) as i64).ok_or_else(|| out_of_range(at, 2, b.len()))
}
fn be32(b: &[u8], at: usize) -> R<i64> {
    b.get(at..at + 4).map(|s| u32::from_be_bytes(s.try_into().unwrap()) as i64).ok_or_else(|| out_of_range(at, 4, b.len()))
}
/// Node's RangeError text for an out-of-bounds Buffer read.
fn out_of_range(at: usize, width: usize, length: usize) -> String {
    if length < width {
        "Attempt to access memory outside buffer bounds".into()
    } else {
        format!("The value of \"offset\" is out of range. It must be >= 0 and <= {}. Received {at}", length - width)
    }
}
fn int(v: &Value) -> i64 {
    v.as_i64().or_else(|| v.as_f64().map(|f| f as i64)).unwrap_or(0)
}
/// `ext.split(/[\\:]/).at(-1).toUpperCase().replace(/\.CST$/, ".CXT")`
fn cast_file_name(file_path: &str) -> String {
    let last = file_path.rsplit(['\\', ':']).next().unwrap_or("").to_uppercase();
    match last.strip_suffix(".CST") {
        Some(stem) => format!("{stem}.CXT"),
        None => last,
    }
}
fn basename(path: &str) -> &str {
    files::file_name(path.trim_end_matches('/'))
}

/// Content-addressed writes: an existing file of the same size is the same
/// content (asset-io.mjs).
pub fn write_asset(fs: &mut dyn Files, target: &str, data: &[u8]) -> R<()> {
    if fs.is_file(target) && fs.size(target)? == data.len() as u64 {
        return Ok(());
    }
    fs.write(target, data)
}

pub fn image_asset_name(data: &[u8]) -> String {
    format!("{}{}", &sha256(data)[..24], if &data[..4] == b"FDIA" { ".fda" } else { ".fdi" })
}

/// Where a game's Director files and their ProjectorRays dumps live.
pub struct Sources<'a> {
    pub media: &'a str,
    pub dumps: &'a str,
    pub source_paths: &'a Map<String, Value>,
    pub palettes: HashMap<String, Vec<u8>>,
}

impl Sources<'_> {
    pub fn dump(&self, fs: &dyn Files, relative: &str) -> R<Dump> {
        plan::read_dump(fs, self.dumps, &plan::file_key(relative))
    }

    /// A CLUT member of an external cast file (externalPalette).
    pub fn external_palette(&mut self, fs: &dyn Files, filename: &str, member: i64) -> R<Vec<u8>> {
        let locator = format!("{filename}:{member}");
        if let Some(bytes) = self.palettes.get(&locator) {
            return Ok(bytes.clone());
        }
        let valid = filename.strip_suffix(".CXT").is_some_and(|stem| {
            !stem.is_empty() && stem.bytes().all(|b| b.is_ascii_uppercase() || b.is_ascii_digit() || b == b'_')
        });
        if !valid {
            return Err("invalid external palette file".into());
        }
        let source_path = self.source_paths.get(filename).and_then(Value::as_str).unwrap_or(filename).to_string();
        let file = self.dump(fs, &source_path)?;
        let config = file.json.iter().find(|c| c["fourCC"] == "VWCF" || c["fourCC"] == "DRCF").map(|c| &c["data"])
            .ok_or("Cannot read properties of undefined (reading 'data')")?;
        let cast = &file.parsed("CAS*").ok_or("Cannot read properties of undefined (reading 'data')")?["memberIDs"];
        let key = active_key_entries(file.parsed("KEY*"))?;
        let cast_id = cast.get((member - int(&config["minMember"])) as usize).cloned().unwrap_or(Value::Null);
        let links: Vec<&Value> = key.iter().filter(|k| k["castID"] == cast_id && k["fourCC"] == "CLUT").collect();
        if links.len() != 1 {
            return Err("unresolved external palette member".into());
        }
        let bytes = file.chunk("CLUT", int(&links[0]["sectionID"])).ok_or("missing CLUT bytes")?.data.clone();
        self.palettes.insert(locator, bytes.clone());
        Ok(bytes)
    }
}

pub struct MovieJob<'a> {
    pub source: &'a Value,
    pub movie_id: usize,
    pub policy: &'a Value,
    pub recovery: &'a str,
    pub output: &'a str,
    pub common_palette: Option<&'a [u8]>,
    pub common_palette_hash: Option<&'a str>,
    pub d5_stage_palette: Option<&'a [u8]>,
}

pub struct FontRecord {
    pub hash: String,
    pub names: Vec<String>,
    pub source: Value,
}

pub struct Converted {
    pub movie: Value,
    pub problems: Vec<Value>,
    pub limits: Vec<Value>,
    pub font_records: Vec<FontRecord>,
}

impl Converted {
    /// As JSON, for the browser importer's pool, which converts movies on
    /// several workers and merges their results in movie order.
    pub fn to_json(&self) -> Value {
        json!({"movie": self.movie, "problems": self.problems, "limits": self.limits,
            "fontRecords": self.font_records.iter()
                .map(|r| json!({"hash": r.hash, "names": r.names, "source": r.source})).collect::<Vec<_>>()})
    }
    pub fn from_json(mut value: Value) -> R<Converted> {
        let list = |v: Value| -> R<Vec<Value>> {
            match v {
                Value::Array(items) => Ok(items),
                _ => Err("converted movie without a list".into()),
            }
        };
        let font_records = list(value["fontRecords"].take())?
            .into_iter()
            .map(|mut r| FontRecord {
                hash: r["hash"].as_str().unwrap_or("").to_string(),
                names: r["names"].as_array().into_iter().flatten().filter_map(|n| n.as_str().map(str::to_string)).collect(),
                source: r["source"].take(),
            })
            .collect();
        Ok(Converted {
            movie: value["movie"].take(),
            problems: list(value["problems"].take())?,
            limits: list(value["limits"].take())?,
            font_records,
        })
    }
}

fn obj(pairs: Vec<(&str, Value)>) -> Value {
    Value::Object(pairs.into_iter().map(|(k, v)| (k.to_string(), v)).collect())
}

pub fn convert_movie(fs: &mut dyn Files, sources: &mut Sources, job: &MovieJob) -> R<Converted> {
    let (source, policy) = (job.source, job.policy);
    let mut problems = Vec::new();
    let mut limits = Vec::new();
    let mut font_records = Vec::new();
    let source_name = source["name"].as_str().unwrap_or("").to_string();
    let projector_file = policy["projector"].as_str();
    let startup = policy["startup_movie"].as_str().filter(|s| !s.is_empty());
    let projector = projector_file.is_some_and(|p| source_name == basename(p).to_uppercase()) && startup.is_some();
    let movie_name = if projector { startup.unwrap().to_string() } else { source_name.clone() };
    let file = if projector {
        let path = files::join(sources.media, projector_file.unwrap());
        let bytes = fs.read(&path)?;
        let selection = policy.get("projector_selection").filter(|v| !v.is_null());
        let view = projector_view(&bytes, selection)?;
        plan::read_dump(fs, sources.dumps, &plan::view_key(projector_file.unwrap(), view.offset))?
    } else {
        let relative = source["source_path"].as_str().unwrap_or(&source_name).to_string();
        sources.dump(fs, &relative)?
    };
    let mut raw: HashMap<(String, i64), &[u8]> = HashMap::new();
    for chunk in &file.chunks {
        raw.insert((chunk.fourcc.clone(), chunk.id), &chunk.data);
    }
    let mut parsed: HashMap<(String, i64), &Value> = HashMap::new();
    for c in &file.json {
        parsed.insert((c["fourCC"].as_str().unwrap_or("").to_string(), int(&c["id"])), &c["data"]);
    }
    let key = active_key_entries(file.parsed("KEY*"))?;
    let director_version = [&source["director_version"], &policy["director_version"]]
        .iter()
        .find(|v| !v.is_null())
        .map(|v| int(v))
        .unwrap_or(600);
    let config = file.parsed(if director_version < 600 { "VWCF" } else { "DRCF" })
        .ok_or("Cannot read properties of undefined (reading 'data')")?;
    let castlist: Vec<Value> = match file.parsed("MCsL") {
        Some(m) => m["entries"].as_array().cloned().unwrap_or_default(),
        None => vec![json!({
            "name": if source_name.ends_with(".CXT") { "External" } else { "Internal" },
            "filePath": "", "minMember": config["minMember"], "maxMember": config["maxMember"], "id": 1024,
        })],
    };
    let mut movie = Map::new();
    movie.insert("name".into(), json!(movie_name));
    movie.insert("sourceName".into(), json!(source_name));
    movie.insert("directorVersion".into(), json!(director_version));
    movie.insert("id".into(), json!(job.movie_id));
    movie.insert("tempo".into(), config["frameRate"].clone());
    movie.insert("stageColor".into(),
        if director_version >= 700 { config["D7stageColorR"].clone() } else { config["preD7stageColor"].clone() });
    movie.insert("casts".into(), json!([]));
    movie.insert("members".into(), json!([]));
    if director_version >= 700 && config["D7stageColorIsRGB"].as_bool().unwrap_or_else(|| int(&config["D7stageColorIsRGB"]) != 0) {
        let rgb = (int(&config["D7stageColorR"]) << 16) | (int(&config["D7stageColorG"]) << 8) | int(&config["D7stageColorB"]);
        movie.insert("stageColorRGB".into(), json!(rgb as i32));
    }
    let aliases = &policy["external_cast_aliases"];
    let find_key = |cast_id: &Value, fourcc: &str| key.iter().find(|k| &k["castID"] == cast_id && k["fourCC"] == fourcc);
    let mut casts = Vec::new();
    let mut members = Vec::new();
    for (i, library) in castlist.iter().enumerate() {
        let file_path = library["filePath"].as_str().unwrap_or("");
        let link = if file_path.is_empty() { find_key(&library["id"], "CAS*") } else { None };
        let authored = cast_file_name(file_path);
        let external = aliases.get(&authored).and_then(Value::as_str).unwrap_or(&authored).to_string();
        let empty_internal = link.is_none() && file_path.is_empty() && int(&library["maxMember"]) < int(&library["minMember"]);
        let mut cast = Map::new();
        cast.insert("number".into(), json!(i + 1));
        cast.insert("name".into(), library["name"].clone());
        cast.insert("file".into(), json!(if link.is_some() || empty_internal { &movie_name } else { &external }));
        if empty_internal {
            cast.insert("empty".into(), json!(true));
        }
        if external != authored {
            cast.insert("authoredFile".into(), json!(authored));
        }
        casts.push(cast);
        let Some(link) = link else { continue };
        if let Some(fmap) = find_key(&library["id"], "Fmap") {
            let data = raw.get(&("Fmap".to_string(), int(&fmap["sectionID"]))).ok_or("missing Fmap bytes")?;
            casts[i].insert("fontMap".into(), Value::Object(font_map(data)?));
        }
        let cast_map = parsed.get(&("CAS*".to_string(), int(&link["sectionID"]))).ok_or("missing cast map")?;
        let member_ids = cast_map["memberIDs"].as_array().cloned().unwrap_or_default();
        for (n, id_value) in member_ids.iter().enumerate() {
            let id = int(id_value);
            if id == 0 {
                continue;
            }
            let info = parsed.get(&("CASt".to_string(), id)).copied();
            let bytes = raw.get(&("CASt".to_string(), id)).copied();
            let member = n as i64 + int(&library["minMember"]);
            let (Some(info), Some(bytes)) = (info, bytes) else { return Err("missing cast member".into()) };
            let mut entry = Map::new();
            entry.insert("number".into(), json!(member));
            entry.insert("cast".into(), json!(i + 1));
            entry.insert("name".into(), match info["info"].get("name") {
                Some(v) if !v.is_null() => v.clone(),
                _ => json!(""),
            });
            entry.insert("type".into(), info["type"].clone());
            let child = |cast_id: Option<i64>, fourcc: &str, required: bool| -> R<Option<&[u8]>> {
                let links: Vec<&Value> =
                    key.iter().filter(|k| cast_id.is_some_and(|c| k["castID"] == c) && k["fourCC"] == fourcc).collect();
                if links.is_empty() && !required {
                    return Ok(None);
                }
                if links.len() != 1 {
                    let shown = cast_id.map_or("undefined".to_string(), |c| c.to_string());
                    return Err(format!("ambiguous/missing {fourcc} child {shown}"));
                }
                let data = raw.get(&(fourcc.to_string(), int(&links[0]["sectionID"])))
                    .ok_or_else(|| format!("missing {fourcc} bytes"))?;
                Ok(Some(data))
            };
            let context = Member {
                i,
                id,
                member,
                bytes,
                info,
                source_name: &source_name,
                movie_name: &movie_name,
                director_version,
                castlist: &castlist,
                key: &key,
                raw: &raw,
                parsed: &parsed,
            };
            let result = convert_member(fs, sources, job, &context, &mut entry, &child, &mut problems, &mut limits, &mut font_records);
            if let Err(error) = result {
                problems.push(obj(vec![("movie", json!(source_name)), ("cast", json!(i + 1)), ("member", json!(member)), ("error", json!(error))]));
            }
            members.push(Value::Object(entry));
        }
    }
    movie.insert("casts".into(), Value::Array(casts.into_iter().map(Value::Object).collect()));
    movie.insert("members".into(), Value::Array(members));
    let shape_palette = file.chunks.iter().find(|c| c.fourcc == "CLUT").map(|c| c.data.clone());
    let palette_bytes = match shape_palette {
        Some(bytes) => bytes,
        None => match job.d5_stage_palette {
            Some(p) => p.to_vec(),
            None => windows_palette(8, false)?,
        },
    };
    let palette: Vec<Value> =
        decode_palette(&palette_bytes)?.iter().map(|[r, g, b]| json!((*r as u32) << 16 | (*g as u32) << 8 | *b as u32)).collect();
    if palette.len() != 256 {
        return Err("unsupported scene palette size".into());
    }
    movie.insert("palette".into(), Value::Array(palette));
    if let Some(external_cast) = policy["palette"]["external_cast"].as_str().filter(|_| policy["palette"].is_object()) {
        let uses = movie["casts"].as_array().unwrap().iter().any(|c| c["file"] == external_cast);
        if source_name != external_cast && uses {
            let first = movie["members"].as_array().unwrap().iter().any(|m| m["cast"] == 1 && m["number"] == 1);
            let map = &file.parsed("CAS*").ok_or("Cannot read properties of undefined (reading 'data')")?["memberIDs"];
            let link = find_key(&map[0], "CLUT");
            let same = link.and_then(|l| raw.get(&("CLUT".to_string(), int(&l["sectionID"]))))
                .is_some_and(|bytes| Some(sha256(bytes).as_str()) == job.common_palette_hash);
            if !first || link.is_none() || !same {
                return Err("external dialogue palette differs".into());
            }
        }
    }
    let mut recovered: HashMap<String, Value> = HashMap::new();
    for resource in source["resources"].as_array().into_iter().flatten() {
        let bytes = fs.read(&files::join(job.recovery, resource["blob"].as_str().unwrap_or("")))?;
        if resource["output_sha256"] != sha256(&bytes).as_str() {
            return Err("modified score evidence".into());
        }
        recovered.insert(
            resource["fourcc"].as_str().unwrap_or("").to_string(),
            serde_json::from_slice(&bytes).map_err(|e| e.to_string())?,
        );
    }
    if let Some(score) = recovered.get("VWSC") {
        movie.insert("score".into(), score_model(score, recovered.get("VWLB"))?);
    }
    for frame in movie.get("score").and_then(|s| s["frames"].as_array()).into_iter().flatten() {
        for channel in frame["channels"].as_array().into_iter().flatten() {
            for behavior in channel["behaviors"].as_array().into_iter().flatten() {
                if let Some(index) = behavior.get("missingInitializer").filter(|v| int(v) != 0) {
                    limits.push(obj(vec![
                        ("movie", json!(movie_name)), ("frame", frame["number"].clone()), ("channel", channel["channel"].clone()),
                        ("kind", json!("missing-behavior-initializer")), ("index", index.clone()), ("implemented", json!(false)),
                    ]));
                }
            }
        }
    }
    if director_version == 500 && job.d5_stage_palette.is_some() && !projector {
        if let Some(score) = movie.get("score") {
            for frame in score["frames"].as_array().into_iter().flatten() {
                for c in frame["channels"].as_array().into_iter().flatten() {
                    let changed = c["changed"].as_array().map(|a| a.iter().take(4).any(|v| int(v) != 0)).unwrap_or(false);
                    if c["channel"] != 5 || !changed {
                        continue;
                    }
                    let b: Vec<u8> = c["bytes"].as_array().unwrap().iter().map(|v| int(v) as u8).collect();
                    if be16(&b, 2)? == 0 {
                        continue;
                    }
                    if bei16(&b, 0)? == -1 && bei16(&b, 2)? == -1 {
                        limits.push(obj(vec![("movie", json!(movie_name)), ("frame", frame["number"].clone()),
                            ("kind", json!("builtin-palette-change")), ("implemented", json!(false))]));
                        continue;
                    }
                    let spec = &policy["d5_stage_palette"];
                    let second = movie["casts"].as_array().unwrap().get(1).map(|c| &c["file"]);
                    if be16(&b, 0)? != 2 || spec["member"] != be16(&b, 2)? || second != Some(&spec["file"]) {
                        return Err("D5 score changes the selected stage palette".into());
                    }
                }
            }
        }
    }
    Ok(Converted { movie: Value::Object(movie), problems, limits, font_records })
}

struct Member<'a> {
    i: usize,
    id: i64,
    member: i64,
    bytes: &'a [u8],
    info: &'a Value,
    source_name: &'a str,
    movie_name: &'a str,
    director_version: i64,
    castlist: &'a [Value],
    key: &'a [Value],
    raw: &'a HashMap<(String, i64), &'a [u8]>,
    parsed: &'a HashMap<(String, i64), &'a Value>,
}

type Child<'c> = dyn Fn(Option<i64>, &str, bool) -> R<Option<&'c [u8]>> + 'c;

#[allow(clippy::too_many_arguments)]
fn convert_member<'c>(
    fs: &mut dyn Files,
    sources: &mut Sources,
    job: &MovieJob,
    m: &Member<'c>,
    entry: &mut Map<String, Value>,
    child: &Child<'c>,
    problems: &mut Vec<Value>,
    limits: &mut Vec<Value>,
    font_records: &mut Vec<FontRecord>,
) -> R<()> {
    let (policy, output, id, member, bytes) = (job.policy, job.output, m.id, m.member, m.bytes);
    let cast_number = m.i as i64 + 1;
    let version = m.director_version;
    let set = |entry: &mut Map<String, Value>, k: &str, v: Value| {
        entry.insert(k.to_string(), v);
    };
    let limit_head = |kind: &str| -> Vec<(&str, Value)> {
        vec![("kind", json!(kind)), ("movie", json!(m.movie_name)), ("cast", json!(cast_number)), ("member", json!(member))]
    };
    let section = |fourcc: &str| m.key.iter().find(|k| k["castID"] == id && k["fourCC"] == fourcc).map(|k| k["sectionID"].clone());
    let specific = |bytes: &'c [u8]| -> R<&'c [u8]> {
        let start = 12 + be32(bytes, 4)? as usize;
        Ok(&bytes[start.min(bytes.len())..])
    };
    match int(&m.info["type"]) {
        1 => {
            let meta = bitmap_metadata(bytes, version)?;
            for (k, v) in meta.to_json() {
                entry.insert(k, v);
            }
            if meta.width == 0 && meta.height == 0 {
                set(entry, "empty", json!(true));
                if let Some(unused) = child(Some(id), "BITD", false)?.filter(|u| !u.is_empty()) {
                    set(entry, "emptyPayload", json!({"bytes": unused.len(), "sha256": sha256(unused)}));
                }
                return Ok(());
            }
            let mut palette: Option<Vec<u8>> = None;
            if meta.depth > 1 && meta.depth <= 8 {
                let overrides = policy["bitmap_palette_overrides"].as_array();
                let found = overrides.and_then(|all| all.iter().find(|p| {
                    p["movie"] == m.movie_name && p["cast"] == meta.palette_cast && p["member"] == meta.palette
                }));
                if let Some(o) = found {
                    let bytes = m.raw.get(&("CLUT".to_string(), int(&o["chunk"])));
                    if !bytes.is_some_and(|b| o["sha256"] == sha256(b).as_str()) {
                        return Err("palette override source changed".into());
                    }
                    palette = Some(bytes.unwrap().to_vec());
                    limits.push(obj(vec![
                        ("kind", json!("source-palette-reference-repair")), ("movie", json!(m.movie_name)),
                        ("cast", json!(cast_number)), ("member", json!(member)), ("sourceCast", json!(meta.palette_cast)),
                        ("sourceMember", json!(meta.palette)), ("sha256", o["sha256"].clone()),
                        ("original_projector_verified", json!(false)),
                    ]));
                } else if meta.depth == 2 || meta.palette <= 0 {
                    palette = Some(builtin_palette(meta.palette, meta.depth)?);
                } else if version == 500 && meta.palette_cast == 0 && meta.palette == 1 && job.d5_stage_palette.is_some() {
                    palette = job.d5_stage_palette.map(<[u8]>::to_vec);
                } else if policy["palette"]["external_cast"].as_str() == Some(m.source_name)
                    && meta.palette_cast == 0 && meta.palette == 1
                {
                    palette = job.common_palette.map(<[u8]>::to_vec);
                } else {
                    let pal_lib = if meta.palette_cast == -1 { m.i as i64 } else { (meta.palette_cast - 1).max(0) };
                    let library = m.castlist.get(pal_lib as usize);
                    let file_path = library.and_then(|l| l["filePath"].as_str()).unwrap_or("");
                    let pal_link = if file_path.is_empty() {
                        let lid = library.map(|l| l["id"].clone()).unwrap_or(Value::Null);
                        m.key.iter().find(|k| k["castID"] == lid && k["fourCC"] == "CAS*")
                    } else {
                        None
                    };
                    let pal_cast = pal_link.and_then(|l| m.parsed.get(&("CAS*".to_string(), int(&l["sectionID"]))));
                    if pal_cast.is_none() && !file_path.is_empty() {
                        let filename = cast_file_name(file_path);
                        let resolved = policy["external_cast_aliases"].get(&filename).and_then(Value::as_str)
                            .unwrap_or(&filename).to_string();
                        palette = Some(sources.external_palette(fs, &resolved, meta.palette)?);
                    } else {
                        let cast_id = match pal_cast {
                            None => None,
                            Some(pc) => {
                                let library = library.ok_or("Cannot read properties of undefined (reading 'minMember')")?;
                                let index = meta.palette - int(&library["minMember"]);
                                if index < 0 { None } else { pc["memberIDs"].get(index as usize).map(int) }
                            }
                        };
                        palette = child(cast_id, "CLUT", true)?.map(<[u8]>::to_vec);
                    }
                }
            }
            let bitd = child(Some(id), "BITD", true)?.unwrap();
            let pixels = bitmap_pixels(&meta, bitd, palette.as_deref(), true)?;
            let fdi_meta = FdiMeta {
                width: meta.width as usize,
                height: meta.height as usize,
                reg_x: meta.reg_x,
                reg_y: meta.reg_y,
                use_alpha: meta.use_alpha,
                alpha_threshold: meta.use_alpha.then_some(meta.alpha_threshold as u8),
                rgba32: false,
            };
            let data = fdi_image(&fdi_meta, &pixels)?;
            if meta.use_alpha {
                let alpha = bitmap_source_alpha(&meta, bitd)?;
                let alpha_hash = sha256(&alpha);
                let alpha_asset = format!("{alpha_hash}.a8");
                let alfa = child(Some(id), "ALFA", false)?;
                if let Some(alfa) = alfa {
                    verify_alfa_plane(&meta, &alpha, alfa)?;
                }
                write_asset(fs, &files::join(output, &format!("alpha/{alpha_asset}")), &alpha)?;
                let partial = alpha.iter().filter(|&&v| v > 0 && v < 255).count();
                let mut source = Map::new();
                for (k, v) in [
                    ("asset", json!(alpha_asset)), ("sha256", json!(alpha_hash)),
                    ("encoding", json!("row-major unsigned 8-bit alpha")), ("bytes", json!(alpha.len())),
                    ("file", json!(m.source_name)), ("castResource", json!(id)),
                    ("bitdResource", section("BITD").ok_or("Cannot read properties of undefined (reading 'sectionID')")?),
                ] {
                    source.insert(k.into(), v);
                }
                if alfa.is_some() {
                    source.insert("alfaVerified".into(), json!(true));
                }
                set(entry, "alphaSource", Value::Object(source));
                set(entry, "alphaEncoding", json!({"sourceBits": 8, "targetBits": if partial > 0 { 8 } else { 1 },
                    "partialPixels": partial, "lossless": true}));
            } else if let Some(alfa) = child(Some(id), "ALFA", false)? {
                set(entry, "unusedAlfa", json!({"bytes": alfa.len(), "sha256": sha256(alfa)}));
            }
            let asset = image_asset_name(&data);
            set(entry, "asset", json!(asset));
            write_asset(fs, &files::join(output, &format!("images/{asset}")), &data)?;
        }
        6 => {
            let (header, mut samples): (Vec<u8>, &[u8]);
            let d5_samples;
            if version == 500 {
                let sound = sound_resource(child(Some(id), "snd ", true)?.unwrap())?;
                let mut h = vec![0u8; 84];
                for (offset, value) in [(40, sound.frames), (44, sound.rate), (68, sound.bits), (76, sound.channels),
                    (20, sound.loop_start), (28, sound.loop_end)]
                {
                    h[offset..offset + 4].copy_from_slice(&value.to_be_bytes());
                }
                header = h;
                d5_samples = sound.samples;
                samples = &d5_samples;
            } else {
                let h = child(Some(id), "sndH", false)?;
                let s = child(Some(id), "sndS", false)?;
                if h.is_none() && s.is_none() {
                    set(entry, "empty", json!(true));
                    return Ok(());
                }
                let (Some(h), Some(s)) = (h, s) else { return Err("sound missing paired media".into()) };
                header = h.to_vec();
                samples = s;
            }
            if header.len() < 84 {
                return Err("truncated sound header".into());
            }
            let bits = be32(&header, 68)? as f64;
            let channels = be32(&header, 76)? as f64;
            let frames = be32(&header, 40)? as f64;
            let declared = frames * bits / 8.0 * channels;
            if version >= 1000 && samples.len() as f64 > declared {
                let pad = samples.len() as f64 - declared;
                let pad_bytes = pad as usize;
                if pad.fract() != 0.0 || samples[..pad_bytes].iter().any(|&v| v != 0) {
                    return Err("unsupported sound layout".into());
                }
                samples = &samples[pad_bytes..];
                set(entry, "trimmedZeroPrefixBytes", js::num(pad));
            }
            if declared != samples.len() as f64 || ![8.0, 16.0].contains(&bits) || ![1.0, 2.0].contains(&channels) {
                return Err("unsupported sound layout".into());
            }
            let rate = be32(&header, 44)?;
            set(entry, "rate", json!(rate));
            set(entry, "frames", json!(frames as i64));
            set(entry, "sourceBytes", json!(samples.len()));
            set(entry, "loopStart", json!(be32(&header, 20)?));
            set(entry, "loopEnd", json!(be32(&header, 28)?));
            let flags = match &m.info["info"] {
                Value::Null => return Err("Cannot read properties of null (reading 'flags')".into()),
                info => int(&info["flags"]),
            };
            set(entry, "looping", json!(i64::from(flags & 16 == 0)));
            set(entry, "channels", json!(channels as i64));
            if int(&entry["loopEnd"]) <= int(&entry["loopStart"]) {
                set(entry, "loopStart", json!(0));
                set(entry, "loopEnd", json!(frames as i64));
            }
            if int(&entry["loopEnd"]) as f64 > frames {
                return Err("sound loop exceeds sample frames".into());
            }
            if let Some(cues) = child(Some(id), "cupt", false)? {
                let count = if cues.len() >= 4 { be32(cues, 0)? as usize } else { 0 };
                if cues.len() != 4 + count * 36 {
                    return Err("unsupported cue point table".into());
                }
                let duration = if rate != 0 { frames * 1000.0 / rate as f64 } else { 0.0 };
                let mut points = Vec::new();
                for n in 0..count {
                    let at = 4 + n * 36;
                    let length = cues[at + 4] as usize;
                    let name = js::latin1(&cues[at + 5..at + 5 + length.min(31)]);
                    if length == 0 || length > 31 || name.chars().any(|c| (c as u32) < 0x20) {
                        if n + 1 != count {
                            return Err("unsupported cue point record".into());
                        }
                        continue;
                    }
                    let milliseconds = be32(cues, at)?;
                    if milliseconds as f64 > duration {
                        continue;
                    }
                    points.push(json!({"milliseconds": milliseconds, "name": name}));
                }
                set(entry, "cuePoints", Value::Array(points));
            }
            let (bits, channels) = (bits as usize, channels as usize);
            let mut wav = Vec::with_capacity(44 + samples.len());
            wav.extend_from_slice(b"RIFF");
            wav.extend_from_slice(&((36 + samples.len()) as u32).to_le_bytes());
            wav.extend_from_slice(b"WAVEfmt ");
            wav.extend_from_slice(&16u32.to_le_bytes());
            wav.extend_from_slice(&1u16.to_le_bytes());
            wav.extend_from_slice(&(channels as u16).to_le_bytes());
            wav.extend_from_slice(&(rate as u32).to_le_bytes());
            wav.extend_from_slice(&((rate as usize * bits / 8 * channels) as u32).to_le_bytes());
            wav.extend_from_slice(&((bits / 8 * channels) as u16).to_le_bytes());
            wav.extend_from_slice(&(bits as u16).to_le_bytes());
            wav.extend_from_slice(b"data");
            wav.extend_from_slice(&(samples.len() as u32).to_le_bytes());
            wav.extend_from_slice(samples);
            if bits == 16 {
                for pair in wav[44..].chunks_exact_mut(2) {
                    pair.swap(0, 1);
                }
            }
            let digest = sha256(&wav)[..24].to_string();
            set(entry, "asset", json!(format!("{digest}.wav64")));
            write_asset(fs, &files::join(output, &format!("wav/{digest}.wav")), &wav)?;
        }
        10 => {
            let strings = cast_strings(bytes)?;
            set(entry, "linkedDirectory", json!(strings.get(2).cloned().unwrap_or_default()));
            let linked = strings.get(3).cloned().unwrap_or_default();
            set(entry, "linkedFile", json!(linked));
            let b = specific(bytes)?;
            if b.len() != 12 || linked.is_empty() {
                return Err("unsupported digital video metadata".into());
            }
            let width = bei16(b, 6)? - bei16(b, 2)?;
            let height = bei16(b, 4)? - bei16(b, 0)?;
            set(entry, "width", json!(width));
            set(entry, "height", json!(height));
            set(entry, "regX", json!(width.div_euclid(2)));
            set(entry, "regY", json!(height.div_euclid(2)));
            set(entry, "videoFlags", json!(be32(b, 8)?));
            problems.push(obj(vec![("movie", json!(m.source_name)), ("cast", json!(cast_number)), ("member", json!(member)),
                ("error", json!("digital video requires target conversion")), ("file", json!(linked))]));
        }
        2 => {
            let b = specific(bytes)?;
            if b.len() != 14 {
                return Err("unsupported film-loop cast layout".into());
            }
            let (top, left) = (bei16(b, 0)?, bei16(b, 2)?);
            let (width, height) = (bei16(b, 6)? - left, bei16(b, 4)? - top);
            set(entry, "top", json!(top));
            set(entry, "left", json!(left));
            set(entry, "width", json!(width));
            set(entry, "height", json!(height));
            set(entry, "regX", json!(width.div_euclid(2)));
            set(entry, "regY", json!(height.div_euclid(2)));
            let flags = be32(b, 8)?;
            set(entry, "filmLoop", json!(i64::from(flags & 32 == 0)));
            set(entry, "filmFlags", json!(flags));
            let data = child(Some(id), "SCVW", true)?.unwrap();
            fs.write(&files::join(output, &format!("{}-{member}.scvw", m.source_name)), data)?;
            let decoded = super::score::decode("VWSC", data, version as u32)
                .map_err(|e| format!("Command failed: build/host/score-probe VWSC {version}\n{e}"))?;
            let score: Value = serde_json::from_str(&decoded).map_err(|e| e.to_string())?;
            set(entry, "filmScore", score_model(&score, None)?);
            set(entry, "filmSourceSha256", json!(sha256(data)));
        }
        8 => {
            let b = specific(bytes)?;
            if b.len() != 17 {
                return Err("unsupported shape layout".into());
            }
            set(entry, "shape", json!(be16(b, 0)?));
            set(entry, "width", json!(bei16(b, 8)? - bei16(b, 4)?));
            set(entry, "height", json!(bei16(b, 6)? - bei16(b, 2)?));
            set(entry, "regX", json!(0));
            set(entry, "regY", json!(0));
            set(entry, "pattern", json!(be16(b, 10)?));
            set(entry, "filled", json!(i64::from(b[14] != 0)));
            set(entry, "lineWidth", json!(b[15]));
            set(entry, "lineDirection", json!(b[16]));
            let found = policy["shape_pattern_overrides"].as_array().and_then(|all| {
                all.iter().find(|p| p["movie"] == m.movie_name && p["cast"] == cast_number && p["member"] == member)
            });
            if let Some(o) = found {
                if o["sha256"] != sha256(bytes).as_str() || o["from"] != entry["pattern"] {
                    return Err("shape override source changed".into());
                }
                let mut l = limit_head("shape-pattern-substitute");
                l.push(("sourcePattern", entry["pattern"].clone()));
                l.push(("targetPattern", o["to"].clone()));
                l.push(("original_projector_verified", json!(false)));
                limits.push(obj(l));
                set(entry, "pattern", o["to"].clone());
            }
            let shape = int(&entry["shape"]);
            if entry["pattern"] != 1 || !(1..=4).contains(&shape) {
                return Err("unsupported shape geometry/pattern".into());
            }
        }
        15 => {
            let xtra = xtra_model(bytes)?;
            let end = (xtra.payload_offset + xtra.payload_length).min(bytes.len());
            let payload = &bytes[xtra.payload_offset.min(end)..end];
            let mut model = Map::new();
            model.insert("symbol".into(), json!(xtra.symbol));
            model.insert("payload_offset".into(), json!(xtra.payload_offset));
            model.insert("payload_length".into(), json!(xtra.payload_length));
            model.insert("payloadHex".into(), json!(js::hex(payload)));
            set(entry, "xtra", Value::Object(model));
            let xmed = child(Some(id), "XMED", false)?;
            if let Some(x) = xmed {
                entry["xtra"].as_object_mut().unwrap().insert("mediaHex".into(), json!(js::hex(x)));
            }
            match xtra.symbol.as_str() {
                "text" => {
                    let xmed = xmed.ok_or("text Xtra has no XMED document")?;
                    for (k, v) in text_xtra_model(payload, xmed, version)? {
                        entry.insert(k, v);
                    }
                    set(entry, "sourceType", json!(15));
                    set(entry, "type", json!(3));
                }
                "font" => {
                    let xmed = xmed.filter(|x| x.starts_with(b"PFR1")).ok_or("unsupported embedded font format")?;
                    set(entry, "fontFormat", json!("PFR1"));
                    let names: Vec<String> = js::latin1(&payload[16.min(payload.len())..])
                        .split('\0').filter(|s| !s.is_empty()).map(str::to_string).collect();
                    set(entry, "fontNames", json!(names));
                    let hash = sha256(xmed);
                    write_asset(fs, &files::join(output, &format!("fonts/{hash}.pfr")), xmed)?;
                    set(entry, "embeddedFontHash", json!(hash));
                    let xmed_resource = section("XMED").ok_or("Cannot read properties of undefined (reading 'sectionID')")?;
                    font_records.push(FontRecord {
                        hash,
                        names,
                        source: json!({"movie": m.movie_name, "cast": cast_number, "member": member,
                            "castResource": id, "xmedResource": xmed_resource}),
                    });
                }
                "vectorShape" => {
                    let xmed = xmed.ok_or("vectorShape has no static SWF media")?;
                    let shape = vector_shape_pixels(xmed)?;
                    set(entry, "width", json!(shape.width));
                    set(entry, "height", json!(shape.height));
                    set(entry, "regX", json!(shape.reg_x));
                    set(entry, "regY", json!(shape.reg_y));
                    set(entry, "sourceType", json!(15));
                    set(entry, "type", json!(1));
                    let mut data = vec![0u8; 32];
                    data[..4].copy_from_slice(b"FDI1");
                    data[4..8].copy_from_slice(&((32 + shape.pixels.len()) as u32).to_be_bytes());
                    data[8..10].copy_from_slice(&(shape.width as u16).to_be_bytes());
                    data[10..12].copy_from_slice(&(shape.height as u16).to_be_bytes());
                    data[12..14].copy_from_slice(&(shape.reg_x as i16).to_be_bytes());
                    data[14..16].copy_from_slice(&(shape.reg_y as i16).to_be_bytes());
                    data.extend_from_slice(&shape.pixels);
                    let asset = format!("{}.fdi", &sha256(&data)[..24]);
                    set(entry, "asset", json!(asset));
                    write_asset(fs, &files::join(output, &format!("images/{asset}")), &data)?;
                }
                "flash" => {
                    let xmed = xmed.ok_or("flash member has no SWF media")?;
                    let off = xmed.windows(3).position(|w| w == b"FWS").ok_or("Xtra flash requires native conversion")?;
                    let flash = flash_frames(&xmed[off..])?;
                    let e = &flash.effects;
                    if e.filters != 0 || e.blend_modes != 0 || e.clip_actions != 0 {
                        let mut l = limit_head("flash-effects-ignored");
                        l.push(("filters", json!(e.filters)));
                        l.push(("blendModes", json!(e.blend_modes)));
                        l.push(("clipActions", json!(e.clip_actions)));
                        l.push(("implemented", json!(false)));
                        limits.push(obj(l));
                    }
                    set(entry, "sourceType", json!(15));
                    set(entry, "width", json!(flash.width));
                    set(entry, "height", json!(flash.height));
                    set(entry, "regX", json!(flash.reg_x));
                    set(entry, "regY", json!(flash.reg_y));
                    if !flash.labels.is_empty() || !flash.stop_frames.is_empty() {
                        set(entry, "flashTimeline", json!({"labels": flash.labels, "stopFrames": flash.stop_frames}));
                    }
                    if !flash.fields.is_empty() {
                        set(entry, "flashFields", json!(flash.fields));
                    }
                    if !flash.named_children.is_empty() {
                        set(entry, "flashNamed", json!(flash.named_children));
                    }
                    let truthy = |v: &Value| v.as_str().is_some_and(|s| !s.is_empty());
                    let unique = |key: &str| {
                        let mut seen: Vec<Value> = Vec::new();
                        for f in &flash.fields {
                            if truthy(&f[key]) && !seen.contains(&f[key]) {
                                seen.push(f[key].clone());
                            }
                        }
                        seen
                    };
                    if flash.fields.iter().any(|f| truthy(&f["variable"])) {
                        let mut l = limit_head("flash-dynamic-text");
                        l.push(("variables", json!(unique("variable"))));
                        l.push(("names", json!(unique("name"))));
                        l.push(("implemented", json!(true)));
                        l.push(("original_projector_verified", json!(false)));
                        limits.push(obj(l));
                    }
                    if flash.fields.iter().any(|f| !truthy(&f["variable"]) && truthy(&f["text"])) {
                        let mut l = limit_head("flash-static-text-layout");
                        l.push(("original_projector_verified", json!(false)));
                        limits.push(obj(l));
                    }
                    if !flash.ignored_actions.is_empty() {
                        let mut l = limit_head("flash-actions-ignored");
                        l.push(("actions", json!(flash.ignored_actions.len())));
                        l.push(("implemented", json!(false)));
                        limits.push(obj(l));
                    }
                    let meta = FdiMeta {
                        width: flash.width,
                        height: flash.height,
                        reg_x: flash.reg_x,
                        reg_y: flash.reg_y,
                        use_alpha: true,
                        alpha_threshold: None,
                        rgba32: false,
                    };
                    let mut assets = Vec::new();
                    for rgba in &flash.frames {
                        let data = fdi_image(&meta, rgba)?;
                        let asset = image_asset_name(&data);
                        write_asset(fs, &files::join(output, &format!("images/{asset}")), &data)?;
                        assets.push(asset);
                    }
                    set(entry, "asset", json!(assets[0]));
                    set(entry, "useAlpha", json!(true));
                    if flash.frames.len() == 1 {
                        set(entry, "type", json!(1));
                    } else {
                        set(entry, "type", json!(2));
                        set(entry, "filmLoop", json!(1));
                        set(entry, "filmAssets", json!(assets));
                        let mut l = limit_head("flash-timeline-flattened");
                        l.push(("frames", json!(flash.frames.len())));
                        l.push(("authoredRate", js::num(flash.rate)));
                        l.push(("original_projector_verified", json!(false)));
                        limits.push(obj(l));
                    }
                }
                other => return Err(format!("Xtra {other} requires native conversion")),
            }
        }
        3 | 7 => {
            let text = child(Some(id), "STXT", false)?;
            let extended = policy["extended_d6"].as_bool().unwrap_or(false);
            if let Some(text) = text.filter(|_| version == 500 || extended) {
                for (k, v) in styled_text(text)? {
                    entry.insert(k, v);
                }
            } else if let Some(text) = text.filter(|t| t.len() >= 12) {
                let length = be32(text, 4)? as usize;
                if length > text.len() - 12 {
                    return Err("field text exceeds chunk".into());
                }
                set(entry, "text", json!(js::latin1(&text[12..12 + length])));
            }
            if extended {
                let start = 12 + be32(bytes, 4)? as usize;
                let end = (start + be32(bytes, 8)? as usize).min(bytes.len());
                let b = &bytes[start.min(end)..end];
                if b.len() < 28 {
                    return Err("truncated D6 text cast metadata".into());
                }
                set(entry, "width", json!(bei16(b, 20)? - bei16(b, 16)?));
                set(entry, "height", json!(bei16(b, 18)? - bei16(b, 14)?));
                set(entry, "regX", json!(0));
                set(entry, "regY", json!(0));
                set(entry, "editable", json!(b[25] & 1 != 0));
                set(entry, "textAlign", json!(bei16(b, 4)?));
            }
        }
        _ => {}
    }
    Ok(())
}

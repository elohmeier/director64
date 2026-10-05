//! The conversion stage (tools/director/compile-director.mjs): every
//! recovered movie's members, film loops and scores into model.json and its
//! content-addressed assets. Movies convert in recovery order, film loops
//! flatten and the stage prescale resamples afterwards, exactly as the
//! reference merged its parallel results.

use std::collections::{BTreeSet, HashMap};

use serde_json::{json, Map, Value};

use super::fdi::prescale_image;
use super::files::{self, Files};
use super::film::compose_film_frame;
use super::js;
use super::movie::{convert_movie, image_asset_name, write_asset, Converted, MovieJob, Sources};
use super::source::sha256;
use super::text::{bind_text_fonts, text_reachability};

type R<T> = Result<T, String>;

pub struct Options<'a> {
    pub output: &'a str,
    pub recovery: &'a str,
    pub media: &'a str,
    pub dumps: &'a str,
    pub policy: &'a Value,
    pub defer_video: bool,
}

pub struct Compiled {
    pub movies: usize,
    pub problems: Vec<Value>,
    pub fatal: Vec<Value>,
}

/// The stage between converting the movies and the stage prescale: the
/// browser importer resamples the prescale queue on its worker pool, then
/// finishes with the renames (compile_finish).
pub struct Pending {
    movies: Vec<Value>,
    problems: Vec<Value>,
    limits: Vec<Value>,
    fonts: Vec<Value>,
    /// The images the stage prescale resamples, in first-use order; empty
    /// when the policy has no prescale.
    pub prescale: Vec<String>,
}

fn int(v: &Value) -> i64 {
    v.as_i64().or_else(|| v.as_f64().map(|f| f as i64)).unwrap_or(0)
}
fn truthy(v: &Value) -> bool {
    match v {
        Value::Null => false,
        Value::Bool(b) => *b,
        Value::Number(n) => n.as_f64().is_some_and(|f| f != 0.0),
        Value::String(s) => !s.is_empty(),
        _ => true,
    }
}
fn basename(path: &str) -> &str {
    files::file_name(path.trim_end_matches('/'))
}

pub fn compile(fs: &mut dyn Files, options: &Options) -> R<Compiled> {
    let pending = compile_start(fs, options)?;
    let mut rescaled = HashMap::new();
    for name in &pending.prescale {
        let data = fs.read(&files::join(options.output, &format!("images/{name}")))?;
        let out = match prescale_asset(&data)? {
            None => name.clone(),
            Some((out, scaled)) => {
                write_asset(fs, &files::join(options.output, &format!("images/{out}")), &scaled)?;
                out
            }
        };
        rescaled.insert(name.clone(), out);
    }
    compile_finish(fs, options, pending, &rescaled)
}

/// What every movie's conversion shares: the recovered files, their source
/// paths and the policy's palettes, and which files convert as movies.
pub struct Setup {
    recovered_files: Vec<Value>,
    source_paths: Map<String, Value>,
    common_palette: Option<Vec<u8>>,
    common_palette_hash: Option<String>,
    d5_stage_palette: Option<Vec<u8>>,
    /// The movies' indices into the recovered files, in conversion order.
    pub jobs: Vec<usize>,
}

impl Setup {
    /// Each job's recovered file name, in job order.
    pub fn job_names(&self) -> Vec<String> {
        self.jobs.iter().map(|&i| self.recovered_files[i]["name"].as_str().unwrap_or("").to_string()).collect()
    }

    /// The files job `k` is known to read: the recovery manifest, its movie's
    /// dump and its recovered score resources. A worker that converts it
    /// elsewhere is sent these with the job.
    pub fn job_inputs(&self, options: &Options, k: usize) -> Vec<String> {
        let source = &self.recovered_files[self.jobs[k]];
        let name = source["name"].as_str().unwrap_or("");
        let relative = self.source_paths.get(name).and_then(Value::as_str).unwrap_or(name);
        let mut inputs = vec![
            files::join(options.recovery, "manifest.json"),
            super::plan::dump_path(options.dumps, &super::plan::file_key(relative)),
        ];
        for resource in source["resources"].as_array().into_iter().flatten() {
            inputs.push(files::join(options.recovery, resource["blob"].as_str().unwrap_or("")));
        }
        inputs
    }
}

pub fn compile_setup(fs: &mut dyn Files, options: &Options) -> R<Setup> {
    let (output, policy) = (options.output, options.policy);
    let recovery: Value =
        serde_json::from_slice(&fs.read(&files::join(options.recovery, "manifest.json"))?).map_err(|e| e.to_string())?;
    for directory in ["images", "wav", "alpha", "fonts"] {
        fs.mkdir_all(&files::join(output, directory))?;
    }
    let recovered_files = recovery["files"].as_array().ok_or("recovery manifest without files")?.clone();
    let source_paths: Map<String, Value> = recovered_files
        .iter()
        .map(|f| {
            let path = if f["source_path"].is_null() { f["name"].clone() } else { f["source_path"].clone() };
            (f["name"].as_str().unwrap_or("").to_string(), path)
        })
        .collect();
    let mut sources = Sources { media: options.media, dumps: options.dumps, source_paths: &source_paths, palettes: HashMap::new() };
    let (mut common_palette, mut common_palette_hash) = (None, None);
    if policy["palette"].is_object() {
        let spec = &policy["palette"];
        let file = sources.dump(fs, spec["file"].as_str().unwrap_or(""))?;
        let chunk = file.chunk("CLUT", int(&spec["chunk"])).ok_or("Cannot read properties of undefined (reading 'data')")?;
        common_palette = Some(chunk.data.clone());
        common_palette_hash = spec["sha256"].as_str().map(str::to_string);
        if common_palette_hash.as_deref() != Some(sha256(&chunk.data).as_str()) {
            return Err("unsupported dialogue palette".into());
        }
    }
    let mut d5_stage_palette = None;
    if policy["d5_stage_palette"].is_object() {
        let spec = &policy["d5_stage_palette"];
        let bytes = sources.external_palette(fs, spec["file"].as_str().unwrap_or(""), int(&spec["member"]))?;
        if spec["sha256"] != sha256(&bytes).as_str() {
            return Err("D5 stage palette changed".into());
        }
        d5_stage_palette = Some(bytes);
    }
    let projector_name = policy["projector"].as_str().map(|p| basename(p).to_uppercase());
    let has_startup = policy["startup_movie"].as_str().is_some_and(|s| !s.is_empty());
    let jobs = recovered_files
        .iter()
        .enumerate()
        .filter(|(_, source)| {
            let name = source["name"].as_str().unwrap_or("");
            let projector = projector_name.as_deref() == Some(name) && has_startup;
            name.ends_with(".DXR") || name.ends_with(".CXT") || projector
        })
        .map(|(i, _)| i)
        .collect();
    Ok(Setup { recovered_files, source_paths, common_palette, common_palette_hash, d5_stage_palette, jobs })
}

/// The movie of job `k` (its movie id is k + 1). `palettes` caches external
/// palettes across calls; it changes no result.
pub fn convert_job(
    fs: &mut dyn Files,
    options: &Options,
    setup: &Setup,
    palettes: &mut HashMap<String, Vec<u8>>,
    k: usize,
) -> R<Converted> {
    let mut sources = Sources {
        media: options.media,
        dumps: options.dumps,
        source_paths: &setup.source_paths,
        palettes: std::mem::take(palettes),
    };
    let job = MovieJob {
        source: &setup.recovered_files[setup.jobs[k]],
        movie_id: k + 1,
        policy: options.policy,
        recovery: options.recovery,
        output: options.output,
        common_palette: setup.common_palette.as_deref(),
        common_palette_hash: setup.common_palette_hash.as_deref(),
        d5_stage_palette: setup.d5_stage_palette.as_deref(),
    };
    let result = convert_movie(fs, &mut sources, &job);
    *palettes = sources.palettes;
    result
}

/// Converts every movie and flattens the film loops; the stage prescale's
/// queue is left to the caller.
pub fn compile_start(fs: &mut dyn Files, options: &Options) -> R<Pending> {
    let setup = compile_setup(fs, options)?;
    let mut palettes = HashMap::new();
    let mut converted = Vec::with_capacity(setup.jobs.len());
    for k in 0..setup.jobs.len() {
        converted.push(convert_job(fs, options, &setup, &mut palettes, k)?);
    }
    compile_merge(fs, options, converted)
}

/// The movies' results, in job order, into one model: embedded fonts, film
/// loops and the prescale queue.
pub fn compile_merge(fs: &mut dyn Files, options: &Options, converted: Vec<Converted>) -> R<Pending> {
    let (output, policy) = (options.output, options.policy);
    let mut movies: Vec<Value> = Vec::new();
    let mut problems: Vec<Value> = Vec::new();
    let mut limits: Vec<Value> = Vec::new();
    let mut font_records = Vec::new();
    for result in converted {
        movies.push(result.movie);
        problems.extend(result.problems);
        limits.extend(result.limits);
        font_records.extend(result.font_records);
    }
    // Embedded fonts recover once per source hash, numbered in first-occurrence order.
    let mut fonts: Vec<Value> = Vec::new();
    let mut font_index: HashMap<String, usize> = HashMap::new();
    for record in font_records {
        let index = match font_index.get(&record.hash) {
            Some(&index) => index,
            None => {
                let pfr = files::join(output, &format!("fonts/{}.pfr", record.hash));
                let mut font = super::font::recover(fs, &pfr, &files::join(output, "fonts"), &record.names)?;
                let object = font.as_object_mut().ok_or("font recovery returned no record")?;
                object.insert("number".into(), json!(fonts.len() + 1));
                object.insert("sources".into(), json!([]));
                fonts.push(font);
                font_index.insert(record.hash.clone(), fonts.len() - 1);
                fonts.len() - 1
            }
        };
        let font = fonts[index].as_object_mut().unwrap();
        let mut aliases: Vec<String> =
            font["aliases"].as_array().into_iter().flatten().filter_map(|v| v.as_str().map(str::to_string)).collect();
        for name in &record.names {
            if !aliases.contains(name) {
                aliases.push(name.clone());
            }
        }
        aliases.sort_by(|a, b| a.encode_utf16().cmp(b.encode_utf16()));
        font.insert("aliases".into(), json!(aliases));
        font["sources"].as_array_mut().unwrap().push(record.source);
    }
    for movie in &mut movies {
        for entry in movie["members"].as_array_mut().unwrap() {
            let entry = entry.as_object_mut().unwrap();
            let Some(hash) = entry.get("embeddedFontHash").and_then(Value::as_str).map(str::to_string) else { continue };
            entry.insert("embeddedFont".into(), fonts[font_index[&hash]]["number"].clone());
            entry.shift_remove("embeddedFontHash");
        }
    }
    let converted: BTreeSet<String> = movies.iter().map(|m| m["name"].as_str().unwrap_or("").to_string()).collect();
    for movie in &movies {
        for library in movie["casts"].as_array().unwrap() {
            let file = library["file"].as_str().unwrap_or("");
            if !converted.contains(file) {
                problems.push(json!({"movie": movie["name"], "cast": library["number"],
                    "error": format!("unresolved external cast {}", js_text(library.get("file")))}));
            }
        }
    }
    flatten_films(fs, output, &mut movies, &mut problems, &mut limits)?;
    let prescale = if truthy(&policy["stage_prescale"]) { prescale_queue(&movies) } else { Vec::new() };
    Ok(Pending { movies, problems, limits, fonts, prescale })
}

/// The rest of the stage, given each prescaled image's new name.
pub fn compile_finish(fs: &mut dyn Files, options: &Options, pending: Pending, rescaled: &HashMap<String, String>) -> R<Compiled> {
    let (output, policy) = (options.output, options.policy);
    let Pending { mut movies, problems, mut limits, mut fonts, prescale } = pending;
    if truthy(&policy["stage_prescale"]) {
        if let Some(name) = prescale.iter().find(|n| !rescaled.contains_key(*n)) {
            return Err(format!("prescale result missing for {name}"));
        }
        apply_prescale(&mut movies, rescaled, &mut limits);
    }
    // Policy-pinned deferrals turn documented residual conversions into
    // explicit approximations; a pin without its problem is stale.
    let deferrals = policy["deferred_conversions"].as_array().cloned().unwrap_or_default();
    let mut used = vec![false; deferrals.len()];
    let mut remaining = Vec::new();
    for problem in problems {
        let error = problem["error"].as_str().unwrap_or("");
        let pin = deferrals.iter().enumerate().position(|(n, d)| {
            !used[n]
                && ["movie", "cast", "member"].iter().all(|k| d.get(*k) == problem.get(*k))
                && d["error"].as_str().is_some_and(|prefix| error.starts_with(prefix))
        });
        match pin {
            Some(n) => {
                used[n] = true;
                let mut limit = Map::new();
                limit.insert("kind".into(), json!("deferred-conversion"));
                for (k, v) in problem.as_object().unwrap() {
                    limit.insert(k.clone(), v.clone());
                }
                limit.insert("disposition".into(), deferrals[n]["disposition"].clone());
                limit.insert("implemented".into(), json!(false));
                limits.push(Value::Object(limit));
            }
            None => remaining.push(problem),
        }
    }
    if let Some(n) = used.iter().position(|u| !u) {
        let d = &deferrals[n];
        return Err(format!("stale deferred conversion pin: {} {}:{}", js_text(d.get("movie")), js_text(d.get("cast")), js_text(d.get("member"))));
    }
    let problems = remaining;
    limits.extend(bind_text_fonts(&mut movies, &mut fonts, truthy(&policy["field_text_styles"]))?);
    // Flash edit fields render through the same recovered fonts; their sizes join the variants.
    {
        let mut aliases: HashMap<String, usize> = HashMap::new();
        for (index, f) in fonts.iter().enumerate() {
            for alias in f["aliases"].as_array().into_iter().flatten() {
                aliases.insert(alias.as_str().unwrap_or("").to_lowercase(), index);
            }
        }
        for movie in &movies {
            for m in movie["members"].as_array().unwrap() {
                for field in m["flashFields"].as_array().into_iter().flatten() {
                    let Some(name) = field["fontName"].as_str().filter(|n| !n.is_empty()) else { continue };
                    let Some(&index) = aliases.get(&name.to_lowercase()) else { continue };
                    let size = js::round(field["fontHeight"].as_f64().unwrap_or(f64::NAN));
                    let f = &mut fonts[index];
                    let number = f["number"].clone();
                    let variants = f["variants"].as_array_mut().ok_or("font without variants")?;
                    if (1.0..=255.0).contains(&size) && !variants.iter().any(|v| v["size"].as_f64() == Some(size)) {
                        variants.push(json!({"size": js::num(size), "asset": format!("fonts/f{number}-{}.font64", js::number(size))}));
                    }
                }
            }
        }
        for f in &mut fonts {
            sort_variants(f);
        }
    }
    if truthy(&policy["stage_prescale"]) {
        for f in &mut fonts {
            let number = f["number"].clone();
            let variants = f["variants"].as_array_mut().ok_or("font without variants")?;
            let mut sizes: BTreeSet<i64> = variants.iter().map(|v| int(&v["size"])).collect();
            for v in variants.clone() {
                let scaled = ((int(&v["size"]) * 4 + 2) as f64 / 5.0).floor().max(1.0) as i64;
                if sizes.insert(scaled) {
                    variants.push(json!({"size": scaled, "asset": format!("fonts/f{number}-{scaled}.font64")}));
                }
            }
            sort_variants(f);
        }
    }
    let script_root = files::join(&files::parent(options.recovery), "lingo");
    let mut scripts = Vec::new();
    if fs.exists(&script_root) {
        let mut names: Vec<String> = fs.list(&script_root)?.into_iter().map(|(name, _)| name).collect();
        names.sort_by(|a, b| a.as_bytes().cmp(b.as_bytes()));
        for name in names.into_iter().filter(|n| n.ends_with(".lingo")) {
            let bytes = fs.read(&files::join(&script_root, &name))?;
            scripts.push((name, String::from_utf8_lossy(&bytes).into_owned()));
        }
    }
    let audit = text_reachability(&movies, &scripts);
    fs.write(&files::join(output, "font-audit.json"), (js::stringify_pretty(&audit) + "\n").as_bytes())?;
    let count = movies.len();
    let model = json!({
        "version": 1, "extendedD6": truthy(&policy["extended_d6"]), "movies": movies, "fonts": fonts,
        "problems": problems, "approximations": limits,
    });
    fs.write(&files::join(output, "model.json"), (js::stringify(&model) + "\n").as_bytes())?;
    let fatal: Vec<Value> = problems
        .iter()
        .filter(|p| !(options.defer_video && p["error"] == "digital video requires target conversion"))
        .cloned()
        .collect();
    Ok(Compiled { movies: count, problems, fatal })
}

/// A value as JS template-literal text; a missing key is `undefined`.
fn js_text(v: Option<&Value>) -> String {
    match v {
        None => "undefined".into(),
        Some(Value::String(s)) => s.clone(),
        Some(Value::Null) => "null".into(),
        Some(Value::Number(n)) => js::number(n.as_f64().unwrap_or(0.0)),
        Some(Value::Bool(b)) => b.to_string(),
        Some(other) => js::stringify(other),
    }
}

fn sort_variants(font: &mut Value) {
    if let Some(variants) = font["variants"].as_array_mut() {
        variants.sort_by(|a, b| {
            let (a, b) = (a["size"].as_f64().unwrap_or(0.0), b["size"].as_f64().unwrap_or(0.0));
            a.partial_cmp(&b).unwrap_or(std::cmp::Ordering::Equal)
        });
    }
}

/// Flattens the bitmap-only source film loops to native frame sequences.
fn flatten_films(fs: &mut dyn Files, output: &str, movies: &mut [Value], problems: &mut Vec<Value>, limits: &mut Vec<Value>) -> R<()> {
    let snapshot: Vec<Value> = movies.to_vec();
    for (index, movie) in snapshot.iter().enumerate() {
        let loops: Vec<&Value> =
            movie["members"].as_array().unwrap().iter().filter(|m| m.get("filmScore").is_some_and(truthy)).collect();
        for loop_member in loops {
            let (cast, number) = (loop_member["cast"].clone(), loop_member["number"].clone());
            let mut loop_limits = Vec::new();
            let mut film_assets: Option<Vec<Value>> = None;
            let mut film_sounds: Option<Vec<Value>> = None;
            let mut use_alpha = false;
            let (width, height) = (int(&loop_member["width"]), int(&loop_member["height"]));
            let frames = loop_member["filmScore"]["frames"].as_array().cloned().unwrap_or_default();
            let target = movies[index]["members"]
                .as_array_mut()
                .unwrap()
                .iter_mut()
                .find(|m| m["cast"] == cast && m["number"] == number)
                .unwrap();
            if width == 0 && height == 0 && frames.is_empty() {
                let target = target.as_object_mut().unwrap();
                target.insert("empty".into(), json!(true));
                target.insert("filmAssets".into(), json!([]));
                continue;
            }
            let result = (|| -> R<()> {
                if width < 1 || height < 1 || width > 1024 || height > 1024 {
                    return Err("film-loop bounds".into());
                }
                let mut states: Vec<(i64, Value)> = Vec::new();
                film_assets = Some(Vec::new());
                film_sounds = Some(Vec::new());
                use_alpha = true;
                for frame in &frames {
                    for c in frame["channels"].as_array().into_iter().flatten() {
                        let channel = int(&c["channel"]);
                        match states.iter_mut().find(|s| s.0 == channel) {
                            Some(slot) => slot.1 = c.clone(),
                            None => states.push((channel, c.clone())),
                        }
                    }
                    let composed = {
                        let fs_ref: &dyn Files = fs;
                        let mut load = |asset: &str| fs_ref.read(&files::join(output, &format!("images/{asset}")));
                        compose_film_frame(movie, loop_member, &states, &mut load, &snapshot, &mut loop_limits)?
                    };
                    let asset = image_asset_name(&composed.data);
                    write_asset(fs, &files::join(output, &format!("images/{asset}")), &composed.data)?;
                    film_assets.as_mut().unwrap().push(json!(asset));
                    film_sounds.as_mut().unwrap().push(composed.sounds);
                }
                Ok(())
            })();
            let target = target.as_object_mut().unwrap();
            if let Some(assets) = &film_assets {
                target.insert("filmAssets".into(), json!(assets));
            }
            if let Some(sounds) = &film_sounds {
                target.insert("filmSounds".into(), json!(sounds));
            }
            if use_alpha {
                target.insert("useAlpha".into(), json!(true));
            }
            limits.extend(loop_limits);
            if let Err(error) = result {
                problems.push(json!({"movie": movie["name"], "cast": cast, "member": number, "error": error}));
                continue;
            }
            let sounds = film_sounds.unwrap_or_default();
            if int(&loop_member["filmScore"]["version"]) != 7
                && sounds.iter().any(|f| f.as_array().is_some_and(|pair| pair.iter().any(truthy)))
            {
                limits.push(json!({"kind": "film-loop-sounds-pending-native", "movie": movie["name"],
                    "cast": cast, "member": number, "implemented": false}));
            }
            let first = film_assets.unwrap_or_default().first().cloned().unwrap_or(Value::Null);
            target.insert("asset".into(), first);
        }
    }
    Ok(())
}

/// The finished image assets the 4/5 prescale of an 800x600 stage
/// resamples, in first-use order.
fn prescale_queue(movies: &[Value]) -> Vec<String> {
    let mut queue: Vec<String> = Vec::new();
    let mut queued = BTreeSet::new();
    let mut enqueue = |name: &Value| {
        let Some(name) = name.as_str() else { return };
        if !(name.ends_with(".fdi") || name.ends_with(".fda") || name.ends_with(".fd2")) || !queued.insert(name.to_string()) {
            return;
        }
        queue.push(name.to_string());
    };
    for movie in movies {
        for m in movie["members"].as_array().unwrap() {
            enqueue(&m["asset"]);
            for name in m["filmAssets"].as_array().into_iter().flatten() {
                enqueue(name);
            }
        }
    }
    queue
}

/// One image asset at 4/5: its content-addressed name and bytes, or None
/// when it keeps its size.
pub fn prescale_asset(data: &[u8]) -> R<Option<(String, Vec<u8>)>> {
    Ok(prescale_image(data)?.map(|scaled| {
        let kind = &scaled[..4];
        let extension = if kind == b"FDIA" { ".fda" } else if kind == b"FDI2" { ".fd2" } else { ".fdi" };
        (format!("{}{extension}", &sha256(&scaled)[..24]), scaled)
    }))
}

fn apply_prescale(movies: &mut [Value], rescaled: &HashMap<String, String>, limits: &mut Vec<Value>) {
    for movie in movies.iter_mut() {
        for m in movie["members"].as_array_mut().unwrap() {
            let m = m.as_object_mut().unwrap();
            if let Some(out) = m.get("asset").and_then(Value::as_str).and_then(|a| rescaled.get(a)) {
                m.insert("asset".into(), json!(out));
            }
            if let Some(assets) = m.get_mut("filmAssets").and_then(Value::as_array_mut) {
                for asset in assets {
                    if let Some(out) = asset.as_str().and_then(|a| rescaled.get(a)) {
                        *asset = json!(out);
                    }
                }
            }
        }
    }
    limits.push(json!({"kind": "stage-prescale", "scale": "4/5", "skipped": "<=16x16", "resampled": rescaled.len(),
        "implemented": true, "original_projector_verified": false}));
}

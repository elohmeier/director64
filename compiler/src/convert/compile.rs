//! The conversion stage: every recovered movie's members, film loops and
//! scores into model.json and its content-addressed assets. Movies convert in
//! recovery order, then film loops flatten.

use std::collections::{BTreeSet, HashMap};

use serde_json::{Map, Value, json};

use super::files::{self, Files};
use super::film::{NESTED_PENDING, compose_film_frame, needs_runtime};
use super::js;
use super::movie::{Converted, MovieJob, Sources, convert_movie, image_asset_name, write_asset};
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

/// The converted movies before the model is finished (compile_finish).
pub struct Pending {
    movies: Vec<Value>,
    problems: Vec<Value>,
    limits: Vec<Value>,
    fonts: Vec<Value>,
}

fn int(v: &Value) -> i64 {
    v.as_i64()
        .or_else(|| v.as_f64().map(|f| f as i64))
        .unwrap_or(0)
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
    compile_finish(fs, options, pending)
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

pub fn compile_setup(fs: &mut dyn Files, options: &Options) -> R<Setup> {
    let (output, policy) = (options.output, options.policy);
    let recovery: Value =
        serde_json::from_slice(&fs.read(&files::join(options.recovery, "manifest.json"))?)
            .map_err(|e| e.to_string())?;
    for directory in ["images", "wav", "alpha", "fonts"] {
        fs.mkdir_all(&files::join(output, directory))?;
    }
    let recovered_files = recovery["files"]
        .as_array()
        .ok_or("recovery manifest without files")?
        .clone();
    let source_paths: Map<String, Value> = recovered_files
        .iter()
        .map(|f| {
            let path = if f["source_path"].is_null() {
                f["name"].clone()
            } else {
                f["source_path"].clone()
            };
            (f["name"].as_str().unwrap_or("").to_string(), path)
        })
        .collect();
    let mut sources = Sources {
        media: options.media,
        dumps: options.dumps,
        source_paths: &source_paths,
        palettes: HashMap::new(),
    };
    let (mut common_palette, mut common_palette_hash) = (None, None);
    if policy["palette"].is_object() {
        let spec = &policy["palette"];
        let file = sources.dump(fs, spec["file"].as_str().unwrap_or(""))?;
        let chunk = file
            .chunk("CLUT", int(&spec["chunk"]))
            .ok_or("Cannot read properties of undefined (reading 'data')")?;
        common_palette = Some(chunk.data.clone());
        common_palette_hash = spec["sha256"].as_str().map(str::to_string);
        if common_palette_hash.as_deref() != Some(sha256(&chunk.data).as_str()) {
            return Err("unsupported dialogue palette".into());
        }
    }
    let mut d5_stage_palette = None;
    if policy["d5_stage_palette"].is_object() {
        let spec = &policy["d5_stage_palette"];
        let bytes = sources.external_palette(
            fs,
            spec["file"].as_str().unwrap_or(""),
            int(&spec["member"]),
        )?;
        if spec["sha256"] != sha256(&bytes).as_str() {
            return Err("D5 stage palette changed".into());
        }
        d5_stage_palette = Some(bytes);
    }
    let projector_name = policy["projector"]
        .as_str()
        .map(|p| basename(p).to_uppercase());
    let has_startup = policy["startup_movie"]
        .as_str()
        .is_some_and(|s| !s.is_empty());
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
    Ok(Setup {
        recovered_files,
        source_paths,
        common_palette,
        common_palette_hash,
        d5_stage_palette,
        jobs,
    })
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

/// Converts every movie and flattens the film loops.
pub fn compile_start(fs: &mut dyn Files, options: &Options) -> R<Pending> {
    let setup = compile_setup(fs, options)?;
    let mut palettes = HashMap::new();
    let mut converted = Vec::with_capacity(setup.jobs.len());
    for k in 0..setup.jobs.len() {
        converted.push(convert_job(fs, options, &setup, &mut palettes, k)?);
    }
    compile_merge(fs, options, converted)
}

/// The movies' results, in job order, into one model: embedded fonts and
/// film loops.
pub fn compile_merge(
    fs: &mut dyn Files,
    options: &Options,
    converted: Vec<Converted>,
) -> R<Pending> {
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
    super::cursor::bind(fs, output, &mut movies)?;
    super::patterns::bind(fs, options.media, output, policy, &mut movies)?;
    // Embedded fonts recover once per source hash, numbered in first-occurrence order.
    let mut fonts: Vec<Value> = Vec::new();
    let mut font_index: HashMap<String, usize> = HashMap::new();
    for record in font_records {
        let index = match font_index.get(&record.hash) {
            Some(&index) => index,
            None => {
                let pfr = files::join(output, &format!("fonts/{}.pfr", record.hash));
                let mut font =
                    super::font::recover(fs, &pfr, &files::join(output, "fonts"), &record.names)?;
                let object = font
                    .as_object_mut()
                    .ok_or("font recovery returned no record")?;
                object.insert("number".into(), json!(fonts.len() + 1));
                object.insert("sources".into(), json!([]));
                fonts.push(font);
                font_index.insert(record.hash.clone(), fonts.len() - 1);
                fonts.len() - 1
            }
        };
        let font = fonts[index].as_object_mut().unwrap();
        let mut aliases: Vec<String> = font["aliases"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(|v| v.as_str().map(str::to_string))
            .collect();
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
            let Some(hash) = entry
                .get("embeddedFontHash")
                .and_then(Value::as_str)
                .map(str::to_string)
            else {
                continue;
            };
            entry.insert(
                "embeddedFont".into(),
                fonts[font_index[&hash]]["number"].clone(),
            );
            entry.shift_remove("embeddedFontHash");
        }
    }
    super::system_font::bind(fs, options.media, output, policy, &mut fonts)?;
    let converted: BTreeSet<String> = movies
        .iter()
        .map(|m| m["name"].as_str().unwrap_or("").to_string())
        .collect();
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
    if truthy(&policy["native_stage"]) {
        trim_film_poses(fs, output, &mut movies, &mut limits)?;
    }
    Ok(Pending {
        movies,
        problems,
        limits,
        fonts,
    })
}

/// The browser and Android TV keep a flattened film-loop pose only where it
/// covers the stage (fdi::trim_image); the runtime places a pose smaller
/// than its loop by the two registration points. Trimmed after every loop
/// is flattened, so nested loops compose from full poses.
/// Every retained plane, including reusable clip/button definitions.
fn flash_assets<'a>(v: &'a Value, out: &mut Vec<&'a Value>) {
    match v {
        Value::Object(o) => {
            for key in ["asset", "maskAsset"] {
                if let Some(a) = o.get(key) {
                    out.push(a);
                }
            }
            for value in o.values() {
                flash_assets(value, out);
            }
        }
        Value::Array(a) => {
            for value in a {
                flash_assets(value, out);
            }
        }
        _ => {}
    }
}

fn trim_film_poses(
    fs: &mut dyn Files,
    output: &str,
    movies: &mut [Value],
    limits: &mut Vec<Value>,
) -> R<()> {
    let mut trimmed: HashMap<String, String> = HashMap::new();
    let (mut before, mut after) = (0usize, 0usize);
    for movie in movies.iter_mut() {
        for m in movie["members"].as_array_mut().unwrap() {
            let Some(assets) = m.get_mut("filmAssets").and_then(Value::as_array_mut) else {
                continue;
            };
            for asset in assets.iter_mut() {
                let Some(name) = asset.as_str().map(str::to_string) else {
                    continue;
                };
                if !trimmed.contains_key(&name) {
                    let data = fs.read(&files::join(output, &format!("images/{name}")))?;
                    let out = match crate::convert::fdi::trim_image(&data)? {
                        Some(cut) => {
                            before += data.len();
                            after += cut.len();
                            let cut_name = image_asset_name(&cut);
                            write_asset(
                                fs,
                                &files::join(output, &format!("images/{cut_name}")),
                                &cut,
                            )?;
                            cut_name
                        }
                        None => name.clone(),
                    };
                    trimmed.insert(name.clone(), out);
                }
                *asset = json!(trimmed[&name]);
            }
        }
    }
    // A full pose no member shows any more is not shipped.
    let shown = referenced_images(movies);
    for (name, out) in &trimmed {
        if name != out && !shown.contains(name) {
            fs.remove(&files::join(output, &format!("images/{name}")))?;
        }
    }
    limits.push(
        json!({"kind": "film-pose-trim", "poses": trimmed.len(), "storedBytesBefore": before,
        "storedBytesAfter": after, "implemented": true, "original_projector_verified": false}),
    );
    Ok(())
}

fn referenced_images(movies: &[Value]) -> BTreeSet<String> {
    let mut shown = BTreeSet::new();
    for movie in movies.iter() {
        for cast in movie["casts"].as_array().into_iter().flatten() {
            for asset in cast["patternAssets"]
                .as_array()
                .into_iter()
                .flatten()
                .filter_map(Value::as_str)
            {
                shown.insert(asset.to_string());
            }
        }
        for m in movie["members"].as_array().unwrap() {
            if let Some(asset) = m["asset"].as_str() {
                shown.insert(asset.to_string());
            }
            for asset in m["filmAssets"]
                .as_array()
                .into_iter()
                .flatten()
                .filter_map(Value::as_str)
            {
                shown.insert(asset.to_string());
            }
            let mut retained = Vec::new();
            flash_assets(&m["filmLayers"], &mut retained);
            flash_assets(&m["flashRuntime"], &mut retained);
            for asset in retained {
                if let Some(asset) = asset.as_str() {
                    shown.insert(asset.to_string());
                }
            }
        }
    }
    shown
}

// Output directories are reused by ordinary native/development builds. Drop
// only obsolete converter images, so old baked film frames cannot remain in
// the browser image pack after switching a member to live composition.
fn prune_images(fs: &mut dyn Files, output: &str, movies: &[Value]) -> R<()> {
    let used = referenced_images(movies);
    let directory = files::join(output, "images");
    for (name, other) in fs.list(&directory)? {
        if !other
            && [".fdi", ".fda", ".fd2"]
                .iter()
                .any(|ext| name.ends_with(ext))
            && !used.contains(&name)
        {
            fs.remove(&files::join(&directory, &name))?;
        }
    }
    Ok(())
}

/// The rest of the stage: deferrals, text fonts and the model.
pub fn compile_finish(fs: &mut dyn Files, options: &Options, pending: Pending) -> R<Compiled> {
    let (output, policy) = (options.output, options.policy);
    let Pending {
        mut movies,
        problems,
        mut limits,
        mut fonts,
    } = pending;
    // Policy-pinned deferrals turn documented residual conversions into
    // explicit approximations; a pin without its problem is stale.
    let deferrals = policy["deferred_conversions"]
        .as_array()
        .cloned()
        .unwrap_or_default();
    let mut used = vec![false; deferrals.len()];
    let mut remaining = Vec::new();
    for problem in problems {
        let error = problem["error"].as_str().unwrap_or("");
        let pin = deferrals.iter().enumerate().position(|(n, d)| {
            !used[n]
                && ["movie", "cast", "member"]
                    .iter()
                    .all(|k| d.get(*k) == problem.get(*k))
                && d["error"]
                    .as_str()
                    .is_some_and(|prefix| error.starts_with(prefix))
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
        return Err(format!(
            "stale deferred conversion pin: {} {}:{}",
            js_text(d.get("movie")),
            js_text(d.get("cast")),
            js_text(d.get("member"))
        ));
    }
    let problems = remaining;
    limits.extend(bind_text_fonts(
        &mut movies,
        &mut fonts,
        truthy(&policy["field_text_styles"]),
    )?);
    limits.extend(super::text::bind_flash_fonts(&movies, &mut fonts)?);
    let script_root = files::join(&files::parent(options.recovery), "lingo");
    let mut scripts = Vec::new();
    if fs.exists(&script_root) {
        let mut names: Vec<String> = fs
            .list(&script_root)?
            .into_iter()
            .map(|(name, _)| name)
            .collect();
        names.sort_by(|a, b| a.as_bytes().cmp(b.as_bytes()));
        for name in names.into_iter().filter(|n| n.ends_with(".lingo")) {
            let bytes = fs.read(&files::join(&script_root, &name))?;
            scripts.push((name, String::from_utf8_lossy(&bytes).into_owned()));
        }
    }
    let audit = text_reachability(&movies, &scripts);
    fs.write(
        &files::join(output, "font-audit.json"),
        (js::stringify_pretty(&audit) + "\n").as_bytes(),
    )?;
    prune_images(fs, output, &movies)?;
    let count = movies.len();
    let model = json!({
        "version": 1, "extendedD6": truthy(&policy["extended_d6"]), "movies": movies, "fonts": fonts,
        "problems": problems, "approximations": limits,
    });
    fs.write(
        &files::join(output, "model.json"),
        (js::stringify(&model) + "\n").as_bytes(),
    )?;
    let fatal: Vec<Value> = problems
        .iter()
        .filter(|p| {
            !(options.defer_video && p["error"] == "digital video requires target conversion")
        })
        .cloned()
        .collect();
    Ok(Compiled {
        movies: count,
        problems,
        fatal,
    })
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

/// Flattens the source film loops to native frame sequences. A loop nesting
/// another waits for it: passes repeat while one finishes a waiting loop.
fn flatten_films(
    fs: &mut dyn Files,
    output: &str,
    movies: &mut [Value],
    problems: &mut Vec<Value>,
    limits: &mut Vec<Value>,
) -> R<()> {
    let mut pending: Vec<(usize, Value, Value)> = Vec::new();
    for (index, movie) in movies.iter().enumerate() {
        for m in movie["members"]
            .as_array()
            .unwrap()
            .iter()
            .filter(|m| m.get("filmScore").is_some_and(truthy))
        {
            pending.push((index, m["cast"].clone(), m["number"].clone()));
        }
    }
    // What compose_film_frame reads: kept current with each flattened loop
    // instead of copied per loop.
    let mut snapshot: Vec<Value> = movies.to_vec();
    // Loops that nest each other (stale castlib numbers) wait forever; once no
    // loop makes progress, the next pass skips their pending children.
    let mut break_cycles = false;
    loop {
        let before = pending.len();
        let mut waiting = Vec::new();
        for (index, cast, number) in std::mem::take(&mut pending) {
            if flatten_film(
                fs,
                output,
                movies,
                &mut snapshot,
                index,
                &cast,
                &number,
                break_cycles,
                problems,
                limits,
            )? {
                waiting.push((index, cast, number));
            }
        }
        if waiting.is_empty() {
            return Ok(());
        }
        if waiting.len() == before {
            if break_cycles {
                for (index, cast, number) in waiting {
                    problems.push(json!({"movie": movies[index]["name"], "cast": cast, "member": number, "error": "film-loop nested cycle"}));
                }
                return Ok(());
            }
            break_cycles = true;
        }
        pending = waiting;
    }
}

/// Flattens one loop; true when it waits for a nested loop.
#[allow(clippy::too_many_arguments)]
fn flatten_film(
    fs: &mut dyn Files,
    output: &str,
    movies: &mut [Value],
    snapshot: &mut [Value],
    index: usize,
    cast: &Value,
    number: &Value,
    break_cycles: bool,
    problems: &mut Vec<Value>,
    limits: &mut Vec<Value>,
) -> R<bool> {
    let waiting = flatten_film_into(
        fs,
        output,
        movies,
        snapshot,
        index,
        cast,
        number,
        break_cycles,
        problems,
        limits,
    )?;
    if !waiting {
        let updated = movies[index]["members"]
            .as_array()
            .unwrap()
            .iter()
            .find(|m| m["cast"] == *cast && m["number"] == *number)
            .cloned();
        if let Some(slot) = snapshot[index]["members"]
            .as_array_mut()
            .unwrap()
            .iter_mut()
            .find(|m| m["cast"] == *cast && m["number"] == *number)
        {
            *slot = updated.unwrap();
        }
    }
    Ok(waiting)
}

#[allow(clippy::too_many_arguments)]
fn flatten_film_into(
    fs: &mut dyn Files,
    output: &str,
    movies: &mut [Value],
    snapshot: &[Value],
    index: usize,
    cast: &Value,
    number: &Value,
    break_cycles: bool,
    problems: &mut Vec<Value>,
    limits: &mut Vec<Value>,
) -> R<bool> {
    let movie = &snapshot[index];
    let loop_member = movie["members"]
        .as_array()
        .unwrap()
        .iter()
        .find(|m| m["cast"] == *cast && m["number"] == *number)
        .unwrap();
    let runtime = needs_runtime(movie, loop_member, snapshot)?;
    let mut runtime_member = loop_member.clone();
    runtime_member["filmRuntime"] = json!(runtime);
    let mut loop_limits = Vec::new();
    let mut film_assets: Option<Vec<Value>> = None;
    let mut film_sounds: Option<Vec<Value>> = None;
    let mut use_alpha = false;
    let (width, height) = (int(&loop_member["width"]), int(&loop_member["height"]));
    let frames = loop_member["filmScore"]["frames"]
        .as_array()
        .cloned()
        .unwrap_or_default();
    let target = movies[index]["members"]
        .as_array_mut()
        .unwrap()
        .iter_mut()
        .find(|m| m["cast"] == *cast && m["number"] == *number)
        .unwrap();
    if width == 0 && height == 0 && frames.is_empty() {
        let target = target.as_object_mut().unwrap();
        target.insert("empty".into(), json!(true));
        target.insert("filmAssets".into(), json!([]));
        return Ok(false);
    }
    let result = (|| -> R<()> {
        // Runtime composition tiles wide scrolling strips and stores only a
        // one-pixel pose/count placeholder; flattened bitmap bounds remain.
        let bound = if runtime { 32767 } else { 4096 };
        if width < 1
            || height < 1
            || width > bound
            || height > bound
            || width * height > 16 * 1024 * 1024
        {
            return Err("film-loop bounds".into());
        }
        let mut states: Vec<(i64, Value)> = Vec::new();
        film_assets = Some(Vec::new());
        film_sounds = Some(Vec::new());
        use_alpha = true;
        for (frame_index, frame) in frames.iter().enumerate() {
            for c in frame["channels"].as_array().into_iter().flatten() {
                let channel = int(&c["channel"]);
                match states.iter_mut().find(|s| s.0 == channel) {
                    Some(slot) => slot.1 = c.clone(),
                    None => states.push((channel, c.clone())),
                }
            }
            let composed = {
                let fs_ref: &dyn Files = fs;
                let mut load = |asset: &str| {
                    if asset.is_empty() {
                        return Err("film-loop child without a converted image".to_string());
                    }
                    fs_ref.read(&files::join(output, &format!("images/{asset}")))
                };
                compose_film_frame(
                    movie,
                    &runtime_member,
                    frame_index,
                    break_cycles,
                    &states,
                    &mut load,
                    snapshot,
                    &mut loop_limits,
                )?
            };
            let asset = image_asset_name(&composed.data);
            write_asset(
                fs,
                &files::join(output, &format!("images/{asset}")),
                &composed.data,
            )?;
            film_assets.as_mut().unwrap().push(json!(asset));
            film_sounds.as_mut().unwrap().push(composed.sounds);
        }
        Ok(())
    })();
    if result.as_ref().is_err_and(|e| e == NESTED_PENDING) {
        return Ok(true);
    }
    let target = target.as_object_mut().unwrap();
    if runtime {
        target.insert("filmRuntime".into(), json!(true));
    }
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
        problems
            .push(json!({"movie": movie["name"], "cast": cast, "member": number, "error": error}));
        return Ok(false);
    }
    let sounds = film_sounds.unwrap_or_default();
    if int(&loop_member["filmScore"]["version"]) != 7
        && int(&movie["directorVersion"]) < 850
        && sounds
            .iter()
            .any(|f| f.as_array().is_some_and(|pair| pair.iter().any(truthy)))
    {
        limits.push(
            json!({"kind": "film-loop-sounds-pending-native", "movie": movie["name"],
            "cast": cast, "member": number, "implemented": false}),
        );
    }
    let first = film_assets
        .unwrap_or_default()
        .first()
        .cloned()
        .unwrap_or(Value::Null);
    target.insert("asset".into(), first);
    Ok(false)
}

#[cfg(test)]
mod image_pruning_tests {
    use super::*;
    #[test]
    fn recompilation_removes_old_bakes_but_keeps_catalog_and_live_layer_assets() {
        let mut fs = files::Memory::default();
        fs.mkdir_all("out/images").unwrap();
        for name in [
            "old-bake.fdi",
            "pose.fdi",
            "child.fd2",
            "brush.fdi",
            "leaf.fda",
            "mask.fda",
            "notes.txt",
        ] {
            fs.write(&format!("out/images/{name}"), b"synthetic")
                .unwrap();
        }
        let movies = vec![json!({"casts":[{"patternAssets":["brush.fdi"]}],"members":[
            {"asset":"child.fd2"},{"filmAssets":["pose.fdi","pose.fdi"],
             "flashRuntime":{"clips":[{"frames":[[{"asset":"leaf.fda","maskAsset":"mask.fda"}]]}]}}
        ]})];
        prune_images(&mut fs, "out", &movies).unwrap();
        assert!(!fs.exists("out/images/old-bake.fdi"));
        for name in [
            "pose.fdi",
            "child.fd2",
            "brush.fdi",
            "leaf.fda",
            "mask.fda",
            "notes.txt",
        ] {
            assert!(fs.exists(&format!("out/images/{name}")), "{name}");
        }
    }
}

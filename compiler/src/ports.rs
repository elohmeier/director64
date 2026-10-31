//! What a port adds to the converted game beyond the generic pipeline, from
//! its game.toml [convert] table: the embedded projector's launcher
//! handlers, program patches, a system-font substitute (with metrics
//! where a port needs them), external media bound to
//! their members, small source-data fixes, and the disc folders the runtime
//! streams, reads as data or prints. Every patch and fix is pinned to the
//! exact source it was reviewed against and refuses anything else.

use std::collections::{BTreeMap, BTreeSet, HashMap};

use serde_json::{Map, Value, json};

use crate::convert::source::sha256;
use crate::lingo;

pub type R<T> = Result<T, String>;

/// A port: its game's slug and edition, the launcher movie its embedded
/// projector's handlers join, and its [convert] table.
#[derive(Clone, Debug, Default)]
pub struct Port {
    pub slug: String,
    pub source_sha256: String,
    pub launcher_movie: Option<String>,
    pub convert: Value,
}

impl Port {
    /// The port as the conversion driver describes it: {slug, source_sha256,
    /// launcher_movie, convert}.
    pub fn from_json(value: &Value) -> R<Port> {
        Ok(Port {
            slug: value["slug"].as_str().ok_or("port without a slug")?.into(),
            source_sha256: value["source_sha256"].as_str().unwrap_or("").into(),
            launcher_movie: value["launcher_movie"].as_str().map(str::to_string),
            convert: value.get("convert").cloned().unwrap_or(json!({})),
        })
    }
}

/// What the port reads besides the model: the substitute font, how many
/// data files it ships, and its converted external media, one record per
/// source file ({source, source_sha256, source_bytes, duration, video,
/// audio[, frames, rate, channels, audioOnly]}).
#[derive(Default)]
pub struct PortFonts {
    pub droid_sans: Option<Vec<u8>>,
    pub data_files: usize,
    pub external_media: Vec<Value>,
}

fn list(value: &Value) -> std::slice::Iter<'_, Value> {
    value.as_array().map_or([].iter(), |a| a.iter())
}
fn text(value: &Value) -> &str {
    value.as_str().unwrap_or("")
}

// ---- The program ----

/// The embedded projector's handlers: each launcher script, parsed with its
/// cast, member and type.
pub fn launcher_handlers(source_manifest: &Value) -> R<Vec<Value>> {
    let launcher = &source_manifest["supplemental_projector"];
    let name = launcher["name"]
        .as_str()
        .ok_or("source manifest without a supplemental projector")?;
    let mut handlers = Vec::new();
    for script in list(&launcher["scripts"]) {
        let member = list(&launcher["members"])
            .rfind(|m| m["id"] == script["member_id"])
            .ok_or("launcher script without its member")?;
        let cast = list(&launcher["libraries"])
            .rfind(|c| c["library_id"] == member["library_id"])
            .and_then(|c| c["name"].as_str())
            .ok_or("launcher member without its cast")?;
        let source = format!(
            "-- cast: {cast}; member: {}; type: {}; name: launcher\n{}",
            member["member_id"],
            text(&script["script_type"]),
            text(&script["lingo"])
        );
        handlers.extend(lingo::parse_movie(&source, name).map_err(|e| e.to_string())?);
    }
    Ok(handlers)
}

/// The program the runtime executes: the recovered program with the port's
/// patches, then the launcher's handlers under the launcher movie's name.
pub fn executable_program(program: &Value, port: &Port, source_manifest: &Value) -> R<Value> {
    let mut program = program.clone();
    if let Some(movie) = &port.launcher_movie {
        let handlers = program["handlers"]
            .as_array_mut()
            .ok_or("program without handlers")?;
        if handlers.iter().any(|h| h["movie"] == movie.as_str()) {
            return Err("embedded launcher conflicts with a media movie".into());
        }
        for mut handler in launcher_handlers(source_manifest)? {
            let object = handler.as_object_mut().ok_or("launcher handler")?;
            let source = object.get("movie").cloned().unwrap_or(Value::Null);
            object.insert("source_movie".into(), source);
            object.insert("movie".into(), json!(movie));
            handlers.push(handler);
        }
    }
    patch_program(&program, &port.convert["patch"])
}

/// Applies [[convert.patch]] entries. Each names a handler (movie, member,
/// cast and handler; or `all` handlers of that name), pins it by
/// `source_sha256` (or by `calls`, a call its body must make), and replaces
/// its `body` with Lingo, sets its `parameters`, or renames a call
/// (`rename_call = [from, to]`, `count` sites), or corrects an appended
/// variable (`retarget_append = [from, to]`, `count` sites). The program records each
/// patch under compatibility_fixes.
pub fn patch_program(program: &Value, patches: &Value) -> R<Value> {
    let mut result = program.clone();
    let mut fixes = Vec::new();
    for patch in list(patches) {
        let name = text(&patch["handler"]).to_lowercase();
        let handlers: Vec<&mut Value> = result["handlers"]
            .as_array_mut()
            .ok_or("program without handlers")?
            .iter_mut()
            .filter(|h| {
                h["name"] == name.as_str()
                    && (patch["all"] == true
                        || (h["movie"] == patch["movie"]
                            && h["member"] == patch["member"]
                            && (patch["cast"].is_null() || h["cast"] == patch["cast"])))
            })
            .collect();
        // `all` covers every handler of the name, if any (Optik has none).
        if patch["all"] != true && handlers.len() != 1 {
            return Err(format!("patch {}: {} handlers match; review required", patch, handlers.len()));
        }
        for handler in handlers {
            if let Some(expected) = patch["source_sha256"].as_str()
                && handler["source_sha256"] != expected
            {
                return Err(format!("{} {name} source changed; review required", handler["movie"]));
            }
            if let Some(call) = patch["calls"].as_str()
                && !handler["body"].to_string().contains(&format!("\"{call}\""))
            {
                return Err(format!("{} {name} no longer calls {call}", handler["movie"]));
            }
            if let Some(parameters) = patch.get("parameters") {
                handler["parameters"] = parameters.clone();
            }
            if let Some([from, to]) = patch["rename_call"].as_array().map(Vec::as_slice) {
                let count = rename_calls(&mut handler["body"], text(from), text(to));
                if Some(count as u64) != patch["count"].as_u64() {
                    return Err(format!("{} {name}: {count} {from} calls; review required", handler["movie"]));
                }
            }
            if let Some([from, to]) = patch["retarget_append"].as_array().map(Vec::as_slice) {
                let count = retarget_appends(&mut handler["body"], text(from), text(to));
                if Some(count as u64) != patch["count"].as_u64() {
                    return Err(format!("{} {name}: {count} appends to {from}; review required", handler["movie"]));
                }
            }
            if patch["body"].is_string() && patch["prepend"].is_string() {
                return Err("patch cannot replace and prepend one handler".into());
            }
            if let Some(body) = patch["body"].as_str().or_else(|| patch["prepend"].as_str()) {
                let line = list(&handler["body"])
                    .next()
                    .and_then(|s| s["line"].as_u64())
                    .or_else(|| handler["line"].as_u64())
                    .unwrap_or(0) as usize;
                let lines: Vec<(usize, String)> = body
                    .lines()
                    .filter(|l| !l.trim().is_empty())
                    .map(|l| (line, l.trim().to_string()))
                    .collect();
                let mut statements = lingo::block(&lines).map_err(|e| e.to_string())?;
                if patch["prepend"].is_string() {
                    statements.extend(handler["body"].as_array().ok_or("handler body")?.iter().cloned());
                }
                handler["body"] = Value::Array(statements);
            }
            fixes.push(json!({"id": patch["id"], "movie": handler["movie"], "member": handler["member"],
                              "handler": name, "source_sha256": handler["source_sha256"]}));
        }
    }
    if !fixes.is_empty() {
        result
            .as_object_mut()
            .ok_or("program")?
            .insert("compatibility_fixes".into(), Value::Array(fixes));
    }
    Ok(result)
}

fn rename_calls(node: &mut Value, from: &str, to: &str) -> usize {
    match node {
        Value::Array(items) => {
            let mut count = 0;
            if items.len() == 3 && items[0] == "call" && items[1] == from {
                items[1] = json!(to);
                count += 1;
            }
            count + items.iter_mut().map(|v| rename_calls(v, from, to)).sum::<usize>()
        }
        Value::Object(object) => object.values_mut().map(|v| rename_calls(v, from, to)).sum(),
        _ => 0,
    }
}

// Fix a recovered `put ... after wrongVariable` without replacing its
// surrounding handler or changing other assignments to that variable.
fn retarget_appends(node: &mut Value, from: &str, to: &str) -> usize {
    if node["op"] == "set" && node["target"] == json!(["variable", from])
        && node["value"][0] == "binary" && node["value"][1] == "&"
        && node["value"][2] == json!(["variable", from]) {
        node["target"][1] = json!(to);
        node["value"][2][1] = json!(to);
        return 1;
    }
    match node {
        Value::Array(items) => items.iter_mut().map(|v| retarget_appends(v, from, to)).sum(),
        Value::Object(object) => object.values_mut().map(|v| retarget_appends(v, from, to)).sum(),
        _ => 0,
    }
}

// ---- The plan ----

/// What the conversion driver does for a port around postprocess_model: the
/// fonts it measures first (tools/convert/font_metrics.c), each {key, font
/// ("substitute" or a converted font asset), size, ranges}; the disc folders the runtime streams as sound (playFile) or
/// reads as data (read_data); the print folder; and the external media to
/// convert ({root, extensions}).
pub fn plan(port: &Port, model: &Value) -> R<Value> {
    let convert = &port.convert;
    let mut measure = Vec::new();
    if let Some(size) = convert["substitute_font"]["controller"]["size"].as_i64() {
        measure.push(json!({"key": format!("substitute:{size}"), "font": "substitute",
                            "size": size, "ranges": ["0x20-0x7e"]}));
    }
    if convert["substitute_font"]["measure"] == true {
        let sizes: BTreeSet<i64> = list(&model["movies"])
            .flat_map(|movie| list(&movie["members"]))
            .flat_map(|member| list(&member["sourceTextStyles"]).chain(list(&member["textFormats"])))
            .filter_map(|run| run["size"].as_i64())
            .map(|size|if size==0 {12}else{size})
            .collect();
        for size in sizes {
            let key = format!("substitute:{size}");
            if !measure.iter().any(|request| request["key"] == key) {
                measure.push(json!({"key": key, "font": "substitute", "size": size,
                                    "ranges": ["0x20-0x7e"]}));
            }
        }
    }
    if convert["measure_fonts"] == true {
        measure.extend(measure_requests(model)?);
    }
    let mut plan = json!({"measure": measure,
        "streams": convert.get("streams").cloned().unwrap_or(json!([])),
        "data": convert.get("data").cloned().unwrap_or(json!([]))});
    for key in ["print", "external_media", "stream_prefer"] {
        if let Some(value) = convert.get(key) {
            plan[key] = value.clone();
        }
    }
    Ok(plan)
}

/// The (font, size) variants a port's layout reads advances for: every text
/// style's, and every Flash edit field's by its font alias, measured over
/// the font's own codepoints in 32..=255 as dense runs.
fn measure_requests(model: &Value) -> R<Vec<Value>> {
    let fonts: BTreeMap<i64, &Value> = list(&model["fonts"])
        .filter_map(|f| f["number"].as_i64().map(|n| (n, f)))
        .collect();
    let mut aliases = HashMap::new();
    for (&number, font) in &fonts {
        for alias in list(&font["aliases"]).filter_map(Value::as_str) {
            aliases.insert(alias.to_lowercase(), number);
        }
    }
    let mut pairs = BTreeSet::new();
    for member in list(&model["movies"]).flat_map(|m| list(&m["members"])) {
        for key in ["textStyle", "textInsertStyle"] {
            let style = &member[key];
            if let (Some(font), Some(size)) = (style["fontId"].as_i64(), style["size"].as_i64())
                && font != 0
            {
                pairs.insert((font, size));
            }
        }
        for run in list(&member["textFormats"]) {
            if let (Some(font),Some(size))=(run["fontId"].as_i64(),run["size"].as_i64())
                && font!=0 { pairs.insert((font,if size==0 {12}else{size})); }
        }
        for field in list(&member["flashFields"]).chain(list(&member["flashFontUses"])) {
            if let Some(&number) = aliases.get(&text(&field["fontName"]).to_lowercase()) {
                let height = field["fontHeight"].as_f64().filter(|h| *h != 0.0).unwrap_or(12.0);
                pairs.insert((number, (height.round_ties_even() as i64).max(1)));
            }
        }
    }
    pairs
        .into_iter()
        .map(|(number, size)| {
            let font = fonts.get(&number).ok_or(format!("font {number} not in the model"))?;
            let codepoints: Vec<i64> =
                list(&font["codepoints"]).filter_map(Value::as_i64).collect();
            Ok(json!({"key": format!("{number}:{size}"), "font": font["asset"], "size": size,
                      "ranges": metric_ranges(&codepoints)?}))
        })
        .collect()
}

/// Contiguous --range arguments over a font's own codepoints within 32..=255.
fn metric_ranges(codepoints: &[i64]) -> R<Vec<String>> {
    let mut covered: Vec<i64> = codepoints.iter().copied().filter(|c| (32..=255).contains(c)).collect();
    covered.sort_unstable();
    covered.dedup();
    let Some(&first) = covered.first() else {
        return Err("font covers no measurable codepoints".into());
    };
    let (mut runs, mut start, mut previous) = (Vec::new(), first, first);
    for &c in &covered[1..] {
        if c != previous + 1 {
            runs.push(format!("0x{start:X}-0x{previous:X}"));
            start = c;
        }
        previous = c;
    }
    runs.push(format!("0x{start:X}-0x{previous:X}"));
    Ok(runs)
}

// ---- The model ----

/// The port's model corrections after conversion. `measure(key)` returns the
/// metrics measured for one of plan()'s requests (tools/convert/font_metrics.c).
pub fn postprocess_model(
    model: &mut Value,
    port: &Port,
    fonts: &PortFonts,
    measure: &mut dyn FnMut(&str) -> R<Vec<u8>>,
) -> R<()> {
    let convert = &port.convert;
    match convert["external_media"]["bind"].as_str() {
        None => {}
        Some("linked-movies") => linked_movies(model, fonts, text(&convert["external_media"]["sounds_movie"]))?,
        Some("linked-sounds") => linked_sounds(model, fonts, &convert["external_media"]["path_aliases"] )?,
        Some("directmedia") => directmedia(model, fonts)?,
        Some(other) => return Err(format!("unknown external media binding {other}")),
    }
    if convert["substitute_font"].is_object() {
        substitute(model, &convert["substitute_font"], fonts, measure)?;
    }
    if convert["measure_fonts"] == true {
        let mut metrics = Map::new();
        for request in measure_requests(model)? {
            let key = text(&request["key"]).to_string();
            let packed = measure(&key)?;
            metrics.insert(key, font_metrics(&packed)?);
        }
        model["fontMetrics"] = Value::Object(metrics);
    }
    for fix in list(&convert["text_fix"]) {
        let member = member_mut(model, text(&fix["movie"]), fix["member"].as_i64().unwrap_or(0))?;
        if sha256(text(&member["text"]).as_bytes()) != text(&fix["text_sha256"]) {
            return Err(format!("{} member {} text changed; review required", fix["movie"], fix["member"]));
        }
        let mut chars: Vec<char> = text(&member["text"]).chars().collect();
        chars.truncate(chars.len().saturating_sub(fix["trim"].as_u64().unwrap_or(0) as usize));
        member["text"] = json!(chars.into_iter().collect::<String>());
        let extent=text(&member["text"]).encode_utf16().count() as u64;
        if let Some(formats)=member["textFormats"].as_array_mut() {
            let insertion=formats.last().filter(|r|r["offset"].as_u64().is_some_and(|o|o>extent)).cloned();
            formats.retain(|r|r["offset"].as_u64().is_some_and(|o|o<=extent));
            if let Some(mut insertion)=insertion {
                insertion["offset"]=json!(extent);
                if formats.last().is_some_and(|r|r["offset"]==extent){formats.pop();}
                formats.push(insertion);
            }
        }

    }
    for fix in list(&convert["tempo_fix"]) {
        tempo_fix(model, fix)?;
    }
    Ok(())
}

fn member_mut<'a>(model: &'a mut Value, file: &str, number: i64) -> R<&'a mut Value> {
    model["movies"]
        .as_array_mut()
        .into_iter()
        .flatten()
        .filter(|f| f["name"] == file)
        .flat_map(|f| f["members"].as_array_mut().into_iter().flatten())
        .find(|m| m["number"] == number)
        .ok_or_else(|| format!("{file} member {number} missing"))
}

/// A score frame's tempo set to `to` where the source has `from`
/// ([[convert.tempo_fix]] {movie, frame, from, to}); repeatable.
fn tempo_fix(model: &mut Value, fix: &Value) -> R<()> {
    let invalid = || format!("tempo fix {fix} no longer matches the recovered score");
    let movie = model["movies"]
        .as_array_mut()
        .into_iter()
        .flatten()
        .find(|m| m["name"] == fix["movie"])
        .ok_or_else(invalid)?;
    let frame = movie["score"]["frames"]
        .as_array_mut()
        .into_iter()
        .flatten()
        .find(|f| f["number"] == fix["frame"])
        .ok_or_else(invalid)?;
    let tempo = frame["channels"]
        .as_array_mut()
        .into_iter()
        .flatten()
        .find(|c| c["channel"] == 1)
        .ok_or_else(invalid)?;
    if !(tempo["tempoRaw"] == fix["from"] || tempo["tempoRaw"] == fix["to"]) || tempo["changed"][6] != 1 {
        return Err(invalid());
    }
    tempo["tempoRaw"] = fix["to"].clone();
    tempo["bytes"][6] = fix["to"].clone();
    Ok(())
}

/// [convert.substitute_font]: every member with source text styles sets in
/// the pinned Droid Sans (font 1), preserving every run's size and metrics, under
/// `asset`. `names` maps source font ids to the names scripts see (default
/// "Director system font N"); `sizes` adds variants; `controller` {members,
/// size} gives those fields the measured metrics of controller entry.
fn substitute(
    model: &mut Value,
    spec: &Value,
    fonts: &PortFonts,
    measure: &mut dyn FnMut(&str) -> R<Vec<u8>>,
) -> R<()> {
    let source = fonts.droid_sans.as_ref().ok_or("the substitute font is missing")?;
    let controller = match spec["controller"]["size"].as_i64() {
        Some(size) => Some((size, ascii_metrics(&measure(&format!("substitute:{size}"))?)?)),
        None => None,
    };
    let controller_members: Vec<&str> = list(&spec["controller"]["members"]).map(text).collect();
    let mut sizes: BTreeSet<i64> = list(&spec["sizes"]).filter_map(Value::as_i64).collect();
    let mut source_fonts = BTreeSet::new();
    let mut measured_sizes = BTreeMap::new();
    if let Some((size, metrics)) = &controller {
        measured_sizes.insert(*size, metrics.clone());
    }
    let mut run_limits=Vec::new();
    for movie in model["movies"].as_array_mut().into_iter().flatten() {
      let modern=movie["directorVersion"].as_i64().is_none_or(|v|v>=600);
      let movie_name=movie["name"].clone();
      for member in movie["members"].as_array_mut().into_iter().flatten() {
        for style in list(&member["sourceTextStyles"]) {
            source_fonts.insert(style["sourceFontId"].as_i64().unwrap_or(0));
        }
        if modern {
            if let Some(formats)=member["textFormats"].as_array_mut() {
                for run in formats {run["fontId"]=json!(1);let size=run["size"].as_i64().unwrap_or(0);sizes.insert(if size==0 {12}else{size});}
            } else if let Some(styles)=member["sourceTextStyles"].as_array() {
                let align=match member["textAlign"].as_i64().unwrap_or(0){-1=>2,1=>1,_=>0};
                let mut formats=Vec::new();
                for style in styles {
                    let size=style["size"].as_i64().unwrap_or(0);
                    // Preserve zero in STXT/RICH, as in the source data. Rendering
                    // uses the twelve-point default measured on original8.5;
                    // this is an explicit fallback inference for older D6 STXT,
                    // whose selected zero-size records are database text styles.
                    if !(0..=255).contains(&size){return Err("invalid substitute text run size".into());}
                    sizes.insert(if size==0 {12}else{size});source_fonts.insert(style["sourceFontId"].as_i64().unwrap_or(0));
                    let face=style["face"].as_u64().unwrap_or(0);
                    if face&!7!=0{run_limits.push(json!({"movie":movie_name,"cast":member["cast"],"member":member["number"],"kind":"text-layout","detail":"unsupported character face effects"}));}
                    let offset=style["offset"].as_u64().unwrap_or(0);
                    if formats.last().is_some_and(|last:&Value|last["offset"]==offset){formats.pop();}
                    formats.push(json!({"offset":offset,
                        "fontId":1,"size":size,"face":face&7,"align":align,
                        "ascent":style["ascent"].as_i64().unwrap_or(0),
                        "lineHeight":style["lineHeight"].as_i64().unwrap_or(0).max(0),
                        "color":style["color"].as_u64().unwrap_or(0)}));
                }
                member["textFormats"]=json!(formats);
            }
        }
        let Some(style) = member.get("sourceTextStyles").and_then(|s| s.get(0)).cloned() else {
            if modern && member["textFormats"].is_array() {
                for key in ["textStyle","textInsertStyle"] {
                    if member[key].is_object() {
                        member[key]["fontId"]=json!(1);
                        if let Some(size)=member[key]["size"].as_i64(){sizes.insert(size);}
                    }
                }
                member["textPresentation"]=json!("explicit-system-font-substitute");
            }
            continue;
        };
        let font = style["sourceFontId"].as_i64().unwrap_or(0);
        source_fonts.insert(font);
        let raw_size = style["size"].as_i64().unwrap_or(0);
        let size = if raw_size==0 {12}else{raw_size};
        sizes.insert(size);
        let align = match member.get("textAlign").and_then(Value::as_i64).unwrap_or(0) {
            -1 => 2,
            1 => 1,
            _ => 0,
        };
        let line_height = style["lineHeight"].as_i64().unwrap_or(0);
        let ascent = style["ascent"].as_i64().unwrap_or(0);
        let name = spec["names"][font.to_string()]
            .as_str()
            .map_or(format!("Director system font {font}"), str::to_string);
        let mut text_style = json!({"fontName": name, "fontId": 1, "size": size,
            "align": align, "ascent": style["ascent"], "descent": (line_height - ascent).max(0),
            "leading": 0, "lineHeight": line_height.max(1), "color": style["color"]});
        if spec["measure"] == true {
            if !measured_sizes.contains_key(&size) {
                let metrics = ascii_metrics(&measure(&format!("substitute:{size}"))?)?;
                measured_sizes.insert(size, metrics);
            }
            text_style["controllerMetrics"] = measured_sizes[&size].clone();
        } else if let Some((controller_size, metrics)) = &controller
            && size == *controller_size
            && controller_members.contains(&text(&member["name"]))
        {
            text_style["controllerMetrics"] = metrics.clone();
        }
        let m = member.as_object_mut().ok_or("member")?;
        m.insert("textStyle".into(), text_style);
        m.insert("textPresentation".into(), json!("explicit-system-font-substitute"));
    }
    }
    if !run_limits.is_empty() {
        if !model["approximations"].is_array(){model["approximations"]=json!([]);}
        model["approximations"].as_array_mut().unwrap().extend(run_limits);
    }
    model["fonts"] = json!([{
        "number": 1, "asset": spec["asset"], "sha256": sha256(source),
        "kind": "system-font-substitute", "sourceFontIds": source_fonts,
        "targetFont": "Droid Sans", "license": "Apache-2.0",
        "variants": sizes.iter().map(|s| json!({"size": s, "asset": format!("fonts/f1-{s}.font64")}))
            .collect::<Vec<_>>(),
    }]);
    Ok(())
}

fn base_name(path: &str) -> &str {
    path.rsplit('/').next().unwrap_or(path)
}

/// Löwenzahn 1's linked QuickTime movies and external AIFF sounds: each
/// video member takes its file's record; the sounds join `sounds_movie`'s
/// cast as members numbered from 1000.
fn linked_movies(model: &mut Value, fonts: &PortFonts, sounds_movie: &str) -> R<()> {
    if list(&model["problems"]).any(|p| p["error"] != "digital video requires target conversion") {
        return Err("unresolved non-video resources".into());
    }
    let mut records = fonts.external_media.clone();
    records.sort_by(|a, b| {
        let parts = |v: &Value| text(&v["source"]).split('/').map(str::to_string).collect::<Vec<_>>();
        parts(a).cmp(&parts(b))
    });
    let mut by_name = HashMap::new();
    for record in records.iter().filter(|r| !text(&r["source"]).to_uppercase().ends_with(".AIF")) {
        if by_name.insert(base_name(text(&record["source"])).to_lowercase(), record.clone()).is_some() {
            return Err("ambiguous external filename".into());
        }
    }
    for member in model["movies"]
        .as_array_mut()
        .into_iter()
        .flatten()
        .flat_map(|m| m["members"].as_array_mut().into_iter().flatten())
    {
        if member["type"] != 10 {
            continue;
        }
        let linked = text(&member["linkedFile"]).to_lowercase();
        let record = by_name.get(&linked).ok_or(format!("unresolved linked video: {}", member["linkedFile"]))?;
        let flags = member["videoFlags"].as_i64().unwrap_or(0);
        let duration = record["duration"].as_f64().unwrap_or(0.0);
        let audio_only = record["audioOnly"] == true;
        let has = |key: &str| !text(&record[key]).is_empty();
        if !has("video") && !(audio_only && has("audio")) {
            return Err(format!("linked video was not converted: {}", member["linkedFile"]));
        }
        if audio_only && has("video") {
            return Err(format!("audio-only movie has a picture asset: {}", member["linkedFile"]));
        }
        if !duration.is_finite() || duration <= 0.0 || duration * 600.0 > u32::MAX as f64 {
            return Err(format!("invalid linked video duration: {}", member["linkedFile"]));
        }
        let m = member.as_object_mut().ok_or("member")?;
        if !audio_only && m.get("videoBoundsFromMedia") == Some(&json!(true)) {
            let width = record["width"].as_u64().ok_or("linked video width missing")?;
            let height = record["height"].as_u64().ok_or("linked video height missing")?;
            if width == 0 || height == 0 || width > 4096 || height > 4096 {
                return Err("linked video dimensions out of bounds".into());
            }
            m.insert("width".into(), json!(width)); m.insert("height".into(), json!(height));
            m.insert("regX".into(), json!(width / 2)); m.insert("regY".into(), json!(height / 2));
        }
        m.insert("asset".into(), record["video"].clone());
        m.insert("videoAudio".into(), record["audio"].clone());
        m.insert("videoAudioOnly".into(), json!(audio_only));
        m.insert("frames".into(), json!((duration * 600.0).round_ties_even() as i64));
        m.insert("rate".into(), json!(600));
        m.insert("videoSource".into(), record["source"].clone());
        m.insert("videoFrameRate".into(), json!(15));
        m.insert("videoLoop".into(), json!(flags & 16 != 0));
        m.insert("videoPaused".into(), json!(flags & 256 != 0));
    }
    let shared = model["movies"]
        .as_array_mut()
        .into_iter()
        .flatten()
        .find(|m| m["name"] == sounds_movie)
        .ok_or(format!("{sounds_movie} missing"))?;
    let members = shared["members"].as_array_mut().ok_or("sound movie members")?;
    members.retain(|m| !m.get("externalAudio").is_some_and(|v| !v.is_null() && v != "" && v != &json!(false)));
    for (n, record) in records.iter().filter(|r| text(&r["source"]).ends_with(".AIF")).enumerate() {
        members.push(json!({"number": 1000 + n, "cast": 1, "type": 6,
            "name": base_name(text(&record["source"])), "asset": record["audio"],
            "rate": record["rate"], "frames": record["frames"], "channels": record["channels"],
            "loopStart": 0, "loopEnd": record["frames"], "looping": 0,
            "externalAudio": record["source"]}));
    }
    model["problems"] = json!([]);
    model["externalMedia"] = Value::Array(records);
    Ok(())
}

/// Linked sounds (Findus' Circus): empty sound members named after an
/// external "<name>.aif" take that file's converted record.
fn linked_sounds(model: &mut Value, fonts: &PortFonts, aliases: &Value) -> R<()> {
    let mut by_name: HashMap<String, Vec<&Value>> = HashMap::new();
    for record in &fonts.external_media {
        let file = base_name(text(&record["source"])).to_lowercase();
        let stem = file.strip_suffix(".aif").ok_or("external sound is not AIFF")?.to_string();
        by_name.entry(stem).or_default().push(record);
    }
    let mut bound = 0;
    for member in model["movies"]
        .as_array_mut()
        .into_iter()
        .flatten()
        .flat_map(|m| m["members"].as_array_mut().into_iter().flatten())
    {
        if member["type"] != 6 || member["empty"] != true {
            continue;
        }
        let Some(candidates) = by_name.get(&text(&member["name"]).to_lowercase()) else {
            continue;
        };
        let directory = text(&member["linkedDirectory"]).rsplit([':', '\\', '/']).next().unwrap_or("").to_uppercase();
        let directory = aliases[&directory].as_str().unwrap_or(&directory);
        let matching: Vec<_> = candidates.iter().filter(|record|
            text(&record["source"]).rsplit_once('/').is_some_and(|(p,_)| p.eq_ignore_ascii_case(directory))).collect();
        let record = if matching.len() == 1 { matching[0] }
            else if candidates.len() == 1 { &candidates[0] }
            else { return Err(format!("ambiguous external sound file {} in {directory}",member["name"])) };
        if text(&record["audio"]).is_empty() {
            return Err(format!("sound {} did not convert", member["name"]));
        }
        let m = member.as_object_mut().ok_or("member")?;
        m.remove("empty");
        for key in ["rate", "frames", "channels"] {
            m.insert(key.into(), record[key].clone());
        }
        m.insert("asset".into(), record["audio"].clone());
        m.insert("loopStart".into(), json!(0));
        m.insert("loopEnd".into(), record["frames"].clone());
        m.insert("looping".into(), json!(0));
        m.insert("externalAudio".into(), record["source"].clone());
        if let Some(cues) = record.get("cuePoints") { m.insert("cuePoints".into(), cues.clone()); }
        bound += 1;
    }
    if bound == 0 {
        return Err("linked sounds not found".into());
    }
    model["externalMedia"] = Value::Array(fonts.external_media.clone());
    Ok(())
}

/// DirectMedia and QuickTime video Xtra members (the Löwenzahn 8.5 titles)
/// bound to their converted MPEG streams, found among the paths the
/// recovery verified for them; never a manufactured DOS filename.
fn directmedia(model: &mut Value, fonts: &PortFonts) -> R<()> {
    let mut by_path = HashMap::new();
    for record in &fonts.external_media {
        let source = text(&record["source"]).replace('\\', "/").to_uppercase();
        if by_path.insert(source, record).is_some() {
            return Err("ambiguous external movie path".into());
        }
    }
    for member in model["movies"]
        .as_array_mut()
        .into_iter()
        .flatten()
        .flat_map(|m| m["members"].as_array_mut().into_iter().flatten())
    {
        let symbol = text(&member["xtra"]["symbol"]);
        if symbol != "TBDIRECTMEDIA" && symbol != "quickTimeMedia" {
            continue;
        }
        let quicktime = symbol == "quickTimeMedia";
        let candidates = member["linkedSourceCandidates"]
            .as_array()
            .ok_or("video member has no verified media candidates")?;
        let record = candidates
            .iter()
            .filter_map(Value::as_str)
            .find_map(|p| by_path.get(&p.to_uppercase()))
            .ok_or_else(|| format!("unresolved external video {} (candidates: {candidates:?})", member["linkedFile"]))?;
        if text(&record["video"]).is_empty() {
            return Err(format!("external video was not converted: {}", record["source"]));
        }
        let audio = text(&record["audio"]);
        let milliseconds = member["videoDurationMs"].as_i64().unwrap_or(0);
        let duration = if milliseconds > 0 {
            milliseconds as f64 / 1000.0
        } else {
            record["duration"].as_f64().unwrap_or(0.0)
        };
        if !duration.is_finite() || duration <= 0.0 || duration * 600.0 > u32::MAX as f64 {
            return Err(format!("invalid external video duration: {}", record["source"]));
        }
        let mut flags = if quicktime { member["videoFlags"].as_u64().unwrap_or(0) } else { 8 };
        if audio.is_empty() {
            flags &= !8;
        }
        let m = member.as_object_mut().ok_or("member")?;
        m.insert("type".into(), json!(10));
        if m.get("videoBoundsFromMedia") == Some(&json!(true)) {
            for axis in ["width", "height"] {
                let n = record[axis].as_u64().filter(|n| (1..=4096).contains(n))
                    .ok_or(format!("invalid external video {axis}"))?;
                m.insert(axis.into(), json!(n));
                m.insert(if axis == "width" {"regX"} else {"regY"}.into(), json!(n / 2));
            }
        }
        m.insert("sourceType".into(), json!(15));
        m.insert("asset".into(), record["video"].clone());
        m.insert("videoAudio".into(), json!(audio));
        m.insert("videoFlags".into(), json!(flags));
        m.insert("frames".into(), json!((duration * 600.0).round() as u32));
        m.insert("rate".into(), json!(600));
        m.insert("videoSource".into(), record["source"].clone());
        if let Some(fps) = record.get("videoFrameRate") {
            m.insert("videoFrameRate".into(), fps.clone());
        }
    }
    model["externalMediaUnbound"] = json!([]);
    model["externalMedia"] = Value::Array(fonts.external_media.clone());
    Ok(())
}

// ---- Measured metrics ----

/// A font's measured metrics (tools/convert/font_metrics.c): advances for
/// 32..=255, kerning, size, ascent and descent.
pub fn font_metrics(measured: &[u8]) -> R<Value> {
    let metrics: Value = serde_json::from_slice(measured).map_err(|e| format!("font metrics: {e}"))?;
    if metrics["advances"].as_array().map(Vec::len) != Some(224) {
        return Err("font metrics without 224 advances".into());
    }
    Ok(metrics)
}

/// The printable-ASCII part of measured metrics, for controller text entry:
/// advances for 32..=126, kerning among them, and the size.
pub fn ascii_metrics(measured: &[u8]) -> R<Value> {
    let metrics = font_metrics(measured)?;
    let advances = &metrics["advances"].as_array().unwrap()[..95];
    let kerning: Vec<&Value> = list(&metrics["kerning"])
        .filter(|k| k[0].as_i64() < Some(127) && k[1].as_i64() < Some(127))
        .collect();
    Ok(json!({"advances": advances, "kerning": kerning, "size": metrics["size"]}))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn substitute_metrics_follow_each_authored_size() {
        let spec=json!({"asset":"fonts/substitute.ttf","measure":true,
            "controller":{"size":24,"members":["entry"]}});
        let mut model=json!({"movies":[{"members":[
            {"name":"entry","sourceTextStyles":[{"sourceFontId":20,"size":24,"lineHeight":28,"ascent":22,"color":0}]},
            {"name":"certificate","sourceTextStyles":[{"sourceFontId":20,"size":36,"lineHeight":42,"ascent":33,"color":0}]}
        ]}]});
        let port=Port{convert:json!({"substitute_font":spec}),..Port::default()};
        let requests=plan(&port,&model).unwrap();
        let keys:Vec<&str>=list(&requests["measure"]).map(|v|text(&v["key"])).collect();
        assert_eq!(keys,vec!["substitute:24","substitute:36"]);
        let mut measured=Vec::new();
        substitute(&mut model,&spec,&PortFonts{droid_sans:Some(b"source-free transport".to_vec()),..PortFonts::default()},
            &mut |key|{
                measured.push(key.to_owned());
                let size=key.rsplit(':').next().unwrap().parse::<i32>().unwrap();
                Ok(serde_json::to_vec(&json!({"size":size,"advances":vec![size/2;224],"kerning":[]})).unwrap())
            }).unwrap();
        assert_eq!(measured,vec!["substitute:24","substitute:36"]);
        let members=&model["movies"][0]["members"];
        assert_eq!(members[0]["textStyle"]["controllerMetrics"]["advances"][0],12);
        assert_eq!(members[1]["textStyle"]["size"],36);
        assert_eq!(members[1]["textStyle"]["controllerMetrics"]["advances"][0],18);
    }

    #[test]
    fn substitute_retains_every_run_size_face_and_measured_request() {
        let spec=json!({"asset":"fonts/substitute.ttf","measure":true});
        let mut model=json!({"movies":[{"directorVersion":600,"name":"TEXT.DXR","members":[
            {"type":3,"cast":1,"number":1,"text":"AB","sourceTextStyles":[
             {"offset":0,"sourceFontId":20,"size":12,"face":1,"lineHeight":14,"ascent":10,"color":0},
             {"offset":1,"sourceFontId":21,"size":18,"face":10,"lineHeight":21,"ascent":16,"color":0x123456}]}]}]});
        let port=Port{convert:json!({"substitute_font":spec}),..Port::default()};
        let planned=plan(&port,&model).unwrap();
        assert_eq!(list(&planned["measure"]).map(|r|text(&r["key"])).collect::<Vec<_>>(),["substitute:12","substitute:18"]);
        substitute(&mut model,&spec,&PortFonts{droid_sans:Some(b"synthetic font transport".to_vec()),..PortFonts::default()},
            &mut |key|{let size=key.rsplit(':').next().unwrap().parse::<i32>().unwrap();
                Ok(serde_json::to_vec(&json!({"size":size,"advances":vec![size/2;224],"kerning":[]})).unwrap())}).unwrap();
        let runs=&model["movies"][0]["members"][0]["textFormats"];
        assert_eq!(runs[0]["fontId"],1);assert_eq!(runs[1]["fontId"],1);
        assert_eq!(runs[1]["size"],18);assert_eq!(runs[1]["face"],2);
        assert_eq!(model["fonts"][0]["variants"].as_array().unwrap().len(),2);
        assert_eq!(model["fonts"][0]["sourceFontIds"],json!([20,21]));
        assert_eq!(model["approximations"][0]["detail"],"unsupported character face effects");
    }

    #[test]
    fn substitute_keeps_zero_point_stxt_but_measures_its_effective_default() {
        let spec=json!({"asset":"fonts/substitute.ttf","measure":true});
        let mut model=json!({"movies":[{"directorVersion":600,"members":[
            {"type":3,"cast":1,"number":1,"text":"AB","sourceTextStyles":[
             {"offset":0,"sourceFontId":224,"size":0,"lineHeight":16,"ascent":12,"color":0},
             {"offset":1,"sourceFontId":224,"size":12,"lineHeight":16,"ascent":12,"color":0}]}]}]});
        let port=Port{convert:json!({"substitute_font":spec}),..Port::default()};
        let planned=plan(&port,&model).unwrap();
        assert_eq!(list(&planned["measure"]).map(|r|text(&r["key"])).collect::<Vec<_>>(),["substitute:12"]);
        substitute(&mut model,&spec,&PortFonts{droid_sans:Some(b"synthetic font transport".to_vec()),..PortFonts::default()},
            &mut |key|{
                assert_eq!(key,"substitute:12");
                Ok(serde_json::to_vec(&json!({"size":12,"advances":vec![6;224],"kerning":[]})).unwrap())
            }).unwrap();
        let member=&model["movies"][0]["members"][0];
        assert_eq!(member["sourceTextStyles"][0]["size"],0);
        assert_eq!(member["textFormats"][0]["size"],0);
        assert_eq!(member["textFormats"][1]["size"],12);
        assert_eq!(member["textStyle"]["size"],12);
        assert_eq!(member["textStyle"]["controllerMetrics"]["advances"][0],6);
        assert_eq!(model["fonts"][0]["variants"],json!([{"size":12,"asset":"fonts/f1-12.font64"}]));
    }

    #[test]
    fn source_pinned_text_trimming_keeps_insertion_format_inside_utf16_extent() {
        let mut model=json!({"movies":[{"name":"TEXT.DXR","members":[{"number":1,
          "text":"A😀BC","textFormats":[{"offset":0,"face":1},{"offset":5,"face":2}]}]}]});
        let port=Port{convert:json!({"text_fix":[{"movie":"TEXT.DXR","member":1,
          "text_sha256":sha256("A😀BC".as_bytes()),"trim":2}]}),..Port::default()};
        postprocess_model(&mut model,&port,&PortFonts::default(),&mut |_|Err("not measured".into())).unwrap();
        assert_eq!(model["movies"][0]["members"][0]["text"],"A😀");
        assert_eq!(model["movies"][0]["members"][0]["textFormats"][1]["offset"],3);
        assert_eq!(model["movies"][0]["members"][0]["textFormats"][1]["face"],2);
    }

    fn program() -> Value {
        json!({"files": [], "handlers": [
            {"movie": "A.DXR", "cast": "Internal", "member": 4, "name": "check", "line": 7,
             "parameters": ["sernum"], "source_sha256": "pinned",
             "body": [{"op": "call", "line": 8, "value": ["call", "getat", [["variable", "list"], ["variable", "a"]]]},
                      {"op": "call", "line": 9, "value": ["call", "getat", [["variable", "list"], ["variable", "b"]]]}]},
            {"movie": "B.DXR", "cast": "Internal", "member": 2, "name": "check", "line": 3,
             "parameters": ["sernum"], "source_sha256": "other", "body": []}]})
    }

    #[test]
    fn patches_replace_bodies_rename_calls_and_record_themselves() {
        let patched = patch_program(&program(), &json!([
            {"id": "rename", "movie": "A.DXR", "member": 4, "handler": "check",
             "source_sha256": "pinned", "rename_call": ["getat", "getaprop"], "count": 2},
            {"id": "answer", "movie": "B.DXR", "member": 2, "handler": "check", "body": "return sernum"}]))
        .unwrap();
        assert_eq!(patched["handlers"][0]["body"][1]["value"][1], "getaprop");
        assert_eq!(patched["handlers"][1]["body"][0]["op"], "return");
        assert_eq!(patched["compatibility_fixes"].as_array().unwrap().len(), 2);
        let all = patch_program(&program(), &json!([{"id": "all", "handler": "check", "all": true,
                                                     "body": "return 1"}]))
        .unwrap();
        assert_eq!(all["compatibility_fixes"].as_array().unwrap().len(), 2);
    }

    #[test]
    fn patches_refuse_changed_sources() {
        for patch in [
            json!({"movie": "A.DXR", "member": 4, "handler": "check", "source_sha256": "changed", "body": ""}),
            json!({"movie": "A.DXR", "member": 4, "handler": "check", "rename_call": ["getat", "x"], "count": 3}),
            json!({"movie": "A.DXR", "member": 5, "handler": "check", "body": ""}),
            json!({"handler": "check", "all": true, "calls": "glunew", "body": ""}),
            json!({"handler": "missing", "movie": "A.DXR", "member": 4, "body": ""}),
        ] {
            assert!(patch_program(&program(), &json!([patch])).is_err());
        }
    }

    #[test]
    fn tempo_fixes_are_pinned_and_repeatable() {
        let mut model = json!({"movies": [{"name": "FIL.DXR", "score": {"frames": [{"number": 33,
            "channels": [{"channel": 1, "tempoRaw": 30, "bytes": [0, 0, 0, 0, 0, 0, 30], "changed": [0, 0, 0, 0, 0, 0, 1]}]}]}}]});
        let fix = json!({"movie": "FIL.DXR", "frame": 33, "from": 30, "to": 0});
        tempo_fix(&mut model, &fix).unwrap();
        tempo_fix(&mut model, &fix).unwrap();
        assert_eq!(model["movies"][0]["score"]["frames"][0]["channels"][0]["tempoRaw"], 0);
        assert!(tempo_fix(&mut model, &json!({"movie": "FIL.DXR", "frame": 33, "from": 24, "to": 12})).is_err());
    }
}

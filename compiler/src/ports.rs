//! What a port adds to the converted game beyond the generic pipeline: the
//! embedded projector's launcher handlers, source-pinned compatibility
//! fixes to the recovered program, and each port's model corrections
//! (games/<slug>/host/compatibility.py and full_media.py, ported so the
//! browser importer applies them too). Every fix is pinned to the exact
//! source it was reviewed against and refuses anything else.

use serde_json::{json, Map, Value};

use crate::convert::source::sha256;
use crate::lingo;

pub type R<T> = Result<T, String>;

/// A port's settings from games/<slug>/game.toml [port].
#[derive(Clone, Debug, Default)]
pub struct Port {
    pub slug: String,
    pub source_sha256: String,
    pub launcher_movie: Option<String>,
    pub native_compatibility: bool,
}

/// The embedded projector's handlers (director64 recovery.report): each
/// launcher script, parsed with its cast, member and type.
pub fn launcher_handlers(source_manifest: &Value) -> R<Vec<Value>> {
    let launcher = &source_manifest["supplemental_projector"];
    let name = launcher["name"].as_str().ok_or("source manifest without a supplemental projector")?;
    let mut handlers = Vec::new();
    for script in launcher["scripts"].as_array().into_iter().flatten() {
        let member = launcher["members"]
            .as_array()
            .into_iter()
            .flatten()
            .rfind(|m| m["id"] == script["member_id"])
            .ok_or("launcher script without its member")?;
        let cast = launcher["libraries"]
            .as_array()
            .into_iter()
            .flatten()
            .rfind(|c| c["library_id"] == member["library_id"])
            .and_then(|c| c["name"].as_str())
            .ok_or("launcher member without its cast")?;
        let text = format!(
            "-- cast: {cast}; member: {}; type: {}; name: launcher\n{}",
            member["member_id"],
            script["script_type"].as_str().unwrap_or(""),
            script["lingo"].as_str().unwrap_or("")
        );
        handlers.extend(lingo::parse_movie(&text, name).map_err(|e| e.to_string())?);
    }
    Ok(handlers)
}

/// The program the runtime executes (director64 aot.executable_program): the
/// recovered program with the port's compatibility fixes, then the
/// launcher's handlers under the launcher movie's name.
pub fn executable_program(program: &Value, port: &Port, source_manifest: &Value) -> R<Value> {
    let mut program = program.clone();
    if port.native_compatibility {
        program = compatibility(&program, port)?;
    }
    if let Some(movie) = &port.launcher_movie {
        let handlers = program["handlers"].as_array_mut().ok_or("program without handlers")?;
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
    Ok(program)
}

fn compatibility(program: &Value, port: &Port) -> R<Value> {
    match port.slug.as_str() {
        "willy-werkel-cars" => willy::compatibility(program, &port.source_sha256),
        "findus-mucklas" => mucklas_compatibility(program, &port.source_sha256),
        "lernerfolg-deutsch-1-2" => deutsch_compatibility(program, &port.source_sha256),
        "loewenzahn-1" => loewenzahn::compatibility(program, &port.source_sha256),
        other => Err(format!("{other}: its compatibility fixes are not in the converter yet")),
    }
}

/// games/findus-mucklas/host/compatibility.py: BR's keyed spacing table at
/// its two authored symbolic getAt sites takes getaProp. Bytecode extcall
/// offsets 546/579 confirm getAt was authored; ScummVM's getAt accepts
/// numeric indices only. A selected-port correction, not a claim that
/// Director generally accepts symbolic getAt arguments.
fn mucklas_compatibility(program: &Value, source: &str) -> R<Value> {
    const SOURCE: &str = "ad4c7afb4b19339c45c2a69756b522b59ba4869199740fd528527f190a806925";
    const SCRIPT: &str = "d2db226e23a14304b8953395fdbe7684f6109f2bdd386609d3db07ce1d49e906";
    let mut result = program.clone();
    let mut matches: Vec<&mut Value> = find_handlers(&mut result, "BR.DXR", 4, "ritabana")
        .into_iter()
        .filter(|h| h["cast"] == "Internal")
        .collect();
    if source != SOURCE || matches.len() != 1 || matches[0]["source_sha256"] != SCRIPT {
        return Err("Mucklas BR compatibility source identity changed; review required".into());
    }
    fn replace(node: &mut Value, count: &mut u32) {
        let target = json!(["call", "getat", [["variable", "avstlist"], ["variable", "a"]]]);
        if *node == target {
            node[1] = json!("getaprop");
            *count += 1;
        } else if let Some(object) = node.as_object_mut() {
            object.values_mut().for_each(|v| replace(v, count));
        } else if let Some(array) = node.as_array_mut() {
            array.iter_mut().for_each(|v| replace(v, count));
        }
    }
    let mut count = 0;
    replace(&mut matches[0]["body"], &mut count);
    if count != 2 {
        return Err("Mucklas BR compatibility call sites changed; review required".into());
    }
    result.as_object_mut().ok_or("program")?.insert(
        "compatibility_fixes".into(),
        json!([{"id": "br-symbolic-spacing", "movie": "BR.DXR", "member": 4, "handler": "ritabana",
                "source_sha256": SCRIPT, "bytecode_offsets": [546, 579], "calls_changed": count,
                "from": "getat", "to": "getaprop", "original_projector_verified": false}]),
    );
    Ok(result)
}

/// games/lernerfolg-deutsch-1-2/host/compatibility.py: the two MX 2004
/// JavaScript-dialect handlers, recovered from their compiled payloads,
/// lower to the runtime's js_* natives (int2hex's lowercase hex; clearGarbage's
/// collection is automatic).
fn deutsch_compatibility(program: &Value, source: &str) -> R<Value> {
    const SOURCE: &str = "4c87e10dcc99dfb68882b51c9daceb7ce4761b482a2a43bf620093289353e0f4";
    const INT2HEX: &str = "400f1d944fee13df345a3ccb97a597363c5edde995cd689359145e1e8185abac";
    const CLEARGARBAGE: &str = "515abd3ad032882fdf3ff3d1d8e2468de62f28da773fa5cec0ae1b127a475822";
    if source != SOURCE {
        return Err("Deutsch compatibility source changed".into());
    }
    let specs = [
        ("javascript-int2hex", "TASKSCRIPTS.CXT", 9, "int2hex", INT2HEX, json!(["num"]),
         json!([{"op": "return", "line": 30, "value": ["call", "js_int2hex", [["variable", "num"]]]}])),
        ("javascript-cleargarbage", "ZMSCRIPTS.CXT", 58, "cleargarbage", CLEARGARBAGE, json!([]),
         json!([{"op": "call", "line": 7405, "value": ["call", "js_cleargarbage", []]}])),
    ];
    let mut result = program.clone();
    let mut fixes = Vec::new();
    for (id, movie, member, name, sha, parameters, body) in specs {
        let mut matches = find_handlers(&mut result, movie, member, name);
        if matches.len() != 1 || matches[0]["source_sha256"] != sha {
            return Err(format!("Deutsch {name} source changed"));
        }
        let handler = &mut matches[0];
        let old = handler["body"].as_array().cloned().unwrap_or_default();
        if old.len() != 1 || old[0].get("op") != Some(&json!("unrecovered")) || old[0].get("opcode") != Some(&json!("unk26")) {
            return Err(format!("Deutsch {name} bytecode shape changed"));
        }
        if handler["parameters"].as_array().is_some_and(|p| !p.is_empty()) {
            return Err(format!("Deutsch {name} parameters changed"));
        }
        handler["parameters"] = parameters;
        handler["body"] = body;
        fixes.push(json!({"id": id, "movie": movie, "member": member, "handler": name, "source_sha256": sha,
                          "disposition": "recovered-javascript-function", "original_projector_verified": false}));
    }
    result.as_object_mut().ok_or("program")?.insert("compatibility_fixes".into(), Value::Array(fixes));
    Ok(result)
}

/// The port's model corrections after conversion. `measure(key)` returns the
/// font64 mkfont packed for one of plan()'s measurement requests, as the
/// port hosts' text_metrics measure it.
pub fn postprocess_model(
    model: &mut Value,
    port: &Port,
    fonts: &PortFonts,
    measure: &mut dyn FnMut(&str) -> R<Vec<u8>>,
) -> R<()> {
    match port.slug.as_str() {
        "willy-werkel-cars" => willy::media(model, fonts, measure),
        "findus-christmas" => christmas_media(model, fonts),
        "lernerfolg-deutsch-1-2" => deutsch_media(model, fonts, measure),
        "loewenzahn-1" => loewenzahn::media(model, fonts),
        _ => Ok(()),
    }
}

/// What the importer does for a port around postprocess_model: whether the
/// model changes at all; the mkfont runs it measures first, each
/// {key, font ("substitute" or a converted font asset), size, ranges}, packed
/// with `--size SIZE --range R... -c 0`; the disc folders whose files the
/// runtime streams as sound (playFile) or reads as data (read_data).
pub fn plan(slug: &str, model: &Value) -> R<Value> {
    let (changes, measure, streams, data) = match slug {
        "willy-werkel-cars" => (
            true,
            vec![json!({"key": "substitute:24", "font": "substitute", "size": 24, "ranges": ["0x20-0x7e"]})],
            vec![],
            vec![],
        ),
        "findus-christmas" => (true, vec![], vec![], vec![]),
        "lernerfolg-deutsch-1-2" => (true, deutsch_measure(model)?, vec!["voc", "sfx"], vec!["tests_db"]),
        "loewenzahn-1" => (true, vec![], vec![], vec![]),
        _ => (false, vec![], vec![], vec![]),
    };
    let mut plan = json!({"model": changes, "measure": measure, "streams": streams, "data": data});
    if slug == "loewenzahn-1" {
        // Linked QuickTime movies and external AIFF sounds under the media
        // root, converted by the host before the model step; and the print
        // documents' artwork folder.
        plan["external_media"] = json!({"root": "MEDIA", "extensions": ["MOV", "AIF"]});
        plan["print"] = json!("MEDIA/PRINT");
    }
    Ok(plan)
}

/// Fonts a port supplies itself: a pinned substitute for unembedded system
/// fonts, and the file its model names for it.
#[derive(Default)]
pub struct PortFonts {
    pub droid_sans: Option<Vec<u8>>,
    /// How many data files the port ships (plan's `data` folders).
    pub data_files: usize,
    /// The port's converted external media (plan's `external_media`), one
    /// record per source file: {source, source_sha256, source_bytes,
    /// duration, video, audio[, frames, rate, channels]}.
    pub external_media: Vec<Value>,
}

/// The substitute font's asset name in the converted model, if the port has one.
pub fn substitute_asset(slug: &str) -> Option<&'static str> {
    match slug {
        "willy-werkel-cars" => Some("fonts/willy-system.ttf"),
        "findus-christmas" => Some("fonts/christmas-system.ttf"),
        "loewenzahn-1" => Some("fonts/d5-system.ttf"),
        _ => None,
    }
}

fn find_handlers<'a>(program: &'a mut Value, movie: &str, member: i64, name: &str) -> Vec<&'a mut Value> {
    program["handlers"]
        .as_array_mut()
        .into_iter()
        .flatten()
        .filter(|h| h["movie"] == movie && h["member"] == member && h["name"] == name)
        .collect()
}

fn text_sha(member: &Value) -> String {
    sha256(member["text"].as_str().unwrap_or("").as_bytes())
}

/// Python json.dumps's ordering for the fixes list and approximations.
fn object(pairs: Vec<(&str, Value)>) -> Value {
    Value::Object(pairs.into_iter().map(|(k, v)| (k.to_string(), v)).collect::<Map<_, _>>())
}

mod willy {
    use super::*;

    const SOURCE: &str = "938be20a50e2825f5acf4788aabfdc145b94d1a1fb61b7264a4a81156428afac";
    const WINDOW: &str = "9cb4f829e9e23a5271fd9568ca2e71bd5f0eee9d18368bc95c1437a710e7ec0d";
    const OPENER: &str = "b8cdef14b9e36e5fb60da419e8af19c7fc6841c965fa318a7eda2bb109224500";
    const PRINT: &str = "408f357aa657c52dfd018fbd3759b070f44a55ba66a9dbb86ae33d2677d388ac";
    const TEMPLATE: &str = "ac7f41cbf12bfe707ef1d0f284ebfd363e1a1272abe9a5ebe2ac69788287bb36";
    const CHARTS: [(i64, &str); 2] = [
        (170, "4926eca66bc61a7a708afbf2bcc2780c553da3005e6af0dcb8e485fb69daeaf1"),
        (173, "2b3f97edade776b90c18901c4a1fdd4bf583bc5723519a00b371eed46f1deb1c"),
    ];

    /// games/willy-werkel-cars/host/compatibility.py
    pub fn compatibility(program: &Value, source: &str) -> R<Value> {
        if source != SOURCE {
            return Err("Willy compatibility source changed".into());
        }
        let mut result = program.clone();
        {
            let mut handlers = find_handlers(&mut result, "06.DXR", 2, "closewindow");
            if handlers.len() != 1 || handlers[0]["source_sha256"] != WINDOW {
                return Err("Willy external chooser source changed".into());
            }
            let body = handlers[0]["body"]
                .as_array_mut()
                .and_then(|b| b.iter_mut().find(|n| n["op"] == "if"))
                .and_then(|n| n["yes"].as_array_mut())
                .ok_or("Willy external chooser context changed")?;
            let tell = body.first().cloned().unwrap_or(Value::Null);
            if tell["op"] != "tell" || tell["target"] != json!(["variable", "mymiaw"]) {
                return Err("Willy external chooser context changed".into());
            }
            // A fail-closed native trap for external PC file selection.
            let mut replacement =
                vec![json!({"op": "call", "line": tell["line"], "value": ["call", "tell_window", [tell["target"]]]})];
            replacement.extend(tell["body"].as_array().cloned().unwrap_or_default());
            body.splice(0..1, replacement);
        }
        {
            let mut openers = find_handlers(&mut result, "06.DXR", 2, "openwindow");
            if openers.len() != 1 || openers[0]["source_sha256"] != OPENER {
                return Err("Willy external chooser opener changed".into());
            }
            let line = openers[0]["body"][1]["line"].clone();
            openers[0]["body"] = json!([{"op": "call", "line": line, "value": ["call", "open_window_trap", []]}]);
        }
        {
            let mut printers = find_handlers(&mut result, "08.DXR", 38, "print");
            if printers.len() != 1 || printers[0]["source_sha256"] != PRINT {
                return Err("Willy certificate print source changed".into());
            }
            let line = printers[0]["body"][0]["line"].clone();
            printers[0]["body"] = json!([{"op": "call", "line": line, "value": ["call", "certificate_print_notice", []]}]);
        }
        result.as_object_mut().ok_or("program")?.insert(
            "compatibility_fixes".into(),
            json!([
                {"id": "certificate-print-notice", "movie": "08.DXR", "member": 38, "handler": "print",
                 "source_sha256": PRINT, "disposition": "dismissible-platform-notice",
                 "original_projector_verified": false},
                {"id": "external-file-chooser-trap", "movie": "06.DXR", "member": 2, "handler": "closewindow",
                 "source_sha256": WINDOW, "disposition": "explicit-runtime-trap",
                 "original_projector_verified": false},
                {"id": "external-file-chooser-opener-trap", "movie": "06.DXR", "member": 2,
                 "handler": "openwindow", "source_sha256": OPENER,
                 "disposition": "recoverable-runtime-trap", "original_projector_verified": false},
            ]),
        );
        Ok(result)
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

    /// games/willy-werkel-cars/host/full_media.py: Droid Sans for the
    /// unembedded system fonts, measured controller fields, and two
    /// source-data corrections.
    pub fn media(model: &mut Value, fonts: &PortFonts, measure: &mut dyn FnMut(&str) -> R<Vec<u8>>) -> R<()> {
        let source = fonts.droid_sans.as_ref().ok_or("Willy needs the Droid Sans substitute")?;
        let asset = "fonts/willy-system.ttf";
        // These fields take controller text entry and source width checks.
        let controller_metrics = font64_ascii_metrics(&measure("substitute:24")?)?;
        let used = substitute_styles(model, |name, size| {
            ((name == "SavedCarName" || name == "EnterField") && size == 24).then(|| controller_metrics.clone())
        })?;
        let template = member_mut(model, "CDDATA.CXT", 1)?;
        if text_sha(template) != TEMPLATE {
            return Err("new-player template source changed".into());
        }
        let mut text = template["text"].as_str().unwrap_or("").to_string();
        text.pop();
        template["text"] = json!(text);
        template["compatibility"] = object(vec![
            ("id", json!("new-player-template-unmatched-bracket")),
            ("source_text_sha256", json!(TEMPLATE)),
            ("change", json!("remove the final unmatched closing bracket")),
            ("original_projector_verified", json!(false)),
        ]);
        for (number, expected) in CHARTS {
            let member = member_mut(model, "00.CXT", number)?;
            if text_sha(member) != expected {
                return Err("animation chart source changed".into());
            }
            let mut text = member["text"].as_str().unwrap_or("").to_string();
            for _ in 0..3 {
                text.pop();
            }
            member["text"] = json!(text);
            member["compatibility"] = object(vec![
                ("id", json!("animation-chart-trailing-digits")),
                ("source_text_sha256", json!(expected)),
                ("change", json!("remove 333 after the closed property list")),
                ("original_projector_verified", json!(false)),
            ]);
        }
        let mut notes = Vec::new();
        for f in model["movies"].as_array().into_iter().flatten() {
            for m in f["members"].as_array().into_iter().flatten() {
                if let Some(fix) = m.get("compatibility").and_then(Value::as_object) {
                    let mut note = object(vec![
                        ("kind", json!("source-data-correction")),
                        ("movie", f["name"].clone()),
                        ("member", m["number"].clone()),
                    ]);
                    for (k, v) in fix {
                        note[k] = v.clone();
                    }
                    notes.push(note);
                }
            }
        }
        let approximations = model["approximations"].as_array_mut().ok_or("model without approximations")?;
        approximations.extend(notes);
        substitute_font(model, asset, source, used);
        Ok(())
    }
}

/// The system-font substitute the ports' full_media.py hosts apply: every
/// member with source text styles takes Droid Sans (font 1) at its first
/// run's size and metrics. `controller(name, size)` adds measured metrics to
/// fields that take controller entry. Returns the sizes and source font ids
/// in use.
struct Substituted {
    sizes: std::collections::BTreeSet<i64>,
    source_fonts: std::collections::BTreeSet<i64>,
}
fn substitute_styles(model: &mut Value, controller: impl Fn(&str, i64) -> Option<Value>) -> R<Substituted> {
    let mut used = Substituted { sizes: Default::default(), source_fonts: Default::default() };
    for movie in model["movies"].as_array_mut().into_iter().flatten() {
        for member in movie["members"].as_array_mut().into_iter().flatten() {
            let Some(style) = member.get("sourceTextStyles").and_then(Value::as_array).and_then(|s| s.first()).cloned() else {
                continue;
            };
            used.source_fonts.insert(style["sourceFontId"].as_i64().unwrap_or(0));
            let size = style["size"].as_i64().unwrap_or(0);
            used.sizes.insert(size);
            let align = match member.get("textAlign").and_then(Value::as_i64).unwrap_or(0) {
                -1 => 2,
                1 => 1,
                _ => 0,
            };
            let line_height = style["lineHeight"].as_i64().unwrap_or(0);
            let ascent = style["ascent"].as_i64().unwrap_or(0);
            let mut text_style = object(vec![
                ("fontName", json!(format!("Director system font {}", style["sourceFontId"]))),
                ("fontId", json!(1)),
                ("size", style["size"].clone()),
                ("align", json!(align)),
                ("ascent", style["ascent"].clone()),
                ("descent", json!((line_height - ascent).max(0))),
                ("leading", json!(0)),
                ("lineHeight", json!(line_height.max(1))),
                ("color", style["color"].clone()),
            ]);
            if let Some(metrics) = controller(member["name"].as_str().unwrap_or(""), size) {
                text_style["controllerMetrics"] = metrics;
            }
            let m = member.as_object_mut().ok_or("member")?;
            m.insert("textStyle".into(), text_style);
            m.insert("textPresentation".into(), json!("explicit-system-font-substitute"));
        }
    }
    Ok(used)
}

/// The substitute's font entry, one font64 variant per size, and its note.
fn substitute_font(model: &mut Value, asset: &str, source: &[u8], used: Substituted) {
    model["fonts"] = json!([object(vec![
        ("number", json!(1)),
        ("asset", json!(asset)),
        ("sha256", json!(sha256(source))),
        ("kind", json!("system-font-substitute")),
        ("sourceFontIds", json!(used.source_fonts)),
        ("targetFont", json!("Droid Sans")),
        ("license", json!("Apache-2.0")),
        ("provenance", json!("third_party/libdragon/examples/fontgallery/assets/droid-sans.ttf")),
        ("variants", Value::Array(used.sizes.iter().map(|s| json!({"size": s, "asset": format!("fonts/f1-{s}.font64")})).collect())),
    ])]);
    if let Some(approximations) = model["approximations"].as_array_mut() {
        approximations.push(object(vec![
            ("kind", json!("system-font-substitute")),
            (
                "description",
                json!("Source STXT metrics and Mac Roman text use regular Droid Sans; mixed fonts and faces use the first source run."),
            ),
            ("implemented", json!(true)),
        ]));
    }
}

/// games/findus-christmas/host/full_media.py: the substitute alone.
fn christmas_media(model: &mut Value, fonts: &PortFonts) -> R<()> {
    let source = fonts.droid_sans.as_ref().ok_or("Christmas needs the Droid Sans substitute")?;
    let used = substitute_styles(model, |_, _| None)?;
    substitute_font(model, "fonts/christmas-system.ttf", source, used);
    Ok(())
}

/// The (font, size) variants whose advances Lernerfolg's layout reads
/// (games/lernerfolg-deutsch-1-2/host/full_media.py font_metrics): every
/// text style's, and every Flash edit field's by its font alias, measured
/// over the font's own codepoints in 32..=255 as dense runs.
fn deutsch_measure(model: &Value) -> R<Vec<Value>> {
    let fonts: std::collections::BTreeMap<i64, &Value> = model["fonts"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|f| f["number"].as_i64().map(|n| (n, f)))
        .collect();
    let mut aliases = std::collections::HashMap::new();
    for (&number, font) in &fonts {
        for alias in font["aliases"].as_array().into_iter().flatten().filter_map(Value::as_str) {
            aliases.insert(alias.to_lowercase(), number);
        }
    }
    let mut pairs = std::collections::BTreeSet::new();
    for movie in model["movies"].as_array().into_iter().flatten() {
        for member in movie["members"].as_array().into_iter().flatten() {
            for key in ["textStyle", "textInsertStyle"] {
                let style = &member[key];
                if let (Some(font), Some(size)) = (style["fontId"].as_i64(), style["size"].as_i64())
                    && font != 0
                {
                    pairs.insert((font, size));
                }
            }
            for field in member["flashFields"].as_array().into_iter().flatten() {
                let name = field["fontName"].as_str().unwrap_or("").to_lowercase();
                if let Some(&number) = aliases.get(&name) {
                    // Python's round: half to even.
                    let height = field["fontHeight"].as_f64().filter(|h| *h != 0.0).unwrap_or(12.0);
                    pairs.insert((number, (height.round_ties_even() as i64).max(1)));
                }
            }
        }
    }
    pairs
        .into_iter()
        .map(|(number, size)| {
            let font = fonts.get(&number).ok_or(format!("font {number} not in the model"))?;
            let codepoints: Vec<i64> = font["codepoints"].as_array().into_iter().flatten().filter_map(Value::as_i64).collect();
            Ok(json!({"key": format!("{number}:{size}"), "font": font["asset"], "size": size,
                      "ranges": metric_ranges(&codepoints)?}))
        })
        .collect()
}

/// text_metrics._ranges: contiguous --range arguments over a font's own
/// codepoints within 32..=255.
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

/// games/lernerfolg-deutsch-1-2/host/full_media.py, the model half: the
/// measured advance tables and the port's notes. The speech streams and task
/// databases are files, not model entries; the importer ships them as packs.
fn deutsch_media(model: &mut Value, fonts: &PortFonts, measure: &mut dyn FnMut(&str) -> R<Vec<u8>>) -> R<()> {
    let mut metrics = Map::new();
    for request in deutsch_measure(model)? {
        let key = request["key"].as_str().unwrap_or("").to_string();
        let packed = measure(&key)?;
        metrics.insert(key, font64_metrics(&packed)?);
    }
    let count = metrics.len();
    model["fontMetrics"] = Value::Object(metrics);
    let notes = [
        object(vec![
            ("kind", json!("external-speech-ulc-stream")),
            ("description", json!("voc/ and sfx/ files stream as ULC wav64 through sound playFile; the codec is selected by measured size against VADPCM and Opus.")),
            ("implemented", json!(true)),
        ]),
        object(vec![
            ("kind", json!("exercise-task-databases")),
            ("description", json!(format!("{} tests_db/ task databases ship verbatim and are read through baReadBinFile; the authored Lingo parses them unchanged.", fonts.data_files))),
            ("implemented", json!(true)),
        ]),
        object(vec![
            ("kind", json!("measured-text-metrics")),
            ("description", json!(format!("{count} authored font variants carry mkfont-measured advance tables for charPosOf and Flash prompt layout."))),
            ("implemented", json!(true)),
        ]),
    ];
    let approximations = model["approximations"].as_array_mut().ok_or("model without approximations")?;
    for note in notes {
        if !approximations.contains(&note) {
            approximations.push(note);
        }
    }
    Ok(())
}

/// text_metrics.decode: advances for 32..=255 (0 where the font has no
/// glyph), kerning among them, point size, ascent and descent.
pub fn font64_metrics(font: &[u8]) -> R<Value> {
    if font.len() < 100 || &font[..4] != b"FNT\x0b" {
        return Err("unsupported font64 metrics header".into());
    }
    let u32_at = |at: usize| -> R<usize> {
        font.get(at..at + 4).map(|b| u32::from_be_bytes(b.try_into().unwrap()) as usize).ok_or_else(|| "truncated font64".to_string())
    };
    let byte = |at: usize| font.get(at).copied().ok_or_else(|| "truncated font64".to_string());
    let (ranges, glyphs, kranges, kern) = (u32_at(72)?, u32_at(80)?, u32_at(84)?, u32_at(92)?);
    // Python dict semantics: first insertion fixes the order, later ranges
    // overwrite the glyph.
    let mut order: Vec<i64> = Vec::new();
    let mut indices = std::collections::HashMap::new();
    for i in 0..u32_at(36)? {
        let at = ranges + i * 12;
        let (start, count, glyph) = (u32_at(at)? as i64, u32_at(at + 4)? as i64, u32_at(at + 8)? as i32 as i64);
        if glyph < 0 {
            return Err("sparse metrics font range".into());
        }
        for c in start.max(32)..(start + count).min(256) {
            if indices.insert(c, glyph + c - start).is_none() {
                order.push(c);
            }
        }
    }
    if !(0x20..0x7f).all(|c| indices.contains_key(&c)) {
        return Err("metrics font lacks printable ASCII".into());
    }
    let advances = (32..=255)
        .map(|c| indices.get(&c).map_or(Ok(0), |&g| byte(glyphs + g as usize * 8)))
        .collect::<R<Vec<u8>>>()?;
    let mut reverse = std::collections::HashMap::new();
    for &c in &order {
        reverse.insert(indices[&c], c);
    }
    let mut pairs = Vec::new();
    if kranges != 0 {
        for &c in &order {
            let g = indices[&c] as usize;
            let at = kranges + g * 4;
            let lo = u16::from_be_bytes([byte(at)?, byte(at + 1)?]) as usize;
            let hi = u16::from_be_bytes([byte(at + 2)?, byte(at + 3)?]) as usize;
            if lo == 0 {
                continue;
            }
            for i in lo..=hi {
                let at = kern + i * 3;
                let second = i16::from_be_bytes([byte(at)?, byte(at + 1)?]) as i64;
                let amount = byte(at + 2)? as i8 as i64;
                if let Some(&other) = reverse.get(&second) {
                    pairs.push(json!([c, other, amount]));
                }
            }
        }
    }
    let i32_at = |at: usize| -> R<i64> { Ok(u32_at(at)? as u32 as i32 as i64) };
    Ok(object(vec![
        ("advances", json!(advances)),
        ("kerning", Value::Array(pairs)),
        ("size", json!(u32_at(8)?)),
        ("ascent", json!(i32_at(12)?)),
        ("descent", json!(-i32_at(16)?)),
    ]))
}

mod loewenzahn {
    use super::*;

    const SOURCE: &str = "f31970b980f096a343a9aec5f10d52931709fbdda3cf09e5a80023791e402af2";
    const PRINT: &str = "0687160c839bd70d5c6a07c1ddb955f06de6b93c23bc24f2b499f8bcf9724901";

    /// games/loewenzahn-1/host/compatibility.py: the print handler hands its
    /// book and chapter to the platform's fixed-document printing.
    pub fn compatibility(program: &Value, source: &str) -> R<Value> {
        let mut result = program.clone();
        {
            let mut matches: Vec<&mut Value> = find_handlers(&mut result, "CURSOR.CXT", 90, "drucken")
                .into_iter()
                .filter(|h| h["cast"] == "External")
                .collect();
            if source != SOURCE || matches.len() != 1 || matches[0]["source_sha256"] != PRINT {
                return Err("Löwenzahn print source identity changed; review required".into());
            }
            let handler = &mut matches[0];
            if handler["parameters"] != json!(["me", "logobreite"]) {
                return Err("Löwenzahn print parameters changed".into());
            }
            let line = handler["line"].clone();
            handler["body"] = json!([{"op": "call", "line": line,
                "value": ["call", "director64_print", [["variable", "mbuch"], ["variable", "mkapitel"]]]}]);
        }
        result.as_object_mut().ok_or("program")?.insert(
            "compatibility_fixes".into(),
            json!([{"id": "phone-printing", "movie": "CURSOR.CXT", "member": 90, "handler": "drucken",
                    "source_sha256": PRINT, "from": "PrintOMatic page composition",
                    "to": "fixed PDF QR handoff", "original_projector_verified": false}]),
        );
        Ok(result)
    }

    fn base_name(path: &str) -> &str {
        path.rsplit('/').next().unwrap_or(path)
    }

    /// games/loewenzahn-1/host/full_media.py, the model half: linked videos
    /// and external sounds from the host's converted records, the Geneva and
    /// Monaco substitute, the target limits and FIL's speed correction.
    pub fn media(model: &mut Value, fonts: &PortFonts) -> R<()> {
        if model["problems"].as_array().into_iter().flatten().any(|p| p["error"] != "digital video requires target conversion") {
            return Err("unresolved non-video resources".into());
        }
        let mut records = fonts.external_media.clone();
        // pathlib order: by path components.
        records.sort_by(|a, b| {
            let parts = |v: &Value| v["source"].as_str().unwrap_or("").split('/').map(str::to_string).collect::<Vec<_>>();
            parts(a).cmp(&parts(b))
        });
        let count = |ext: &str| records.iter().filter(|r| r["source"].as_str().unwrap_or("").ends_with(ext)).count();
        if count(".MOV") != 66 || count(".AIF") != 219 {
            return Err("external media denominator changed".into());
        }
        let mut by_name = std::collections::HashMap::new();
        for record in &records {
            let name = base_name(record["source"].as_str().unwrap_or("")).to_lowercase();
            if by_name.insert(name, record.clone()).is_some() {
                return Err("ambiguous external filename".into());
            }
        }
        for movie in model["movies"].as_array_mut().into_iter().flatten() {
            for member in movie["members"].as_array_mut().into_iter().flatten() {
                if member["type"] != 10 {
                    continue;
                }
                let linked = member["linkedFile"].as_str().unwrap_or("").to_lowercase();
                let record = by_name.get(&linked).ok_or(format!("unresolved linked video: {}", member["linkedFile"]))?;
                let flags = member["videoFlags"].as_i64().unwrap_or(0);
                let duration = record["duration"].as_f64().unwrap_or(0.0);
                let m = member.as_object_mut().ok_or("member")?;
                m.insert("asset".into(), record["video"].clone());
                m.insert("videoAudio".into(), record["audio"].clone());
                m.insert("frames".into(), json!((duration * 600.0).round_ties_even() as i64));
                m.insert("rate".into(), json!(600));
                m.insert("videoSource".into(), record["source"].clone());
                m.insert("videoFrameRate".into(), json!(15));
                m.insert("videoLoop".into(), json!(flags & 16 != 0));
                m.insert("videoPaused".into(), json!(flags & 256 != 0));
            }
        }
        let shared = model["movies"]
            .as_array_mut()
            .into_iter()
            .flatten()
            .find(|m| m["name"] == "CURSOR.CXT")
            .ok_or("CURSOR.CXT missing")?;
        let members = shared["members"].as_array_mut().ok_or("CURSOR.CXT members")?;
        members.retain(|m| !m.get("externalAudio").is_some_and(|v| !v.is_null() && v != "" && v != &json!(false)));
        for (n, record) in records.iter().filter(|r| r["source"].as_str().unwrap_or("").ends_with(".AIF")).enumerate() {
            members.push(object(vec![
                ("number", json!(1000 + n)),
                ("cast", json!(1)),
                ("type", json!(6)),
                ("name", json!(base_name(record["source"].as_str().unwrap_or("")))),
                ("asset", record["audio"].clone()),
                ("rate", record["rate"].clone()),
                ("frames", record["frames"].clone()),
                ("channels", record["channels"].clone()),
                ("loopStart", json!(0)),
                ("loopEnd", record["frames"].clone()),
                ("looping", json!(0)),
                ("externalAudio", record["source"].clone()),
            ]));
        }
        model["problems"] = json!([]);
        model["externalMedia"] = Value::Array(records);
        // Geneva is a source system font, absent from the disc: a deliberate
        // substitute from the pinned SDK, keeping the source STXT metrics.
        let source = fonts.droid_sans.as_ref().ok_or("Löwenzahn needs the Droid Sans substitute")?;
        let mut sizes: std::collections::BTreeSet<i64> = [18, 24].into();
        for movie in model["movies"].as_array_mut().into_iter().flatten() {
            for member in movie["members"].as_array_mut().into_iter().flatten() {
                let Some(styles) = member.get("sourceTextStyles").and_then(Value::as_array).cloned() else {
                    continue;
                };
                let style = styles.first().cloned().unwrap_or(Value::Null);
                let font = style["sourceFontId"].as_i64().unwrap_or(0);
                if styles.len() != 1 || !(font == 3 || font == 4) || !(style["face"] == 0 || style["face"] == 1) {
                    return Err("unhandled source STXT font/style".into());
                }
                let size = style["size"].as_i64().unwrap_or(0);
                if !(1..=255).contains(&size) {
                    return Err("invalid source font size".into());
                }
                sizes.insert(size);
                let ascent = style["ascent"].as_i64().unwrap_or(0);
                let line_height = style["lineHeight"].as_i64().unwrap_or(0);
                let m = member.as_object_mut().ok_or("member")?;
                m.insert("textStyle".into(), object(vec![
                    ("fontName", json!(if font == 3 { "Geneva" } else { "Monaco" })),
                    ("fontId", json!(1)),
                    ("size", style["size"].clone()),
                    ("align", json!(0)),
                    ("ascent", style["ascent"].clone()),
                    ("descent", json!(line_height - ascent)),
                    ("leading", json!(0)),
                    ("lineHeight", style["lineHeight"].clone()),
                    ("color", style["color"].clone()),
                ]));
                m.insert("textPresentation".into(), json!("explicit-system-font-substitute"));
            }
        }
        model["fonts"] = json!([object(vec![
            ("number", json!(1)),
            ("asset", json!("fonts/d5-system.ttf")),
            ("sha256", json!(sha256(source))),
            ("kind", json!("system-font-substitute")),
            ("sourceFonts", json!(["Geneva", "Monaco"])),
            ("targetFont", json!("Droid Sans")),
            ("license", json!("Apache-2.0")),
            ("provenance", json!("third_party/libdragon/examples/fontgallery/assets/droid-sans.ttf")),
            ("variants", Value::Array(sizes.iter().map(|s| json!({"size": s, "asset": format!("fonts/f1-{s}.font64")})).collect())),
        ])]);
        let limits = [
            ("system-font-substitute", "Geneva and Monaco are not embedded; regular Droid Sans renders Mac Roman STXT as UTF-8 with word wrapping. The single bold source style is rendered regular."),
            ("builtin-cursor-glyphs", "Standard OS cursor roles use target glyphs. Authored monochrome bitmap/mask cursors retain their original artwork and registration points."),
            ("transition-cut", "Authored transitions use cuts; source score timing and navigation are retained."),
            ("movie-window", "One centered 416x240 source DIALOG window with suspended stage state."),
            ("phone-printing", "The print stamp opens a QR link to a hosted PDF. Documents use recovered text and original print artwork, A4 flow layout and Liberation Sans."),
        ];
        let approximations = model["approximations"].as_array_mut().ok_or("model without approximations")?;
        approximations.retain(|a| {
            let kind = a["kind"].as_str().unwrap_or("");
            !limits.iter().any(|(k, _)| *k == kind) && kind != "video-conversion" && kind != "cursor-substitute"
        });
        for (kind, detail) in limits {
            approximations.push(json!({"kind": kind, "detail": detail}));
        }
        approximations.push(json!({"kind": "video-conversion", "codec": "h264", "width": 160, "fps": 15,
            "quality": 25, "seek_seconds": 2, "audio": "source rates/channels; ULC packing"}));
        correct_film_speed(model)
    }

    #[cfg(test)]
    mod tests {
        use super::*;

        fn film_score() -> Value {
            let mut bytes = vec![0; 24];
            bytes[6] = 30;
            let mut changed = vec![0; 24];
            changed[6] = 1;
            json!({"movies": [{"name": "FIL.DXR", "directorVersion": 500, "score": {
                "labels": [{"name": "3", "frame": 31}],
                "frames": [{"number": 33, "channels": [
                    {"channel": 0, "behaviors": [{"cast": 1, "member": 84}]},
                    {"channel": 1, "tempoRaw": 30, "bytes": bytes, "changed": changed}]}]}}],
                "approximations": []})
        }

        #[test]
        fn film_speed_correction_is_scoped_recorded_and_repeatable() {
            let original = film_score();
            let mut model = original.clone();
            correct_film_speed(&mut model).unwrap();
            let tempo = &model["movies"][0]["score"]["frames"][0]["channels"][1];
            assert_eq!((tempo["tempoRaw"].clone(), tempo["bytes"][6].clone()), (json!(0), json!(0)));
            assert_eq!(model["approximations"][0]["source_tempo"], 30);
            assert_eq!(model["approximations"][0]["original_projector_compared"], false);
            let corrected = model.clone();
            correct_film_speed(&mut model).unwrap();
            assert_eq!(model, corrected);
            let tempo = &mut model["movies"][0]["score"]["frames"][0]["channels"][1];
            tempo["tempoRaw"] = json!(30);
            tempo["bytes"][6] = json!(30);
            model["approximations"] = json!([]);
            assert_eq!(model, original);
        }

        #[test]
        fn film_speed_correction_rejects_changed_source() {
            let changes: [fn(&mut Value); 4] = [
                |m| m["movies"][0]["directorVersion"] = json!(600),
                |m| m["movies"][0]["score"]["frames"][0]["number"] = json!(34),
                |m| m["movies"][0]["score"]["frames"][0]["channels"][0]["behaviors"][0]["member"] = json!(85),
                |m| m["movies"][0]["score"]["frames"][0]["channels"][1]["tempoRaw"] = json!(24),
            ];
            for change in changes {
                let mut model = film_score();
                change(&mut model);
                assert!(correct_film_speed(&mut model).unwrap_err().contains("no longer matches"));
            }
        }
    }

    /// full_media.py correct_film_speed: FILMSPIEL's slider owns tempo on its
    /// animation loop, without the score's 30 fps override on FIL frame 33.
    fn correct_film_speed(model: &mut Value) -> R<()> {
        let correction = json!({"kind": "film-speed-score-correction", "movie": "FIL.DXR", "frame": 33,
            "source_tempo": 30, "target_tempo": 0,
            "detail": "Remove the loop's 30 fps override so FILMSPIEL's slider controls tempo.",
            "original_projector_compared": false});
        let applied = model["approximations"].as_array().is_some_and(|a| a.contains(&correction));
        let expected = if applied { 0 } else { 30 };
        let invalid = || "FIL speed correction no longer matches the recovered score".to_string();
        let movie = model["movies"].as_array_mut().into_iter().flatten().find(|m| m["name"] == "FIL.DXR").ok_or_else(invalid)?;
        let labelled = movie["score"]["labels"].as_array().is_some_and(|l| l.contains(&json!({"name": "3", "frame": 31})));
        let version = movie["directorVersion"] == 500;
        let frame = movie["score"]["frames"].as_array_mut().into_iter().flatten().find(|f| f["number"] == 33).ok_or_else(invalid)?;
        let channels = frame["channels"].as_array_mut().ok_or_else(invalid)?;
        let script_ok = channels.iter().find(|c| c["channel"] == 0).is_some_and(|c| c["behaviors"] == json!([{"cast": 1, "member": 84}]));
        let tempo = channels.iter_mut().find(|c| c["channel"] == 1).ok_or_else(invalid)?;
        let valid = version && labelled && script_ok
            && tempo["tempoRaw"] == expected && tempo["bytes"][6] == expected && tempo["changed"][6] == 1;
        if !valid {
            return Err(invalid());
        }
        tempo["bytes"][6] = json!(0);
        tempo["tempoRaw"] = json!(0);
        if !applied {
            model["approximations"].as_array_mut().ok_or_else(invalid)?.push(correction);
        }
        Ok(())
    }
}

/// libdragon font64 metrics for printable ASCII (the port hosts'
/// text_metrics.decode): advances, kerning pairs among printable
/// characters, and the point size.
pub fn font64_ascii_metrics(font: &[u8]) -> R<Value> {
    if font.len() < 100 || &font[..4] != b"FNT\x0b" {
        return Err("unsupported font64 metrics header".into());
    }
    let u32_at = |at: usize| -> R<usize> {
        font.get(at..at + 4).map(|b| u32::from_be_bytes(b.try_into().unwrap()) as usize).ok_or_else(|| "truncated font64".to_string())
    };
    let (ranges, glyphs, kranges, kern) = (u32_at(72)?, u32_at(80)?, u32_at(84)?, u32_at(92)?);
    let mut indices = std::collections::BTreeMap::new();
    for i in 0..u32_at(36)? {
        let at = ranges + i * 12;
        let first = u32_at(at)? as i64;
        let count = u32_at(at + 4)? as i64;
        let glyph = u32_at(at + 8)? as i32 as i64;
        if glyph < 0 {
            return Err("sparse controller font range".into());
        }
        for c in first.max(32)..(first + count).min(127) {
            indices.insert(c, glyph + c - first);
        }
    }
    if indices.len() != 95 {
        return Err("controller font lacks printable ASCII".into());
    }
    let byte = |at: usize| font.get(at).copied().ok_or_else(|| "truncated font64".to_string());
    let advances = (32..127).map(|c| byte(glyphs + indices[&c] as usize * 8)).collect::<R<Vec<u8>>>()?;
    let reverse: std::collections::HashMap<i64, i64> = indices.iter().map(|(&c, &g)| (g, c)).collect();
    let mut pairs = Vec::new();
    if kranges != 0 {
        for (&c, &g) in &indices {
            let at = kranges + g as usize * 4;
            let lo = u16::from_be_bytes([byte(at)?, byte(at + 1)?]) as usize;
            let hi = u16::from_be_bytes([byte(at + 2)?, byte(at + 3)?]) as usize;
            if lo == 0 {
                continue;
            }
            for i in lo..=hi {
                let at = kern + i * 3;
                let second = i16::from_be_bytes([byte(at)?, byte(at + 1)?]) as i64;
                let amount = byte(at + 2)? as i8 as i64;
                if let Some(&other) = reverse.get(&second) {
                    pairs.push(json!([c, other, amount]));
                }
            }
        }
    }
    Ok(object(vec![("advances", json!(advances)), ("kerning", Value::Array(pairs)), ("size", json!(u32_at(8)?))]))
}

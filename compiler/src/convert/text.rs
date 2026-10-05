//! Typography binding (tools/director/font-model.mjs): decoded source styles
//! bind only to a matching recovered original font; unavailable authored
//! fonts stay explicit, never aliased by similarity.

use std::collections::{BTreeSet, HashMap};

use serde_json::{json, Map, Value};

use super::js;

fn int(v: &Value) -> i64 {
    v.as_i64().or_else(|| v.as_f64().map(|f| f as i64)).unwrap_or(0)
}
fn is_integer(v: &Value) -> bool {
    v.as_i64().is_some() || v.as_f64().is_some_and(|f| f.fract() == 0.0)
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

/// Binds text members' styles to the recovered fonts, records each font's
/// sizes as variants and returns the fidelity limits found.
pub fn bind_text_fonts(movies: &mut [Value], fonts: &mut [Value], field_styles: bool) -> Result<Vec<Value>, String> {
    let mut aliases: HashMap<String, usize> = HashMap::new();
    for (index, font) in fonts.iter().enumerate() {
        for alias in font["aliases"].as_array().into_iter().flatten() {
            let key = alias.as_str().unwrap_or("").to_lowercase();
            if aliases.get(&key).is_some_and(|&other| other != index) {
                return Err(format!("ambiguous embedded font {}", alias.as_str().unwrap_or("")));
            }
            aliases.insert(key, index);
        }
    }
    let mut variants: Vec<BTreeSet<i64>> = vec![BTreeSet::new(); fonts.len()];
    let mut limits = Vec::new();
    for movie in movies.iter_mut() {
        let movie_name = movie["name"].clone();
        for member in movie["members"].as_array_mut().into_iter().flatten() {
            let Some(t) = member.get("typography").filter(|t| !t.is_null()).cloned() else { continue };
            let paragraph = &t["paragraphs"][int(&t["paragraphRuns"][0]["index"]) as usize];
            let mut style = |index: i64| -> Result<Value, String> {
                let s = &t["styles"][index as usize];
                let font = aliases.get(&s["font"].as_str().unwrap_or("").to_lowercase()).copied();
                let size = &s["pointSize"];
                let size_f = size.as_f64().unwrap_or(f64::NAN);
                if !is_integer(size) || !(1.0..=255.0).contains(&size_f) {
                    return Err(format!("unsupported authored point size {}", js::number(size_f)));
                }
                if let Some(f) = font {
                    variants[f].insert(size_f as i64);
                }
                let line_height = if truthy(&paragraph["leadingFixed"]) {
                    paragraph["leadingFixed"].clone()
                } else {
                    json!(int(&s["ascent"]) + int(&s["descent"]) + int(&s["leading"]) + int(&paragraph["leadingExtra"]))
                };
                let fg = |i: usize| ((int(&s["foreground"][i]) as u32 >> 8) & 255) as i64;
                Ok(json!({
                    "fontName": s["font"], "fontId": font.map(|f| fonts[f]["number"].clone()).unwrap_or(json!(0)),
                    "size": size, "align": paragraph["justification"], "ascent": s["ascent"], "descent": s["descent"],
                    "leading": s["leading"], "lineHeight": line_height,
                    "color": fg(0) * 65536 + fg(1) * 256 + fg(2),
                }))
            };
            let text_style = style(int(&t["styleRuns"][0]["index"]))?;
            let insert_style = style(int(&t["insertStyle"]))?;
            let object = member.as_object_mut().unwrap();
            object.insert("textStyle".into(), text_style);
            object.insert("textInsertStyle".into(), insert_style);
            let text_length = member["text"].as_str().map(|s| s.encode_utf16().count()).unwrap_or(0) as i64;
            let mut active: Vec<i64> = Vec::new();
            for run in t["styleRuns"].as_array().into_iter().flatten() {
                let (offset, index) = (int(&run["offset"]), int(&run["index"]));
                if (offset < text_length || offset == 0) && !active.contains(&index) {
                    active.push(index);
                }
            }
            let mut missing: Vec<String> = Vec::new();
            for i in active.iter().copied().chain([int(&t["insertStyle"])]) {
                let font = t["styles"][i as usize]["font"].as_str().unwrap_or("").to_string();
                if !aliases.contains_key(&font.to_lowercase()) && !missing.contains(&font) {
                    missing.push(font);
                }
            }
            let (cast, number) = (member["cast"].clone(), member["number"].clone());
            if !missing.is_empty() {
                limits.push(json!({"movie": movie_name, "cast": cast, "member": number, "kind": "unavailable-source-font",
                    "fonts": missing, "detail": "Authored font names/style metrics preserved; these source fonts are not embedded in the recovered media."}));
            }
            let mut rendering: Vec<&str> = Vec::new();
            let distinct: BTreeSet<String> = active.iter().map(|&i| js::stringify(&t["styles"][i as usize])).collect();
            if distinct.len() > 1 {
                rendering.push("multiple character styles");
            }
            if active.iter().any(|&i| t["styles"][i as usize]["face"].as_object().is_some_and(|f| !f.is_empty())) {
                rendering.push("character face effects");
            }
            let first_paragraph = int(&t["paragraphRuns"][0]["index"]);
            if t["paragraphRuns"].as_array().into_iter().flatten()
                .filter(|r| int(&r["offset"]) < text_length)
                .any(|r| int(&r["index"]) != first_paragraph)
            {
                rendering.push("multiple paragraph styles");
            }
            let p = paragraph;
            if int(&p["justification"]) > 2
                || ["direction", "leftIndent", "rightIndent", "firstIndent", "spacing", "leadingVariable", "topExtra",
                    "bottomExtra", "leftExtra", "rightExtra"].iter().any(|k| truthy(&p[*k]))
                || p["tabs"].as_array().is_some_and(|t| !t.is_empty())
            {
                rendering.push("extended paragraph layout");
            }
            if !rendering.is_empty() {
                limits.push(json!({"movie": movie_name, "cast": cast, "member": number, "kind": "text-layout", "detail": rendering.join(", ")}));
            }
            let object = member.as_object_mut().unwrap();
            object.insert("textPresentation".into(), json!(if missing.is_empty() { "recovered-original-font" } else { "unavailable-source-font" }));
        }
    }
    if field_styles {
        for movie in movies.iter_mut() {
            let movie_name = movie["name"].clone();
            let casts = movie["casts"].clone();
            for member in movie["members"].as_array_mut().into_iter().flatten() {
                if member.get("typography").is_some_and(|t| !t.is_null()) {
                    continue;
                }
                let Some(styles) = member["sourceTextStyles"].as_array().filter(|s| !s.is_empty()).cloned() else { continue };
                let s = &styles[0];
                let cast_index = int(&member["cast"]) - 1;
                let mapped = casts.get(cast_index as usize).and_then(|c| c["fontMap"].get(s["sourceFontId"].to_string())).cloned();
                let font = mapped.as_ref().and_then(|m| aliases.get(&m.as_str().unwrap_or("").to_lowercase())).copied();
                let (cast, number) = (member["cast"].clone(), member["number"].clone());
                let Some(f) = font else {
                    let name = mapped.clone().unwrap_or_else(|| json!(format!("font id {}", s["sourceFontId"])));
                    limits.push(json!({"movie": movie_name, "cast": cast, "member": number, "kind": "unavailable-source-font",
                        "fonts": [name], "detail": "Authored field font identity preserved; this source font is not embedded in the recovered media."}));
                    member.as_object_mut().unwrap().insert("textPresentation".into(), json!("unavailable-source-font"));
                    continue;
                };
                let size = int(&s["size"]);
                if !is_integer(&s["size"]) || !(1..=255).contains(&size) {
                    return Err(format!("unsupported authored field point size {}", s["size"]));
                }
                variants[f].insert(size);
                let align = match int(&member["textAlign"]) {
                    1 => 1,
                    -1 => 2,
                    _ => 0,
                };
                let style = json!({
                    "fontName": mapped, "fontId": fonts[f]["number"], "size": s["size"], "align": align,
                    "ascent": s["ascent"], "descent": (int(&s["lineHeight"]) - int(&s["ascent"])).max(0), "leading": 0,
                    "lineHeight": s["lineHeight"], "color": s["color"],
                });
                let text_length = member["text"].as_str().map(|t| t.encode_utf16().count()).unwrap_or(0) as i64;
                let active: Vec<&Value> =
                    styles.iter().filter(|r| int(&r["offset"]) < text_length || int(&r["offset"]) == 0).collect();
                let mut rendering: Vec<&str> = Vec::new();
                let distinct: BTreeSet<String> = active
                    .iter()
                    .map(|r| js::stringify(&json!([r["sourceFontId"], r["size"], r["face"], r["color"]])))
                    .collect();
                if distinct.len() > 1 {
                    rendering.push("multiple character styles");
                }
                if truthy(&s["face"]) {
                    rendering.push("character face effects");
                }
                if !rendering.is_empty() {
                    limits.push(json!({"movie": movie_name, "cast": cast, "member": number, "kind": "text-layout", "detail": rendering.join(", ")}));
                }
                let object = member.as_object_mut().unwrap();
                // `member.textStyle = member.textInsertStyle = {...}` assigns right to left.
                object.insert("textInsertStyle".into(), style.clone());
                object.insert("textStyle".into(), style);
                object.insert("textPresentation".into(), json!("recovered-original-font"));
            }
        }
    }
    for (index, font) in fonts.iter_mut().enumerate() {
        let number = font["number"].clone();
        let list: Vec<Value> = variants[index]
            .iter()
            .map(|size| json!({"size": size, "asset": format!("fonts/f{}-{size}.font64", js::stringify(&number))}))
            .collect();
        font.as_object_mut().unwrap().insert("variants".into(), Value::Array(list));
    }
    Ok(limits)
}

/// Score and literal-name evidence for text members (font-audit.json).
pub fn text_reachability(movies: &[Value], scripts: &[(String, String)]) -> Value {
    let by_name: HashMap<&str, &Value> = movies.iter().map(|m| (m["name"].as_str().unwrap_or(""), m)).collect();
    let mut score_refs: HashMap<String, Vec<Value>> = HashMap::new();
    for movie in movies {
        for frame in movie["score"]["frames"].as_array().into_iter().flatten() {
            for c in frame["channels"].as_array().into_iter().flatten() {
                if int(&c["channel"]) < 6 {
                    continue;
                }
                let b: Vec<u8> = c["bytes"].as_array().into_iter().flatten().map(|v| int(v) as u8).collect();
                let (cast, number) = (u16::from_be_bytes([b[4], b[5]]) as i64, u16::from_be_bytes([b[6], b[7]]) as i64);
                if b[0] == 0 || number == 0 {
                    continue;
                }
                let library = movie["casts"].as_array().into_iter().flatten().find(|x| int(&x["number"]) == cast);
                let Some(target) = library.and_then(|l| by_name.get(l["file"].as_str().unwrap_or(""))) else { continue };
                let target_cast = if std::ptr::eq(*target, movie) { cast } else { 1 };
                let key = format!("{}:{target_cast}:{number}", target["name"].as_str().unwrap_or(""));
                let refs = score_refs.entry(key).or_default();
                if refs.len() < 8 {
                    refs.push(json!({"movie": movie["name"], "frame": frame["number"], "channel": c["channel"]}));
                }
            }
        }
    }
    let mut members = Vec::new();
    for movie in movies {
        for m in movie["members"].as_array().into_iter().flatten() {
            if !m.get("typography").is_some_and(|t| !t.is_null()) {
                continue;
            }
            let key = format!("{}:{}:{}", movie["name"].as_str().unwrap_or(""), js::stringify(&m["cast"]), js::stringify(&m["number"]));
            let name = m["name"].as_str().unwrap_or("");
            let literal: Vec<Value> = if name.is_empty() {
                vec![]
            } else {
                let needle = format!("\"{}\"", name.to_lowercase());
                scripts.iter().filter(|(_, text)| text.to_lowercase().contains(&needle)).map(|(n, _)| json!(n)).collect()
            };
            let score = score_refs.get(&key).cloned().unwrap_or_default();
            let referenced = !score.is_empty() || !literal.is_empty();
            let mut entry = Map::new();
            for (k, v) in [
                ("movie", movie["name"].clone()), ("cast", m["cast"].clone()), ("member", m["number"].clone()),
                ("name", m["name"].clone()), ("initialFont", m["textStyle"]["fontName"].clone()),
                ("insertionFont", m["textInsertStyle"]["fontName"].clone()),
                ("initialFontRecovered", json!(truthy(&m["textStyle"]["fontId"]))),
                ("insertionFontRecovered", json!(truthy(&m["textInsertStyle"]["fontId"]))),
                ("scoreReferences", Value::Array(score)), ("literalNameReferences", Value::Array(literal)),
                ("evidence", json!(if referenced { "referenced" } else { "no score or literal-name reference found" })),
            ] {
                entry.insert(k.into(), v);
            }
            members.push(Value::Object(entry));
        }
    }
    json!({"computedReferencesProvenUnreachable": false, "members": members})
}

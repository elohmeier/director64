//! All source character/paragraph boundaries retained as normalized RICH runs.
use super::{font_scope, int, scoped_font, truthy};
use serde_json::{Value, json};
use std::collections::{BTreeSet, HashMap};
fn normalized(style: &Value, offset: i64, face: u8) -> Value {
    json!({"offset":offset,"fontId":style["fontId"],"size":style["size"],
  "face":face,"align":style["align"],"ascent":style["ascent"],
  "lineHeight":style["lineHeight"],"color":style["color"],"charSpacing":0})
}
fn char_spacing(value: &Value) -> Result<i16, String> {
    if value.is_null() {
        return Ok(0);
    }
    // Paige style_info.char_extra is a signed16.16 pixel value.
    // Hermes-Paige cddd954, PGHEADER/PAIGE.H and PGSOURCE/PGREAD.C.
    value.as_i64().filter(|n| n % 65536 == 0)
        .and_then(|n| i16::try_from(n / 65536).ok())
        .ok_or_else(|| format!("unsupported authored characterExtra {value}; requires integral pixels in -32768..32767"))
}
fn add(formats: &mut Vec<Value>, value: Value) {
    if formats
        .last()
        .is_some_and(|last| last["offset"] == value["offset"])
    {
        formats.pop();
    }
    let mut compared = value.clone();
    compared.as_object_mut().unwrap().remove("offset");
    if let Some(last) = formats.last() {
        let mut before = last.clone();
        before.as_object_mut().unwrap().remove("offset");
        if before == compared {
            return;
        }
    }
    formats.push(value);
}
fn size(value: &Value) -> Result<i64, String> {
    value
        .as_i64()
        .or_else(|| {
            value
                .as_f64()
                .filter(|n| n.fract() == 0.0)
                .map(|n| n as i64)
        })
        .filter(|n| (1..=255).contains(n))
        .ok_or_else(|| format!("unsupported authored point size {value}"))
}
pub(super) fn paige_size(value: &Value) -> Result<i64, String> {
    // Paige's style_info point=0 selects its default twelve-point font.
    // Keep this separate from STXT, which stores an explicit point size.
    // HERMES Paige programming manual, section 30.6, "Changing point size".
    if value.as_f64() == Some(0.0) {
        Ok(12)
    } else {
        size(value)
    }
}
fn page_face(face: &Value) -> (u8, bool) {
    let flags = [
        ("bold", 1),
        ("italic", 2),
        ("underline", 4),
        ("strikeout", 8),
    ];
    let mut out = 0;
    for (name, bit) in flags {
        if truthy(&face[name]) {
            out |= bit;
        }
    }
    let unsupported = face.as_object().is_some_and(|f| {
        f.iter()
            .any(|(name, value)| truthy(value) && !flags.iter().any(|(known, _)| name == known))
    });
    (out, unsupported)
}
/// Add format tables after legacy single-style metadata and merge diagnostics.
pub fn bind(
    movies: &mut [Value],
    fonts: &[Value],
    aliases: &HashMap<String, Vec<usize>>,
    variants: &mut [BTreeSet<i64>],
    field_styles: bool,
    limits: &mut Vec<Value>,
) -> Result<(), String> {
    for movie in movies {
        if movie["directorVersion"].as_i64().is_some_and(|v| v < 600) {
            continue;
        }
        let scope = font_scope(movie);
        let movie_name = movie["name"].clone();
        let casts = movie["casts"].clone();
        for member in movie["members"].as_array_mut().into_iter().flatten() {
            if member["type"].as_i64().is_some_and(|t| t != 3 && t != 7) {
                continue;
            }
            let text_length = member["text"]
                .as_str()
                .map_or(0, |t| t.encode_utf16().count()) as i64;
            let mut formats = Vec::new();
            let mut missing = Vec::<Value>::new();
            let mut unsupported = false;
            let mut paragraph_layout = false;
            if let Some(t) = member.get("typography").filter(|t| !t.is_null()).cloned() {
                let mut boundaries = BTreeSet::from([0]);
                for r in t["styleRuns"]
                    .as_array()
                    .into_iter()
                    .flatten()
                    .chain(t["paragraphRuns"].as_array().into_iter().flatten())
                {
                    let offset = int(&r["offset"]);
                    if offset >= 0 && offset < text_length {
                        boundaries.insert(offset);
                    }
                }
                boundaries.insert(text_length);
                for offset in boundaries {
                    let selected = |key: &str| {
                        t[key]
                            .as_array()
                            .into_iter()
                            .flatten()
                            .filter(|r| int(&r["offset"]) <= offset)
                            .last()
                            .map_or(0, |r| int(&r["index"]))
                    };
                    let index = if offset == text_length {
                        int(&t["insertStyle"])
                    } else {
                        selected("styleRuns")
                    };
                    let source = &t["styles"][index as usize];
                    let paragraph = &t["paragraphs"][selected("paragraphRuns") as usize];
                    paragraph_layout |= int(&paragraph["justification"]) > 3
                        || [
                            "direction",
                            "leftIndent",
                            "rightIndent",
                            "firstIndent",
                            "spacing",
                            "leadingVariable",
                            "topExtra",
                            "bottomExtra",
                            "leftExtra",
                            "rightExtra",
                        ]
                        .iter()
                        .any(|k| truthy(&paragraph[*k]))
                        || paragraph["tabs"]
                            .as_array()
                            .is_some_and(|tabs| !tabs.is_empty());
                    let name = source["font"].as_str().unwrap_or("");
                    let found = scoped_font(aliases, fonts, &scope, name)?;
                    let point = paige_size(&source["pointSize"])?;
                    if let Some(f) = found {
                        variants[f].insert(point);
                    } else if !missing.contains(&json!(name)) {
                        missing.push(json!(name));
                    }
                    let (face, unhandled) = page_face(&source["face"]);
                    unsupported |= unhandled;
                    let fg = |i: usize| ((int(&source["foreground"][i]) >> 8) & 255) as u32;
                    let line = if truthy(&paragraph["leadingFixed"]) {
                        int(&paragraph["leadingFixed"])
                    } else {
                        int(&source["ascent"])
                            + int(&source["descent"])
                            + int(&source["leading"])
                            + int(&paragraph["leadingExtra"])
                    };
                    let style = json!({"fontId":found.map(|f|fonts[f]["number"].clone()).unwrap_or(json!(0)),
      "size":point,"align":int(&paragraph["justification"]),"ascent":int(&source["ascent"]),
      "lineHeight":line,"color":(fg(0)<<16)|(fg(1)<<8)|fg(2)});
                    let mut run = normalized(&style, offset, face);
                    run["charSpacing"] = json!(char_spacing(&source["characterExtra"]).map_err(
                        |error| format!(
                            "{movie_name} cast {} member {} UTF16 offset {offset}: {error}",
                            member["cast"], member["number"]
                        )
                    )?);
                    // Director reports the stored zero while Paige renders
                    // the default12-point glyphs (original8.5 probe).
                    run["size"] = source["pointSize"].clone();
                    add(&mut formats, run);
                }
            } else if field_styles {
                for source in member["sourceTextStyles"].as_array().into_iter().flatten() {
                    let offset = int(&source["offset"]);
                    if offset < 0 || offset > text_length {
                        return Err("field style offset outside text".into());
                    }
                    let cast = int(&member["cast"]) - 1;
                    let name = casts
                        .get(cast as usize)
                        .and_then(|c| c["fontMap"].get(source["sourceFontId"].to_string()))
                        .and_then(Value::as_str);
                    let found = match name {
                        Some(n) => scoped_font(aliases, fonts, &scope, n)?,
                        None => None,
                    };
                    let point = size(&source["size"])?;
                    if let Some(f) = found {
                        variants[f].insert(point);
                    } else {
                        let name = name.map(|n| json!(n)).unwrap_or_else(|| {
                            json!(format!("font id {}", source["sourceFontId"]))
                        });
                        if !missing.contains(&name) {
                            missing.push(name);
                        }
                    }
                    let face = int(&source["face"]);
                    unsupported |= face & !7 != 0;
                    let style = json!({"fontId":found.map(|f|fonts[f]["number"].clone()).unwrap_or(json!(0)),
      "size":point,"align":match int(&member["textAlign"]){-1=>2,1=>1,_=>0},
      "ascent":int(&source["ascent"]),"lineHeight":int(&source["lineHeight"]),"color":int(&source["color"])});
                    add(&mut formats, normalized(&style, offset, (face & 7) as u8));
                }
            }
            if formats.is_empty() {
                continue;
            }
            if formats[0]["offset"] != 0 || formats.len() > 4096 {
                return Err("invalid or excessive text format boundaries".into());
            }
            for pair in formats.windows(2) {
                if int(&pair[0]["offset"]) >= int(&pair[1]["offset"]) {
                    return Err("text style offsets must increase".into());
                }
            }
            let identity = |l: &Value| {
                l["movie"] == movie_name
                    && l["cast"] == member["cast"]
                    && l["member"] == member["number"]
            };
            if !missing.is_empty() {
                if let Some(limit) = limits
                    .iter_mut()
                    .find(|l| identity(l) && l["kind"] == "unavailable-source-font")
                {
                    let fonts = limit["fonts"].as_array_mut().unwrap();
                    for name in missing {
                        if !fonts.contains(&name) {
                            fonts.push(name);
                        }
                    }
                } else {
                    limits.push(json!({"movie":movie_name,"cast":member["cast"],"member":member["number"],"kind":"unavailable-source-font","fonts":missing,"detail":"Authored run font identity preserved; source font is not embedded."}));
                }
            }
            if paragraph_layout
                && !limits.iter().any(|l| {
                    identity(l)
                        && l["kind"] == "text-layout"
                        && l["detail"]
                            .as_str()
                            .is_some_and(|d| d.contains("extended paragraph layout"))
                })
            {
                limits.push(json!({"movie":movie_name,"cast":member["cast"],"member":member["number"],"kind":"text-layout","detail":"extended paragraph layout"}));
            }
            if unsupported {
                limits.push(json!({"movie":movie_name,"cast":member["cast"],"member":member["number"],"kind":"text-layout","detail":"unsupported character face effects"}));
            }
            member
                .as_object_mut()
                .unwrap()
                .insert("textFormats".into(), json!(formats));
        }
    }
    Ok(())
}

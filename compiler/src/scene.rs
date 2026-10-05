//! Scene tables from the converter's model (director/model.json): casts,
//! members, the member-name index, palette, score frames and deltas, labels
//! and each family's additions (text styles, cue points, behaviors, D5
//! video, D10 Flash surfaces), computed exactly as src/director64/director.py
//! writes them into generated C for a runtime built for `Profile`.

use std::collections::{BTreeMap, HashMap, HashSet};

use serde_json::Value;

use crate::emit::text_hash;

/// Mirrors DG_SPAN in runtime/director/director.h.
pub const DG_SPAN: u16 = 0x8000;

/// The runtime family a package is laid out for (runtime/lingo/family.h):
/// the port's Director version (500, 600, 700, 800, 1000) and whether it is
/// built with the extended-D6 services.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Profile {
    pub version: u16,
    pub extended: bool,
}

impl Profile {
    /// DG_MODERN: every family but plain Director 6.
    pub fn modern(&self) -> bool {
        !(self.version == 600 && !self.extended)
    }
    pub fn d5(&self) -> bool {
        self.version == 500
    }
    pub fn d10(&self) -> bool {
        self.version >= 1000
    }
}

pub struct Cast {
    pub name: String,
    pub file: u16,
    pub cast: u16,
}

/// dg_text_style_t.
#[derive(Clone, Debug, PartialEq)]
pub struct TextStyle {
    pub font_name: String,
    pub font_id: u8,
    pub size: u8,
    pub align: u8,
    pub ascent: i16,
    pub descent: i16,
    pub leading: i16,
    pub line_height: i16,
    pub color: u32,
    /// Measured metrics (extended builds only): advances from codepoint 32
    /// and kerning triples (first, second, amount), flattened.
    pub advances: Option<Vec<u8>>,
    pub kerning: Vec<i16>,
    pub kerning_count: u32,
}

/// dg_flash_field_t.
pub struct FlashField {
    pub name: String,
    pub variable: String,
    pub text: String,
    pub x: i16,
    pub y: i16,
    pub width: i16,
    pub height: i16,
    pub margin_left: i16,
    pub margin_right: i16,
    pub indent: i16,
    pub align: u8,
    pub word_wrap: u8,
    pub multiline: u8,
    pub leading: i8,
    pub style: Option<u16>,
}

pub struct Member {
    pub id: u32,
    pub number: u16,
    pub cast: u16,
    pub kind: u16,
    pub width: u16,
    pub height: u16,
    pub reg_x: i16,
    pub reg_y: i16,
    pub name: String,
    pub asset: String,
    pub text: String,
    pub samples: u32,
    pub rate: u32,
    pub loop_start: u32,
    pub loop_end: u32,
    pub shape: u16,
    pub pattern: u16,
    pub filled: u8,
    pub line_width: u8,
    pub looping: u8,
    pub film_loop: u8,
    pub line_direction: u8,
    pub film_assets: Vec<String>,
    // DG_EXTENDED
    pub editable: u8,
    /// Binary text (with NULs), stored as bytes rather than a string.
    pub text_bytes: Option<Vec<u8>>,
    pub source_bytes: u32,
    pub cues: Vec<(u32, String)>,
    // DG_MODERN: indices into Scene::styles
    pub style: Option<u16>,
    pub insert_style: Option<u16>,
    // DG_D5
    pub video_audio: String,
    pub video_flags: u32,
    pub film_sounds: Vec<u32>,
    // DG_D10
    pub source_xtra: u8,
    pub flash_labels: Vec<(String, u16)>,
    pub flash_fields: Vec<FlashField>,
}

/// dg_spec_t, in field order.
#[derive(Clone, Default, PartialEq, Debug)]
pub struct Spec {
    pub member: u32,
    pub script: u32,
    pub x: i16,
    pub y: i16,
    pub width: i16,
    pub height: i16,
    pub ink: u8,
    pub blend: u8,
    pub kind: u8,
    pub flags: u8,
    pub fore: u8,
    pub back: u8,
    pub thickness: u8,
    pub stretch: u8,
    pub trails: u8,
    // DG_MODERN
    pub loc_z: i16,
    pub tempo: u16,
    pub delay: u16,
    pub fore_rgb: u32,
    pub back_rgb: u32,
    pub rotation: i32,
    pub skew: i32,
    // DG_EXTENDED
    pub behaviors: Vec<(u32, String)>,
}

pub struct Delta {
    pub channel: u16,
    pub mask: u16,
    pub spec: Spec,
}

pub struct Scene {
    pub name: String,
    pub stem: String,
    pub id: u16,
    pub tempo: u16,
    pub casts: Vec<Cast>,
    pub members: Vec<Member>,
    pub member_index: Vec<(u32, u16)>,
    pub palette: Vec<u32>,
    pub frames: Vec<(u32, u16)>,
    pub deltas: Vec<Delta>,
    pub labels: Vec<(String, u16)>,
    // DG_MODERN
    pub stage_color: u32,
    pub styles: Vec<TextStyle>,
}

fn int(value: Option<&Value>) -> i64 {
    value.and_then(|v| v.as_i64().or_else(|| v.as_f64().map(|f| f as i64)).or_else(|| v.as_bool().map(i64::from))).unwrap_or(0)
}
fn float(value: Option<&Value>) -> f64 {
    value.and_then(|v| v.as_f64().or_else(|| v.as_bool().map(|b| b as i64 as f64))).unwrap_or(0.0)
}
/// Python truthiness of a JSON value.
fn truthy(value: Option<&Value>) -> bool {
    match value {
        None | Some(Value::Null) => false,
        Some(Value::Bool(b)) => *b,
        Some(Value::Number(n)) => n.as_f64().is_some_and(|f| f != 0.0),
        Some(Value::String(s)) => !s.is_empty(),
        Some(Value::Array(a)) => !a.is_empty(),
        Some(Value::Object(o)) => !o.is_empty(),
    }
}
fn text(value: Option<&Value>) -> String {
    value.and_then(Value::as_str).unwrap_or("").to_string()
}
/// A value as a C initializer converts it into the field's type: modulo
/// the type's width, as the generated tables have always compiled.
trait CInt {
    fn wrap(value: i64) -> Self;
}
macro_rules! c_int {
    ($($t:ty),*) => {$(impl CInt for $t { fn wrap(value: i64) -> Self { value as $t } })*};
}
c_int!(u8, i8, u16, i16, u32, i32);
fn narrow<T: CInt>(value: i64, _what: &str) -> Result<T, String> {
    Ok(T::wrap(value))
}
/// Python's round(): half to even.
fn round(value: f64) -> i64 {
    value.round_ties_even() as i64
}
/// Python's str.casefold for the identifiers the tables compare.
fn casefold(text: &str) -> String {
    text.to_lowercase()
}

/// A versioned FPS command; a D8 delay is a separate native field.
pub fn score_tempo(record: &[u8], version: u32, extended: bool) -> Result<u16, String> {
    let opcode = record[6];
    if opcode == 0 {
        return Ok(0);
    }
    if (version >= 7 || extended) && opcode == 247 {
        return Ok(0);
    }
    if opcode != 246 {
        return Err(format!("unsupported Director {version} tempo opcode {opcode}"));
    }
    let rate = u16::from_be_bytes([record[4], record[5]]);
    if !(1..=if version >= 7 { 999 } else { 120 }).contains(&rate) {
        return Err(format!("unsupported Director {version} frame rate {rate}"));
    }
    Ok(rate)
}

/// Director version the movie's tables are laid out for, as director.py
/// derives it from the declared version and the score record size.
fn movie_version(movie: &Value) -> u32 {
    let score = movie.get("score").cloned().unwrap_or(Value::Null);
    let mut declared = int(movie.get("directorVersion")).max(0) as u32;
    if movie.get("directorVersion").is_none() {
        declared = 6;
    }
    if declared >= 100 {
        declared /= 100;
    }
    let record = score.get("recordSize").or_else(|| score.get("record_size")).map(|v| int(Some(v))).unwrap_or(24);
    if declared == 5 {
        5
    } else if record == 48 || int(score.get("version")) >= 13 || declared >= 7 {
        if declared >= 10 {
            10
        } else if declared == 7 {
            7
        } else {
            8
        }
    } else {
        6
    }
}

/// Rejects split identities between recovered code and resource bindings.
pub fn validate_script_casts(model: &Value, program: &Value) -> Result<(), String> {
    let movies = model["movies"].as_array().ok_or("model without movies")?;
    for handler in program["handlers"].as_array().ok_or("program without handlers")? {
        let movie_name = casefold(&text(handler.get("movie")));
        let movie = movies
            .iter()
            .rfind(|m| casefold(&text(m.get("name"))) == movie_name)
            .ok_or_else(|| format!("{movie_name}: script movie has no scene"))?;
        let cast_name = casefold(&text(handler.get("cast")));
        let cast = movie["casts"].as_array().into_iter().flatten().rfind(|c| casefold(&text(c.get("name"))) == cast_name);
        if !cast.is_some_and(|c| casefold(&text(c.get("file"))) == movie_name) {
            return Err(format!(
                "{}:{}:{}: script cast does not match local resource namespace",
                text(handler.get("movie")),
                text(handler.get("cast")),
                int(handler.get("member"))
            ));
        }
    }
    Ok(())
}

/// A style's canonical identity, as director.py dedupes them
/// (json.dumps with sorted keys).
fn style_key(style: &Value) -> String {
    fn sorted(v: &Value) -> Value {
        match v {
            Value::Object(m) => {
                let ordered: BTreeMap<&String, Value> = m.iter().map(|(k, v)| (k, sorted(v))).collect();
                Value::Object(ordered.into_iter().map(|(k, v)| (k.clone(), v)).collect())
            }
            Value::Array(a) => Value::Array(a.iter().map(sorted).collect()),
            other => other.clone(),
        }
    }
    sorted(style).to_string()
}

struct Styles<'a> {
    profile: Profile,
    metrics: &'a serde_json::Map<String, Value>,
    table: Vec<TextStyle>,
    known: HashMap<String, u16>,
}

impl Styles<'_> {
    fn metrics_for(&self, font_id: i64, size: &Value) -> Option<&Value> {
        // f"{fontId}:{size}": an integer prints plainly, a float as repr().
        let size = match (size.as_i64(), size.as_f64()) {
            (Some(i), _) => i.to_string(),
            (None, Some(f)) => format!("{f:?}"),
            _ => size.to_string(),
        };
        self.metrics.get(&format!("{font_id}:{size}"))
    }

    /// The style's index in the movie's table, adding it on first use.
    fn add(&mut self, style: &Value) -> Result<Option<u16>, String> {
        if !truthy(Some(style)) {
            return Ok(None);
        }
        let key = style_key(style);
        if let Some(&index) = self.known.get(&key) {
            return Ok(Some(index));
        }
        let font_id = int(style.get("fontId"));
        let mut s = TextStyle {
            font_name: text(style.get("fontName")),
            font_id: narrow(font_id, "text style font")?,
            size: narrow(int(style.get("size")), "text style size")?,
            align: narrow(int(style.get("align")), "text style alignment")?,
            ascent: narrow(int(style.get("ascent")), "text style ascent")?,
            descent: narrow(int(style.get("descent")), "text style descent")?,
            leading: narrow(int(style.get("leading")), "text style leading")?,
            line_height: narrow(int(style.get("lineHeight")), "text style line height")?,
            color: narrow(int(style.get("color")), "text style color")?,
            advances: None,
            kerning: Vec::new(),
            kerning_count: 0,
        };
        let metrics = match style.get("controllerMetrics") {
            Some(m) if !m.is_null() => Some(m.clone()),
            _ if font_id != 0 => self.metrics_for(font_id, &style["size"]).cloned(),
            _ => None,
        };
        if let Some(metrics) = metrics.filter(|m| truthy(Some(m))) {
            if !self.profile.extended {
                return Err("measured text metrics need the extended runtime".into());
            }
            let advances = metrics["advances"].as_array().ok_or("text metrics without advances")?;
            s.advances = Some(advances.iter().map(|v| narrow(int(Some(v)), "advance")).collect::<Result<_, _>>()?);
            let pairs = metrics["kerning"].as_array().ok_or("text metrics without kerning")?;
            s.kerning_count = pairs.len() as u32;
            for pair in pairs {
                for v in pair.as_array().ok_or("kerning pair")? {
                    s.kerning.push(narrow(int(Some(v)), "kerning")?);
                }
            }
            if s.ascent == 0 && s.line_height == 0 && metrics.get("ascent").is_some() {
                // Zeroed STXT vertical metrics mean "use the face's own".
                s.ascent = narrow(int(metrics.get("ascent")), "measured ascent")?;
                s.descent = narrow(int(metrics.get("descent")), "measured descent")?;
                s.line_height = narrow(
                    int(metrics.get("ascent")) + int(metrics.get("descent")) + s.leading as i64,
                    "measured line height",
                )?;
            }
        }
        let index = narrow(self.table.len() as i64, "text style count")?;
        self.table.push(s);
        self.known.insert(key, index);
        Ok(Some(index))
    }
}

/// A converted Flash edit field's style: its recorded descriptor plus the
/// embedded font's measured metrics.
fn flash_field_style(field: &Value, aliases: &HashMap<String, i64>, metrics: &serde_json::Map<String, Value>) -> Value {
    let height = float(field.get("fontHeight"));
    let size = if truthy(field.get("fontHeight")) { round(height).max(1) } else { 12 };
    let leading = if truthy(field.get("leading")) { round(float(field.get("leading"))) } else { 0 };
    let font_name = text(field.get("fontName"));
    let font_id = aliases.get(&casefold(&font_name)).copied().unwrap_or(0);
    let measured = if font_id != 0 { metrics.get(&format!("{font_id}:{size}")) } else { None };
    let color: Vec<i64> = match field.get("color").and_then(Value::as_array).filter(|c| !c.is_empty()) {
        Some(c) => c.iter().map(|v| int(Some(v))).collect(),
        None => vec![0, 0, 0, 255],
    };
    let align = match int(field.get("align")) {
        1 => 2,
        2 => 1,
        _ => 0,
    };
    serde_json::json!({
        "fontName": font_name,
        "fontId": font_id,
        "size": size,
        "align": align,
        "ascent": measured.map_or(size, |m| int(m.get("ascent"))),
        "descent": measured.map_or(0, |m| int(m.get("descent"))),
        "leading": leading,
        "lineHeight": size + leading,
        "color": color[0] * 65536 + color[1] * 256 + color[2],
    })
}

/// Python's `\s` for str: str.isspace().
fn python_space(c: char) -> bool {
    c.is_whitespace() || ('\u{1c}'..='\u{1f}').contains(&c)
}

/// Authored initial field markup reduced to its runs: `</p>` and the
/// whitespace after it become a carriage return, other tags vanish.
fn strip_field_html(initial: &str) -> String {
    let mut out = String::new();
    let mut rest = initial;
    while let Some(at) = rest.find("</p>") {
        out.push_str(&rest[..at]);
        out.push('\r');
        rest = rest[at + 4..].trim_start_matches(python_space);
    }
    out.push_str(rest);
    let mut stripped = String::new();
    let mut chars = out.char_indices().peekable();
    let bytes = out.as_str();
    while let Some((i, c)) = chars.next() {
        if c == '<' {
            if let Some(close) = bytes[i + 1..].find('>') {
                let end = i + 1 + close;
                while chars.peek().is_some_and(|&(j, _)| j <= end) {
                    chars.next();
                }
                continue;
            }
        }
        stripped.push(c);
    }
    stripped.trim_end_matches('\r').to_string()
}

pub fn scenes(model: &Value, profile: Profile) -> Result<Vec<Scene>, String> {
    if model["problems"].as_array().is_none_or(|p| !p.is_empty()) {
        return Err("resource conversion is incomplete".into());
    }
    let extended = model.get("extendedD6").is_some_and(|v| truthy(Some(v)));
    if extended != profile.extended {
        return Err("the model's extended-D6 flag does not match the runtime profile".into());
    }
    let movies = model["movies"].as_array().ok_or("model without movies")?;
    let id_of = |name: &str| -> Result<u32, String> {
        movies
            .iter()
            .rfind(|m| text(m.get("name")) == name)
            .map(|m| int(m.get("id")) as u32)
            .ok_or_else(|| format!("unknown cast file {name}"))
    };
    let empty_metrics = serde_json::Map::new();
    let metrics = model.get("fontMetrics").and_then(Value::as_object).unwrap_or(&empty_metrics);
    let mut aliases = HashMap::new();
    for font in model.get("fonts").and_then(Value::as_array).into_iter().flatten() {
        for alias in font.get("aliases").and_then(Value::as_array).into_iter().flatten() {
            aliases.insert(casefold(alias.as_str().unwrap_or("")), int(font.get("number")));
        }
    }
    let mut result = Vec::new();
    for (position, movie) in movies.iter().enumerate() {
        let name = text(movie.get("name"));
        if int(movie.get("id")) != position as i64 + 1 {
            return Err("movie ids must index the registry".into());
        }
        let version = movie_version(movie);
        // director.py emits a family's fields where its version asks for
        // them; the runtime's structures must have them.
        let modern_fields = version != 6 || extended;
        if (modern_fields && !profile.modern())
            || (version == 5 && !profile.d5())
            || (version >= 10 && !profile.d10())
        {
            return Err(format!("{name}: Director {version} tables need a different runtime profile"));
        }
        let mut styles = Styles { profile, metrics, table: Vec::new(), known: HashMap::new() };
        let casts_json = movie["casts"].as_array().cloned().unwrap_or_default();
        let reference = |cast: i64, member: i64| -> Result<u32, String> {
            if member == 0 {
                return Ok(0);
            }
            if cast < 1 || cast as usize > casts_json.len() {
                return Err(format!("{name}: invalid cast {cast}:{member}"));
            }
            let library = &casts_json[cast as usize - 1];
            let file = text(library.get("file"));
            let target = id_of(&file)?;
            let target_cast = if file == name { cast } else { 1 };
            Ok(target << 20 | (target_cast as u32) << 16 | member as u32)
        };
        let palette: Vec<u32> = match movie.get("palette").and_then(Value::as_array) {
            Some(p) => p.iter().map(|v| int(Some(v)) as u32).collect(),
            None => {
                let mut p = vec![0xFFFFFF; 255];
                p.push(0);
                p
            }
        };
        if palette.len() != 256 {
            return Err("scene palette must have 256 colors".into());
        }
        let mut sorted: Vec<&Value> = movie["members"].as_array().map(|m| m.iter().collect()).unwrap_or_default();
        sorted.sort_by_key(|m| (int(m.get("cast")), int(m.get("number"))));
        let mut members = Vec::new();
        for m in sorted {
            let film_assets: Vec<String> = m
                .get("filmAssets")
                .and_then(Value::as_array)
                .map(|a| a.iter().map(|v| v.as_str().unwrap_or("").to_string()).collect())
                .unwrap_or_default();
            if film_assets.len() > 255 {
                return Err(format!("{name}: film loop with {} poses", film_assets.len()));
            }
            let what = |field: &str| format!("{name} member {}:{} {field}", int(m.get("cast")), int(m.get("number")));
            let member_text = text(m.get("text"));
            let binary = extended && member_text.contains('\0');
            let text_bytes = if binary {
                let hex = text(m.get("textHex"));
                Some((0..hex.len()).step_by(2).map(|i| u8::from_str_radix(&hex[i..i + 2], 16)).collect::<Result<Vec<u8>, _>>()
                    .map_err(|_| what("textHex"))?)
            } else {
                None
            };
            let cue_points = m.get("cuePoints").and_then(Value::as_array).cloned().unwrap_or_default();
            let cues = if extended {
                cue_points
                    .iter()
                    .map(|c| Ok((narrow(int(c.get("milliseconds")), &what("cue"))?, text(c.get("name")))))
                    .collect::<Result<Vec<_>, String>>()?
            } else {
                Vec::new()
            };
            let (style, insert_style) = if modern_fields {
                (styles.add(&m["textStyle"])?, styles.add(&m["textInsertStyle"])?)
            } else {
                (None, None)
            };
            let film_sounds = if version == 5 {
                let mut values = Vec::new();
                for frame in m.get("filmSounds").and_then(Value::as_array).into_iter().flatten() {
                    for pair in frame.as_array().into_iter().flatten() {
                        values.push(if truthy(Some(pair)) { reference(int(pair.get("cast")), int(pair.get("member")))? } else { 0 });
                    }
                }
                values
            } else {
                Vec::new()
            };
            let (mut flash_labels, mut flash_fields) = (Vec::new(), Vec::new());
            if version >= 10 {
                for label in m.get("flashTimeline").and_then(|t| t.get("labels")).and_then(Value::as_array).into_iter().flatten() {
                    flash_labels.push((text(label.get("name")), narrow(int(label.get("frame")), &what("flash label"))?));
                }
                let mut seen = HashSet::new();
                for field in m.get("flashFields").and_then(Value::as_array).into_iter().flatten() {
                    let field_name = text(field.get("name"));
                    let variable = text(field.get("variable"));
                    let binding = casefold(if !field_name.is_empty() { &field_name } else { &variable });
                    if binding.is_empty() || !seen.insert(binding) {
                        continue;
                    }
                    let mut initial = text(field.get("text"));
                    if truthy(field.get("html")) && !initial.is_empty() {
                        initial = strip_field_html(&initial);
                    }
                    let bounds = &field["bounds"];
                    let rounded = |v: Option<&Value>| if truthy(v) { round(float(v)) } else { 0 };
                    let style = styles.add(&flash_field_style(field, &aliases, metrics))?;
                    flash_fields.push(FlashField {
                        name: if !field_name.is_empty() { field_name } else { variable.clone() },
                        variable,
                        text: initial,
                        x: narrow(round(float(bounds.get("left"))), &what("field x"))?,
                        y: narrow(round(float(bounds.get("top"))), &what("field y"))?,
                        width: narrow(round(float(bounds.get("width"))), &what("field width"))?,
                        height: narrow(round(float(bounds.get("height"))), &what("field height"))?,
                        margin_left: narrow(rounded(field.get("leftMargin")), &what("field margin"))?,
                        margin_right: narrow(rounded(field.get("rightMargin")), &what("field margin"))?,
                        indent: narrow(rounded(field.get("indent")), &what("field indent"))?,
                        align: match int(field.get("align")) {
                            1 => 2,
                            2 => 1,
                            _ => 0,
                        },
                        word_wrap: u8::from(truthy(field.get("wordWrap"))),
                        multiline: u8::from(truthy(field.get("multiline"))),
                        leading: narrow(rounded(field.get("leading")), &what("field leading"))?,
                        style,
                    });
                }
            }
            let source_xtra = match m.get("xtra").and_then(|x| x.get("symbol")).and_then(Value::as_str) {
                Some("flash") => 1,
                Some("vectorShape") => 2,
                _ => 0,
            };
            members.push(Member {
                id: reference(int(m.get("cast")), int(m.get("number")))?,
                number: narrow(int(m.get("number")), &what("number"))?,
                cast: narrow(int(m.get("cast")), &what("cast"))?,
                kind: narrow(int(m.get("type")), &what("type"))?,
                width: narrow(int(m.get("width")), &what("width"))?,
                height: narrow(int(m.get("height")), &what("height"))?,
                reg_x: narrow(int(m.get("regX")), &what("regX"))?,
                reg_y: narrow(int(m.get("regY")), &what("regY"))?,
                name: text(m.get("name")),
                asset: text(m.get("asset")),
                text: if binary { String::new() } else { member_text },
                samples: narrow(int(m.get("frames")), &what("frames"))?,
                rate: narrow(int(m.get("rate")), &what("rate"))?,
                loop_start: narrow(int(m.get("loopStart")), &what("loopStart"))?,
                loop_end: narrow(int(m.get("loopEnd")), &what("loopEnd"))?,
                shape: narrow(int(m.get("shape")), &what("shape"))?,
                pattern: narrow(int(m.get("pattern")), &what("pattern"))?,
                filled: narrow(int(m.get("filled")), &what("filled"))?,
                line_width: narrow(int(m.get("lineWidth")), &what("lineWidth"))?,
                looping: narrow(int(m.get("looping")), &what("looping"))?,
                film_loop: narrow(int(m.get("filmLoop")), &what("filmLoop"))?,
                line_direction: narrow(int(m.get("lineDirection")), &what("lineDirection"))?,
                film_assets,
                editable: if extended { u8::from(truthy(m.get("editable"))) } else { 0 },
                text_bytes,
                source_bytes: if extended { narrow(int(m.get("sourceBytes")), &what("sourceBytes"))? } else { 0 },
                cues,
                style,
                insert_style,
                video_audio: if version == 5 { text(m.get("videoAudio")) } else { String::new() },
                video_flags: if version == 5 { narrow(int(m.get("videoFlags")), &what("videoFlags"))? } else { 0 },
                film_sounds,
                source_xtra: if version >= 10 { source_xtra } else { 0 },
                flash_labels,
                flash_fields,
            });
        }
        if members.len() > u16::MAX as usize {
            return Err(format!("{name}: too many members"));
        }
        // The folded-name index, stable for equal hashes (director.py sorts
        // the same way).
        let mut member_index: Vec<(u32, u16)> =
            members.iter().enumerate().map(|(i, m)| (text_hash(&m.name), i as u16)).collect();
        member_index.sort_by_key(|entry| entry.0);
        let mut casts = Vec::new();
        for c in &casts_json {
            let file = text(c.get("file"));
            casts.push(Cast {
                name: text(c.get("name")),
                file: id_of(&file)? as u16,
                cast: if file == name { narrow(int(c.get("number")), "cast number")? } else { 1 },
            });
        }
        let mut frames = Vec::new();
        let mut deltas: Vec<Delta> = Vec::new();
        let empty = Vec::new();
        let score_frames = movie.get("score").and_then(|s| s.get("frames")).and_then(Value::as_array).unwrap_or(&empty);
        let (record_size, channel_limit) = if version >= 7 { (48, if version >= 10 { 1006 } else { 806 }) } else { (24, 126) };
        for frame in score_frames {
            let channels = frame["channels"].as_array().ok_or("frame without channels")?;
            frames.push((deltas.len() as u32, narrow(channels.len() as i64, "frame channels")?));
            for channel in channels {
                let b: Vec<u8> = channel["bytes"]
                    .as_array()
                    .ok_or("channel without bytes")?
                    .iter()
                    .map(|v| int(Some(v)) as u8)
                    .collect();
                let changed: Vec<bool> = channel["changed"]
                    .as_array()
                    .ok_or("channel without changed")?
                    .iter()
                    .map(|v| v.as_bool().unwrap_or_else(|| int(Some(v)) != 0))
                    .collect();
                let c = int(channel.get("channel"));
                if b.len() != record_size || changed.len() != record_size {
                    return Err(format!("{name}: invalid Director {version} score record size"));
                }
                if !(0..channel_limit).contains(&c) {
                    return Err(format!("{name}: score channel {c} exceeds profile"));
                }
                let u16_at = |n: usize| u16::from_be_bytes([b[n], b[n + 1]]) as i64;
                let i16_at = |n: usize| i16::from_be_bytes([b[n], b[n + 1]]);
                let i32_at = |n: usize| i32::from_be_bytes([b[n], b[n + 1], b[n + 2], b[n + 3]]);
                let member = if c == 1 || c == 5 {
                    0
                } else if c >= 6 {
                    reference(u16_at(4), u16_at(6))?
                } else {
                    reference(u16_at(0), u16_at(2))?
                };
                let behaviors = channel["behaviors"].as_array().cloned().unwrap_or_default();
                if behaviors.len() > 1 && !extended {
                    return Err("multiple behaviors require additional dispatch slots".into());
                }
                if behaviors.iter().any(|s| truthy(s.get("parameters"))) && !extended {
                    return Err("parameterized behavior requires extended runtime".into());
                }
                let mut script = match behaviors.first() {
                    Some(s) => reference(int(s.get("cast")), int(s.get("member")))?,
                    None => 0,
                };
                if c == 0 && script == 0 {
                    script = member;
                }
                let mut spec = if c >= 6 {
                    Spec {
                        member,
                        script,
                        x: i16_at(14),
                        y: i16_at(12),
                        width: i16_at(18),
                        height: i16_at(16),
                        ink: b[1] & 63,
                        blend: ((255 - b[21] as u32) * 100 / 255) as u8,
                        kind: b[0],
                        flags: b[20],
                        fore: b[2],
                        back: b[3],
                        thickness: b[22],
                        stretch: u8::from(b[1] & 128 != 0),
                        trails: u8::from(b[1] & 64 != 0),
                        ..Spec::default()
                    }
                } else {
                    let kind = if c == 1 && version < 7 {
                        if version == 5 { b[6] } else { score_tempo(&b, version, extended)? as u8 }
                    } else {
                        0
                    };
                    Spec { member, script, kind, ..Spec::default() }
                };
                if modern_fields {
                    spec.loc_z = if c >= 6 { (c - 5) as i16 } else { 0 };
                    spec.tempo = if c == 1 && version == 5 {
                        if (1..=120).contains(&b[6]) { b[6] as u16 } else { 0 }
                    } else if c == 1 {
                        score_tempo(&b, version, extended)?
                    } else {
                        0
                    };
                    spec.delay = if c == 1 && version == 5 {
                        if b[6] >= 196 { 256 - b[6] as u16 } else { 0 }
                    } else if c == 1 && b[6] == 247 {
                        u16_at(4) as u16
                    } else {
                        0
                    };
                    let rgb = |index: usize, g: usize, blue: usize| {
                        if c < 6 {
                            0
                        } else if version < 7 {
                            palette[b[index] as usize]
                        } else {
                            (b[index] as u32) << 16 | (b[g] as u32) << 8 | b[blue] as u32
                        }
                    };
                    spec.fore_rgb = rgb(2, 24, 26);
                    spec.back_rgb = rgb(3, 25, 27);
                    if c >= 6 && version >= 7 {
                        spec.rotation = i32_at(28);
                        spec.skew = i32_at(32);
                    }
                }
                if extended {
                    spec.behaviors = behaviors
                        .iter()
                        .map(|s| Ok((reference(int(s.get("cast")), int(s.get("member")))?, text(s.get("parameters")))))
                        .collect::<Result<_, String>>()?;
                }
                let mut mask: u16 = 0;
                let any = |range: &[usize]| range.iter().any(|&i| changed[i]);
                for (flag, indexes) in [
                    (1u16, &[4usize, 5, 6, 7][..]),
                    (2, &[12, 13, 14, 15]),
                    (4, &[18, 19]),
                    (8, &[1]),
                    (16, &[21]),
                    (32, &[0]),
                    (64, &[16, 17]),
                    (128, &[2]),
                    (256, &[3]),
                    (512, &[22]),
                    (1024, &[20]),
                ] {
                    if any(indexes) {
                        mask |= flag;
                    }
                }
                if version >= 7 {
                    for (flag, indexes) in
                        [(128u16, &[20usize, 24, 26][..]), (256, &[20, 25, 27]), (2048, &[28, 29, 30, 31]), (4096, &[32, 33, 34, 35])]
                    {
                        if any(indexes) {
                            mask |= flag;
                        }
                    }
                }
                if c >= 6 && changed.iter().all(|&x| x) {
                    mask |= DG_SPAN;
                }
                deltas.push(Delta { channel: c as u16, mask, spec });
            }
        }
        let labels = movie
            .get("score")
            .and_then(|s| s.get("labels"))
            .and_then(Value::as_array)
            .map(|l| -> Result<Vec<(String, u16)>, String> {
                l.iter().map(|x| Ok((text(x.get("name")), narrow(int(x.get("frame")), "label frame")?))).collect()
            })
            .transpose()?
            .unwrap_or_default();
        let stage_color = if modern_fields {
            match movie.get("stageColorRGB") {
                Some(v) => int(Some(v)) as u32,
                None => palette[(int(movie.get("stageColor")) & 255) as usize],
            }
        } else {
            0
        };
        result.push(Scene {
            stem: name.replace('.', "_").to_lowercase(),
            name,
            id: narrow(int(movie.get("id")), "movie id")?,
            tempo: narrow(int(movie.get("tempo")), "movie tempo")?,
            casts,
            members,
            member_index,
            palette,
            frames,
            deltas,
            labels,
            stage_color,
            styles: styles.table,
        });
    }
    Ok(result)
}

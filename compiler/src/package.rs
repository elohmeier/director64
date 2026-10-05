//! The game package (docs/package-format.md): a compiled program and its
//! scene tables in one versioned, little-endian binary that a prebuilt
//! runtime validates and loads.

use std::collections::HashMap;

use sha2::{Digest, Sha256};

use crate::emit::{fold, Compiled};
use crate::names::Names;
use crate::scene::{Profile, Scene, TextStyle};

pub const FORMAT: u16 = 2;

/// The runtime ABI a package is compiled against: name ids and opcodes are
/// positions in these two files.
pub fn abi_digest(names_txt: &[u8], bytecode_header: &[u8]) -> [u8; 16] {
    let mut hasher = Sha256::new();
    hasher.update(names_txt);
    hasher.update(bytecode_header);
    let digest = hasher.finalize();
    let mut out = [0u8; 16];
    out.copy_from_slice(&digest[..16]);
    out
}

#[derive(Default)]
struct Strings {
    bytes: Vec<u8>,
    offsets: HashMap<String, u32>,
}

impl Strings {
    fn add(&mut self, text: &str) -> Result<u32, String> {
        if text.as_bytes().contains(&0) {
            return Err(format!("string with an embedded NUL: {text:?}"));
        }
        if let Some(&offset) = self.offsets.get(text) {
            return Ok(offset);
        }
        let offset = u32::try_from(self.bytes.len()).map_err(|_| "string pool exceeds 4 GiB")?;
        self.bytes.extend_from_slice(text.as_bytes());
        self.bytes.push(0);
        self.offsets.insert(text.to_string(), offset);
        Ok(offset)
    }
}

#[derive(Default)]
struct Out(Vec<u8>);

impl Out {
    fn u8(&mut self, v: u8) {
        self.0.push(v);
    }
    fn u16(&mut self, v: u16) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn i16(&mut self, v: i16) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn i8(&mut self, v: i8) {
        self.0.push(v as u8);
    }
    fn i32(&mut self, v: i32) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn u32(&mut self, v: u32) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn f64(&mut self, v: f64) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn count(&mut self, n: usize) -> Result<(), String> {
        self.u32(u32::try_from(n).map_err(|_| "count exceeds u32")?);
        Ok(())
    }
}

fn u16_of(value: usize, what: &str) -> Result<u16, String> {
    u16::try_from(value).map_err(|_| format!("{what} {value} exceeds u16"))
}

pub fn write(
    compiled: &Compiled,
    names: &Names,
    scenes: &[Scene],
    profile: Profile,
    abi: [u8; 16],
    meta: &str,
) -> Result<Vec<u8>, String> {
    let mut strings = Strings::default();
    // Offset 0 is the empty string, and the pool always ends with a NUL.
    strings.add("")?;
    let mut code: Vec<u8> = Vec::new();
    let mut symbols = Out::default();
    symbols.count(compiled.symbols.len())?;
    symbols.count(compiled.symbol_bucket_count)?;
    for text in &compiled.symbols {
        symbols.u32(strings.add(text)?);
    }
    for &start in &compiled.symbol_buckets {
        symbols.count(start)?;
    }
    let mut globals = Out::default();
    globals.count(compiled.globals.len())?;
    for name in &compiled.globals {
        globals.u32(strings.add(name)?);
    }
    let units: HashMap<&str, usize> =
        compiled.units.iter().enumerate().map(|(i, u)| (u.stem.as_str(), i)).collect();
    let mut movies = Out::default();
    movies.count(scenes.len())?;
    for scene in scenes {
        let unit = &compiled.units[*units
            .get(scene.stem.as_str())
            .ok_or_else(|| format!("{}: scene without a bytecode unit", scene.name))?];
        movies.u32(strings.add(&unit.movie)?);
        // Code blocks are shared by handlers with identical bytecode.
        let mut placed: Vec<(u32, u32)> = Vec::with_capacity(unit.blocks.len());
        for (_, bytes, _) in &unit.blocks {
            let offset = u32::try_from(code.len()).map_err(|_| "code exceeds 4 GiB")?;
            code.extend_from_slice(bytes);
            placed.push((offset, bytes.len() as u32));
        }
        movies.count(unit.entries.len())?;
        for entry in &unit.entries {
            movies.u32(strings.add(&entry.name)?);
            movies.u32(u32::from(u16_of(entry.member as usize, "handler member")?));
            movies.u32(strings.add(&entry.cast)?);
            movies.u32(strings.add(&entry.kind)?);
            movies.count(entry.arguments)?;
            movies.count(entry.locals.len())?;
            movies.count(entry.entry)?;
            movies.count(entry.properties.len())?;
            for local in &entry.locals {
                movies.u32(strings.add(local)?);
            }
            for property in &entry.properties {
                movies.u32(strings.add(property)?);
            }
            let (offset, size) = placed[entry.block];
            movies.u32(offset);
            movies.u32(size);
        }
        movies.count(unit.names.len())?;
        for (name, used) in unit.names.iter().zip(&unit.symbol_use) {
            movies.u32(strings.add(name)?);
            movies.u16(names.id(name));
            movies.u16(if *used { u16_of(compiled.symbol_ids[&fold(name)], "symbol id")? } else { 0xFFFF });
        }
        movies.count(unit.doubles.len())?;
        for &value in &unit.doubles {
            movies.f64(value);
        }
        movies.count(unit.bucket_count())?;
        if !unit.entries.is_empty() {
            for &i in &unit.order {
                movies.u16(u16_of(i, "handler order")?);
            }
            for &i in &unit.buckets {
                movies.u16(u16_of(i, "handler bucket")?);
            }
        }
        movies.u16(scene.id);
        movies.u16(scene.tempo);
        if profile.modern() {
            movies.u32(scene.stage_color);
            movies.u16(u16_of(scene.styles.len(), "text style count")?);
            for style in &scene.styles {
                write_style(&mut movies, &mut strings, style, profile)?;
            }
        }
        movies.u16(u16_of(scene.casts.len(), "cast count")?);
        for cast in &scene.casts {
            movies.u32(strings.add(&cast.name)?);
            movies.u16(cast.file);
            movies.u16(cast.cast);
        }
        movies.count(scene.members.len())?;
        for m in &scene.members {
            movies.u32(m.id);
            for v in [m.number, m.cast, m.kind, m.width, m.height] {
                movies.u16(v);
            }
            movies.i16(m.reg_x);
            movies.i16(m.reg_y);
            for s in [&m.name, &m.asset, &m.text] {
                movies.u32(strings.add(s)?);
            }
            for v in [m.samples, m.rate, m.loop_start, m.loop_end] {
                movies.u32(v);
            }
            movies.u16(m.shape);
            movies.u16(m.pattern);
            for v in [m.filled, m.line_width, m.looping, m.film_assets.len() as u8, m.film_loop, m.line_direction] {
                movies.u8(v);
            }
            for asset in &m.film_assets {
                movies.u32(strings.add(asset)?);
            }
            if profile.extended {
                movies.u8(m.editable);
                match &m.text_bytes {
                    // Binary text follows inline, NUL-terminated.
                    Some(bytes) => {
                        movies.count(bytes.len())?;
                        movies.0.extend_from_slice(bytes);
                        movies.u8(0);
                    }
                    None => movies.u32(0),
                }
                movies.u32(m.source_bytes);
                movies.u8(u8::try_from(m.cues.len()).map_err(|_| "cue count exceeds u8")?);
                for (milliseconds, name) in &m.cues {
                    movies.u32(*milliseconds);
                    movies.u32(strings.add(name)?);
                }
            }
            if profile.modern() {
                movies.u16(m.style.map_or(0, |i| i + 1));
                movies.u16(m.insert_style.map_or(0, |i| i + 1));
            }
            if profile.d5() {
                movies.u32(strings.add(&m.video_audio)?);
                movies.u32(m.video_flags);
                movies.count(m.film_sounds.len())?;
                for &v in &m.film_sounds {
                    movies.u32(v);
                }
            }
            if profile.d10() {
                movies.u8(m.source_xtra);
                movies.u8(u8::try_from(m.flash_labels.len()).map_err(|_| "flash label count exceeds u8")?);
                movies.u8(u8::try_from(m.flash_fields.len()).map_err(|_| "flash field count exceeds u8")?);
                for (name, frame) in &m.flash_labels {
                    movies.u32(strings.add(name)?);
                    movies.u16(*frame);
                }
                for f in &m.flash_fields {
                    for s in [&f.name, &f.variable, &f.text] {
                        movies.u32(strings.add(s)?);
                    }
                    for v in [f.x, f.y, f.width, f.height, f.margin_left, f.margin_right, f.indent] {
                        movies.i16(v);
                    }
                    movies.u8(f.align);
                    movies.u8(f.word_wrap);
                    movies.u8(f.multiline);
                    movies.i8(f.leading);
                    movies.u16(f.style.map_or(0, |i| i + 1));
                }
            }
        }
        for &(hash, member) in &scene.member_index {
            movies.u32(hash);
            movies.u16(member);
        }
        for &color in &scene.palette {
            movies.u32(color);
        }
        movies.count(scene.frames.len())?;
        for &(first, count) in &scene.frames {
            movies.u32(first);
            movies.u16(count);
        }
        movies.count(scene.deltas.len())?;
        for delta in &scene.deltas {
            let s = &delta.spec;
            movies.u16(delta.channel);
            movies.u16(delta.mask);
            movies.u32(s.member);
            movies.u32(s.script);
            for v in [s.x, s.y, s.width, s.height] {
                movies.i16(v);
            }
            for v in [s.ink, s.blend, s.kind, s.flags, s.fore, s.back, s.thickness, s.stretch, s.trails] {
                movies.u8(v);
            }
            if profile.modern() {
                movies.i16(s.loc_z);
                movies.u16(s.tempo);
                movies.u16(s.delay);
                movies.u32(s.fore_rgb);
                movies.u32(s.back_rgb);
                movies.i32(s.rotation);
                movies.i32(s.skew);
            }
            if profile.extended {
                movies.u8(u8::try_from(s.behaviors.len()).map_err(|_| "behavior count exceeds u8")?);
                for (script, parameters) in &s.behaviors {
                    movies.u32(*script);
                    movies.u32(strings.add(parameters)?);
                }
            }
        }
        movies.u16(u16_of(scene.labels.len(), "label count")?);
        for (name, frame) in &scene.labels {
            movies.u32(strings.add(name)?);
            movies.u16(*frame);
        }
    }
    let sections: Vec<(&[u8; 4], Vec<u8>)> = vec![
        (b"META", meta.as_bytes().to_vec()),
        (b"STRS", strings.bytes),
        (b"CODE", code),
        (b"SYMB", symbols.0),
        (b"GLOB", globals.0),
        (b"MOVI", movies.0),
    ];
    let mut out = Out::default();
    out.0.extend_from_slice(b"D64P");
    out.u16(FORMAT);
    out.u16(profile.version);
    out.u16(u16::from(profile.extended));
    out.u16(sections.len() as u16);
    out.0.extend_from_slice(&abi);
    out.u32(0);
    let mut offset = 32 + 12 * sections.len();
    for (tag, body) in &sections {
        out.0.extend_from_slice(*tag);
        out.count(offset)?;
        out.count(body.len())?;
        offset += body.len();
    }
    for (_, body) in sections {
        out.0.extend_from_slice(&body);
    }
    Ok(out.0)
}

/// One dg_text_style_t: str font name; u8 font id, size, align; i16 ascent,
/// descent, leading, line height; u32 color; in extended runtimes u16
/// advance count (0xFFFF: none) with the advances, u32 kerning pair count and
/// u32 kerning value count with the values.
fn write_style(out: &mut Out, strings: &mut Strings, style: &TextStyle, profile: Profile) -> Result<(), String> {
    out.u32(strings.add(&style.font_name)?);
    out.u8(style.font_id);
    out.u8(style.size);
    out.u8(style.align);
    for v in [style.ascent, style.descent, style.leading, style.line_height] {
        out.i16(v);
    }
    out.u32(style.color);
    if profile.extended {
        match &style.advances {
            Some(advances) => {
                out.u16(u16_of(advances.len(), "advance count").and_then(|n| {
                    if n == 0xFFFF { Err("advance count 65535".into()) } else { Ok(n) }
                })?);
                out.0.extend_from_slice(advances);
                out.u32(style.kerning_count);
                out.count(style.kerning.len())?;
                for &v in &style.kerning {
                    out.i16(v);
                }
            }
            None => out.u16(0xFFFF),
        }
    }
    Ok(())
}

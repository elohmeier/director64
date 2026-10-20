//! Offline conversion of the static SWF envelopes used by D8/D10 vectorShape
//! XMED and D10 Flash members (tools/director/swf-model.mjs). Static
//! visuals flatten: shapes (solid and gradient fills, line styles, curves),
//! nested sprite timelines, embedded font glyphs and static text. The
//! dynamic surface is recorded, never guessed. Every floating-point
//! expression keeps the reference's evaluation order, so the pixels match.

use std::collections::HashMap;

use serde_json::{Value, json};

use super::js;

#[path = "swf_audio.rs"]
pub mod audio;
#[path = "swf_filter.rs"]
pub mod filters;
#[path = "swf_helpers.rs"]
mod helpers;
#[path = "swf_morph.rs"]
mod morph;
#[path = "swf_runtime.rs"]
mod runtime;

type R<T> = Result<T, String>;

fn u16le(b: &[u8], at: usize) -> R<usize> {
    b.get(at..at + 2)
        .map(|s| u16::from_le_bytes([s[0], s[1]]) as usize)
        .ok_or_else(|| "truncated SWF record".into())
}
fn i16le(b: &[u8], at: usize) -> R<i64> {
    b.get(at..at + 2)
        .map(|s| i16::from_le_bytes([s[0], s[1]]) as i64)
        .ok_or_else(|| "truncated SWF record".into())
}
fn u32le(b: &[u8], at: usize) -> R<usize> {
    b.get(at..at + 4)
        .map(|s| u32::from_le_bytes(s.try_into().unwrap()) as usize)
        .ok_or_else(|| "truncated SWF record".into())
}
fn byte(b: &[u8], at: usize) -> R<u8> {
    b.get(at)
        .copied()
        .ok_or_else(|| "truncated SWF record".into())
}
fn jmax(a: f64, b: f64) -> f64 {
    if a.is_nan() || b.is_nan() {
        f64::NAN
    } else {
        a.max(b)
    }
}
fn jmin(a: f64, b: f64) -> f64 {
    if a.is_nan() || b.is_nan() {
        f64::NAN
    } else {
        a.min(b)
    }
}
/// `parseInt(value, 10)`: leading whitespace, a sign, then digits.
fn parse_int(value: &str) -> f64 {
    let t = value.trim_start_matches(super::source::js_space);
    let (sign, digits) = match t.as_bytes().first() {
        Some(b'-') => (-1.0, &t[1..]),
        Some(b'+') => (1.0, &t[1..]),
        _ => (1.0, t),
    };
    let end = digits
        .find(|c: char| !c.is_ascii_digit())
        .unwrap_or(digits.len());
    if end == 0 {
        f64::NAN
    } else {
        sign * digits[..end].parse::<f64>().unwrap()
    }
}
fn index_of(b: &[u8], value: u8, from: usize) -> Option<usize> {
    b.get(from..)?
        .iter()
        .position(|&x| x == value)
        .map(|i| i + from)
}

struct Bits<'a> {
    bytes: &'a [u8],
    bit: usize,
}

impl<'a> Bits<'a> {
    fn new(bytes: &'a [u8], offset: usize) -> Self {
        Self {
            bytes,
            bit: offset * 8,
        }
    }
    fn unsigned(&mut self, count: usize) -> R<f64> {
        if count > 32 || self.bit + count > self.bytes.len() * 8 {
            return Err("truncated SWF bit field".into());
        }
        let mut value = 0f64;
        for _ in 0..count {
            value = value * 2.0 + ((self.bytes[self.bit >> 3] >> (7 - (self.bit & 7))) & 1) as f64;
            self.bit += 1;
        }
        Ok(value)
    }
    fn signed(&mut self, count: usize) -> R<f64> {
        let value = self.unsigned(count)?;
        Ok(if count > 0 && value >= 2f64.powi(count as i32 - 1) {
            value - 2f64.powi(count as i32)
        } else {
            value
        })
    }
    fn fixed16(&mut self, count: usize) -> R<f64> {
        Ok(self.signed(count)? / 65536.0)
    }
    fn align(&mut self) -> usize {
        self.bit = self.bit.div_ceil(8) * 8;
        self.bit / 8
    }
    fn rect(&mut self) -> R<[f64; 4]> {
        let count = self.unsigned(5)? as usize;
        let r = [
            self.signed(count)?,
            self.signed(count)?,
            self.signed(count)?,
            self.signed(count)?,
        ];
        self.align();
        Ok(r)
    }
    fn matrix(&mut self) -> R<Matrix> {
        let (mut a, mut d, mut b, mut c) = (1.0, 1.0, 0.0, 0.0);
        if self.unsigned(1)? != 0.0 {
            let n = self.unsigned(5)? as usize;
            a = self.fixed16(n)?;
            d = self.fixed16(n)?;
        }
        if self.unsigned(1)? != 0.0 {
            let n = self.unsigned(5)? as usize;
            b = self.fixed16(n)?;
            c = self.fixed16(n)?;
        }
        let n = self.unsigned(5)? as usize;
        let tx = self.signed(n)?;
        let ty = self.signed(n)?;
        self.align();
        Ok(Matrix { a, b, c, d, tx, ty })
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Matrix {
    a: f64,
    b: f64,
    c: f64,
    d: f64,
    tx: f64,
    ty: f64,
}

fn multiply(o: &Matrix, i: &Matrix) -> Matrix {
    Matrix {
        a: o.a * i.a + o.c * i.b,
        b: o.b * i.a + o.d * i.b,
        c: o.a * i.c + o.c * i.d,
        d: o.b * i.c + o.d * i.d,
        tx: o.a * i.tx + o.c * i.ty + o.tx,
        ty: o.b * i.tx + o.d * i.ty + o.ty,
    }
}

const CURVE_SEGMENTS: usize = 8;

#[derive(Clone)]
struct Gradient {
    matrix: Matrix,
    stops: Vec<(f64, [f64; 4])>,
    radial: bool,
}

#[derive(Clone)]
enum Fill {
    Color([f64; 4]),
    Gradient(Gradient),
    Bitmap(BitmapFill),
}

/// A decoded SWF bitmap character, straight (not premultiplied) RGBA.
struct Bitmap {
    width: usize,
    height: usize,
    rgba: Vec<[u8; 4]>,
}

#[derive(Clone)]
struct BitmapFill {
    bitmap: Option<std::rc::Rc<Bitmap>>,
    matrix: Matrix,
    clipped: bool,
}

#[derive(Clone)]
struct Line {
    width: f64,
    color: [f64; 4],
}

#[derive(Clone, Copy)]
struct Edge {
    x0: f64,
    y0: f64,
    x1: f64,
    y1: f64,
    f0: usize,
    f1: usize,
    ln: usize,
}

#[derive(Clone)]
struct Shape {
    fills: Vec<Fill>,
    lines: Vec<Line>,
    edges: Vec<Edge>,
}

struct Styles<'a> {
    p: usize,
    fills: Vec<Fill>,
    lines: Vec<Line>,
    bitmaps: &'a HashMap<usize, std::rc::Rc<Bitmap>>,
}

fn color(b: &[u8], p: usize, alpha: bool) -> R<[f64; 4]> {
    Ok([
        byte(b, p)? as f64,
        byte(b, p + 1)? as f64,
        byte(b, p + 2)? as f64,
        if alpha { byte(b, p + 3)? as f64 } else { 255.0 },
    ])
}

fn gradient_style(
    b: &[u8],
    state: &mut Styles,
    version: u32,
    radial: bool,
    focal: bool,
) -> R<Fill> {
    let mut bits = Bits::new(b, state.p);
    let matrix = bits.matrix()?;
    let mut p = bits.align();
    let head = byte(b, p)?;
    p += 1;
    let count = head & 15;
    if count == 0 {
        return Err("empty SWF gradient".into());
    }
    let mut stops = Vec::new();
    for _ in 0..count {
        let ratio = byte(b, p)? as f64;
        p += 1;
        stops.push((ratio, color(b, p, version >= 3)?));
        p += if version >= 3 { 4 } else { 3 };
    }
    if focal {
        let _focus = i16le(b, p)? as f64 / 256.0;
        p += 2;
    }
    state.p = p;
    Ok(Fill::Gradient(Gradient {
        matrix,
        stops,
        radial,
    }))
}

fn style_tables(b: &[u8], state: &mut Styles, version: u32) -> R<(usize, usize)> {
    let mut p = state.p;
    let read_count = |p: &mut usize| -> R<usize> {
        let mut n = byte(b, *p)? as usize;
        *p += 1;
        if n == 255 {
            n = u16le(b, *p)?;
            *p += 2;
        }
        Ok(n)
    };
    let (fill_base, line_base) = (state.fills.len(), state.lines.len());
    let fill_count = read_count(&mut p)?;
    for _ in 0..fill_count {
        let kind = byte(b, p)?;
        p += 1;
        if kind == 0 {
            state.fills.push(Fill::Color(color(b, p, version >= 3)?));
            p += if version >= 3 { 4 } else { 3 };
        } else if kind == 0x10 || kind == 0x12 || kind == 0x13 {
            state.p = p;
            let fill = gradient_style(b, state, version, kind != 0x10, kind == 0x13)?;
            state.fills.push(fill);
            p = state.p;
        } else if (0x40..=0x43).contains(&kind) {
            let id = u16le(b, p)?;
            let mut bits = Bits::new(b, p + 2);
            let matrix = bits.matrix()?;
            p = bits.align();
            let bitmap = match state.bitmaps.get(&id) {
                Some(found) => Some(found.clone()),
                None if id == 0xFFFF => None,
                None => return Err("SWF bitmap fill references absent bitmap".into()),
            };
            state.fills.push(Fill::Bitmap(BitmapFill {
                bitmap,
                matrix,
                clipped: kind & 1 != 0,
            }));
        } else {
            return Err(format!("unsupported SWF fill style {kind:#x}"));
        }
    }
    let line_count = read_count(&mut p)?;
    for _ in 0..line_count {
        let width = u16le(b, p)? as f64;
        p += 2;
        if version == 4 {
            let first = byte(b, p)?;
            let join = (first >> 4) & 3;
            p += 2;
            if first & 8 != 0 {
                return Err("SWF stroke fill needs explicit conversion".into());
            }
            if join == 2 {
                p += 2;
            }
        }
        let alpha = version >= 3 || version == 4;
        state.lines.push(Line {
            width,
            color: color(b, p, alpha)?,
        });
        p += if alpha { 4 } else { 3 };
    }
    state.p = p;
    Ok((fill_base, line_base))
}

struct Collector {
    x: f64,
    y: f64,
    fill0: usize,
    fill1: usize,
    line: usize,
    fill_base: usize,
    line_base: usize,
    edges: Vec<Edge>,
}

impl Collector {
    fn new() -> Self {
        Self {
            x: 0.0,
            y: 0.0,
            fill0: 0,
            fill1: 0,
            line: 0,
            fill_base: 0,
            line_base: 0,
            edges: Vec::new(),
        }
    }
    fn push(&mut self, nx: f64, ny: f64) -> R<()> {
        let annotate = |style: usize, base: usize| if style != 0 { base + style } else { 0 };
        self.edges.push(Edge {
            x0: self.x,
            y0: self.y,
            x1: nx,
            y1: ny,
            f0: annotate(self.fill0, self.fill_base),
            f1: annotate(self.fill1, self.fill_base),
            ln: annotate(self.line, self.line_base),
        });
        self.x = nx;
        self.y = ny;
        if self.edges.len() > 262144 {
            return Err("SWF edge count exceeds conversion bounds".into());
        }
        Ok(())
    }
    fn curve(&mut self, cx: f64, cy: f64, ax: f64, ay: f64) -> R<()> {
        let (qx, qy) = (self.x + cx, self.y + cy);
        let (ex, ey) = (qx + ax, qy + ay);
        let (sx, sy) = (self.x, self.y);
        for i in 1..=CURVE_SEGMENTS {
            let t = i as f64 / CURVE_SEGMENTS as f64;
            let u = 1.0 - t;
            self.push(
                u * u * sx + 2.0 * u * t * qx + t * t * ex,
                u * u * sy + 2.0 * u * t * qy + t * t * ey,
            )?;
        }
        Ok(())
    }
}

fn shape_records(
    bits: &mut Bits,
    collector: &mut Collector,
    on_style: &mut Option<&mut dyn FnMut(usize, &mut Collector) -> R<usize>>,
) -> R<()> {
    let mut fill_bits = bits.unsigned(4)? as usize;
    let mut line_bits = bits.unsigned(4)? as usize;
    loop {
        if bits.unsigned(1)? != 0.0 {
            let straight = bits.unsigned(1)? != 0.0;
            let count = bits.unsigned(4)? as usize + 2;
            if straight {
                let (mut dx, mut dy) = (0.0, 0.0);
                if bits.unsigned(1)? != 0.0 {
                    dx = bits.signed(count)?;
                    dy = bits.signed(count)?;
                } else if bits.unsigned(1)? != 0.0 {
                    dy = bits.signed(count)?;
                } else {
                    dx = bits.signed(count)?;
                }
                let (x, y) = (collector.x + dx, collector.y + dy);
                collector.push(x, y)?;
            } else {
                let (cx, cy) = (bits.signed(count)?, bits.signed(count)?);
                let (ax, ay) = (bits.signed(count)?, bits.signed(count)?);
                collector.curve(cx, cy, ax, ay)?;
            }
        } else {
            let flags = bits.unsigned(5)? as u32;
            if flags == 0 {
                break;
            }
            let (mut mv, mut f0, mut f1, mut line) = (None, None, None, None);
            if flags & 1 != 0 {
                let count = bits.unsigned(5)? as usize;
                mv = Some((bits.signed(count)?, bits.signed(count)?));
            }
            if flags & 2 != 0 {
                f0 = Some(bits.unsigned(fill_bits)? as usize);
            }
            if flags & 4 != 0 {
                f1 = Some(bits.unsigned(fill_bits)? as usize);
            }
            if flags & 8 != 0 {
                line = Some(bits.unsigned(line_bits)? as usize);
            }
            if flags & 16 != 0 {
                let Some(callback) = on_style.as_mut() else {
                    return Err("SWF glyph style table".into());
                };
                let p = bits.align();
                let next = callback(p, collector)?;
                bits.bit = next * 8;
                fill_bits = bits.unsigned(4)? as usize;
                line_bits = bits.unsigned(4)? as usize;
                collector.fill0 = 0;
                collector.fill1 = 0;
                collector.line = 0;
            }
            if let Some((x, y)) = mv {
                collector.x = x;
                collector.y = y;
            }
            if let Some(v) = f0 {
                collector.fill0 = v;
            }
            if let Some(v) = f1 {
                collector.fill1 = v;
            }
            if let Some(v) = line {
                collector.line = v;
            }
        }
    }
    Ok(())
}

fn parse_shape(
    b: &[u8],
    version: u32,
    bitmaps: &HashMap<usize, std::rc::Rc<Bitmap>>,
) -> R<(usize, Shape)> {
    if b.len() < 4 {
        return Err("truncated SWF shape".into());
    }
    let id = u16le(b, 0)?;
    // Empty authoring shapes can use a zero-padded 18-byte RECT slot,
    // followed by empty fill/line tables and an empty edge stream.
    if version < 4 && b.len() == 24 && b[2] == 8 && b[3..].iter().all(|&v| v == 0) {
        return Ok((
            id,
            Shape {
                fills: Vec::new(),
                lines: Vec::new(),
                edges: Vec::new(),
            },
        ));
    }
    let mut bits = Bits::new(b, 2);
    bits.rect()?;
    if version == 4 {
        bits.rect()?;
        if (bits.unsigned(8)? as u32) & !3 != 0 {
            return Err("SWF shape4 winding/reserved flags need explicit conversion".into());
        }
    }
    let mut state = Styles {
        p: bits.align(),
        fills: Vec::new(),
        lines: Vec::new(),
        bitmaps,
    };
    let mut collector = Collector::new();
    let (fb, lb) = style_tables(b, &mut state, version)?;
    collector.fill_base = fb;
    collector.line_base = lb;
    bits.bit = state.p * 8;
    let state_cell = std::cell::RefCell::new(state);
    {
        let mut callback = |p: usize, collector: &mut Collector| -> R<usize> {
            let mut state = state_cell.borrow_mut();
            state.p = p;
            let (fb, lb) = style_tables(b, &mut state, version)?;
            collector.fill_base = fb;
            collector.line_base = lb;
            Ok(state.p)
        };
        let mut on_style: Option<&mut dyn FnMut(usize, &mut Collector) -> R<usize>> =
            Some(&mut callback);
        shape_records(&mut bits, &mut collector, &mut on_style)?;
    }
    let state = state_cell.into_inner();
    if bits.align() != b.len() {
        return Err("unconsumed SWF shape data".into());
    }
    for e in &collector.edges {
        if e.f0 > state.fills.len() || e.f1 > state.fills.len() || e.ln > state.lines.len() {
            return Err("SWF edge references absent style".into());
        }
    }
    Ok((
        id,
        Shape {
            fills: state.fills,
            lines: state.lines,
            edges: collector.edges,
        },
    ))
}

fn parse_glyph(b: &[u8], start: usize, end: usize) -> R<Shape> {
    let mut collector = Collector::new();
    let mut bits = Bits::new(b, start);
    shape_records(&mut bits, &mut collector, &mut None)?;
    if bits.align() > end {
        return Err("SWF glyph exceeds its extent".into());
    }
    Ok(Shape {
        fills: vec![Fill::Color([0.0, 0.0, 0.0, 255.0])],
        lines: Vec::new(),
        edges: collector.edges,
    })
}

struct Font {
    name: String,
    glyphs: Vec<Shape>,
    codes: Option<Vec<usize>>,
    advances: Option<Vec<f64>>,
    ascent: Option<f64>,
    em: f64,
}

fn parse_font(b: &[u8], tag: usize) -> R<(usize, Font)> {
    let id = u16le(b, 0)?;
    if tag == 10 {
        let first = u16le(b, 2)?;
        let count = first / 2;
        let mut offsets = Vec::new();
        for i in 0..count {
            offsets.push(2 + u16le(b, 2 + i * 2)?);
        }
        offsets.push(b.len());
        let glyphs = (0..count)
            .map(|i| parse_glyph(b, offsets[i], offsets[i + 1]))
            .collect::<R<_>>()?;
        return Ok((
            id,
            Font {
                name: String::new(),
                glyphs,
                codes: None,
                advances: None,
                ascent: None,
                em: 1024.0,
            },
        ));
    }
    let flags = byte(b, 2)?;
    let wide_offsets = flags & 8 != 0;
    let wide_codes = tag == 75 || flags & 4 != 0;
    let has_layout = flags & 0x80 != 0;
    let name_length = byte(b, 4)? as usize;
    let name = js::latin1(b.get(5..5 + name_length).ok_or("truncated SWF font")?)
        .trim_end_matches('\0')
        .to_string();
    let mut p = 5 + name_length;
    let count = u16le(b, p)?;
    p += 2;
    let table = p;
    let mut offsets = Vec::new();
    for i in 0..=count {
        offsets.push(
            table
                + if wide_offsets {
                    u32le(b, table + i * 4)?
                } else {
                    u16le(b, table + i * 2)?
                },
        );
    }
    let glyphs = (0..count)
        .map(|i| parse_glyph(b, offsets[i], offsets[i + 1]))
        .collect::<R<_>>()?;
    p = offsets[count];
    let mut codes = Vec::new();
    for _ in 0..count {
        codes.push(if wide_codes {
            u16le(b, p)?
        } else {
            byte(b, p)? as usize
        });
        p += if wide_codes { 2 } else { 1 };
    }
    let (mut ascent, mut advances) = (None, None);
    if has_layout {
        ascent = Some(i16le(b, p)? as f64);
        p += 6;
        let mut list = Vec::new();
        for _ in 0..count {
            list.push(i16le(b, p)? as f64);
            p += 2;
        }
        advances = Some(list);
    }
    Ok((
        id,
        Font {
            name,
            glyphs,
            codes: Some(codes),
            advances,
            ascent,
            em: if tag == 75 { 20480.0 } else { 1024.0 },
        },
    ))
}

struct TextRecord {
    font: Option<usize>,
    color: [f64; 4],
    height: f64,
    glyphs: Vec<(usize, f64, f64)>,
}
struct Text {
    matrix: Matrix,
    records: Vec<TextRecord>,
}

fn parse_define_text(b: &[u8], version: u32) -> R<(usize, Text)> {
    let id = u16le(b, 0)?;
    let mut bits = Bits::new(b, 2);
    bits.rect()?;
    let matrix = bits.matrix()?;
    let mut p = bits.align();
    let glyph_bits = byte(b, p)? as usize;
    let advance_bits = byte(b, p + 1)? as usize;
    p += 2;
    let mut records = Vec::new();
    let (mut font, mut color_value, mut x, mut y, mut height) =
        (None, [0.0, 0.0, 0.0, 255.0], 0.0, 0.0, 240.0);
    while b.get(p).is_some_and(|&v| v != 0) {
        let flags = byte(b, p)?;
        p += 1;
        if flags & 0x80 == 0 {
            return Err("unsupported SWF text record".into());
        }
        if flags & 8 != 0 {
            font = Some(u16le(b, p)?);
            p += 2;
        }
        if flags & 4 != 0 {
            color_value = color(b, p, version == 2)?;
            p += if version == 2 { 4 } else { 3 };
        }
        if flags & 1 != 0 {
            x = i16le(b, p)? as f64;
            p += 2;
        }
        if flags & 2 != 0 {
            y = i16le(b, p)? as f64;
            p += 2;
        }
        if flags & 8 != 0 {
            height = u16le(b, p)? as f64;
            p += 2;
        }
        let glyph_count = byte(b, p)? as usize;
        p += 1;
        let mut entry = Bits::new(b, p);
        let mut glyphs = Vec::new();
        let mut pen = x;
        for _ in 0..glyph_count {
            let index = entry.unsigned(glyph_bits)? as usize;
            let advance = entry.signed(advance_bits)?;
            glyphs.push((index, pen, y));
            pen += advance;
        }
        x = pen;
        p = entry.align();
        records.push(TextRecord {
            font,
            color: color_value,
            height,
            glyphs,
        });
    }
    Ok((id, Text { matrix, records }))
}

#[derive(Clone)]
struct Field {
    embed_fonts: bool,
    bounds: [f64; 4],
    word_wrap: bool,
    multiline: bool,
    html: bool,
    font: Option<usize>,
    font_height: f64,
    color: [f64; 4],
    align: f64,
    left_margin: f64,
    right_margin: f64,
    indent: f64,
    leading: f64,
    variable: String,
    text: String,
}

fn decode_text(bytes: &[u8], utf8: bool) -> String {
    if utf8 {
        String::from_utf8_lossy(bytes.strip_prefix(b"\xEF\xBB\xBF").unwrap_or(bytes)).into_owned()
    } else {
        js::windows_1252(bytes)
    }
}

fn parse_edit_text(b: &[u8], utf8: bool) -> R<(usize, Field)> {
    let id = u16le(b, 0)?;
    let mut bits = Bits::new(b, 2);
    let bounds = bits.rect()?;
    let mut p = bits.align();
    let flags = u16le(b, p)?;
    p += 2;
    let mut field = Field {
        embed_fonts: flags & 0x100 != 0,
        bounds,
        word_wrap: flags & 0x40 != 0,
        multiline: flags & 0x20 != 0,
        html: flags & 0x200 != 0,
        font: None,
        font_height: 240.0,
        color: [0.0, 0.0, 0.0, 255.0],
        align: 0.0,
        left_margin: 0.0,
        right_margin: 0.0,
        indent: 0.0,
        leading: 0.0,
        variable: String::new(),
        text: String::new(),
    };
    if flags & 0x10 != 0 {
        return Err("SWF password field needs explicit conversion".into());
    }
    if flags & 1 != 0 {
        field.font = Some(u16le(b, p)?);
        field.font_height = u16le(b, p + 2)? as f64;
        p += 4;
    }
    if flags & 0x8000 != 0 {
        let end = index_of(b, 0, p).ok_or("truncated SWF field")?;
        p = end + 1;
    }
    if flags & 4 != 0 {
        field.color = color(b, p, true)?;
        p += 4;
    }
    if flags & 2 != 0 {
        p += 2;
    }
    if flags & 0x2000 != 0 {
        field.align = byte(b, p)? as f64;
        field.left_margin = u16le(b, p + 1)? as f64;
        field.right_margin = u16le(b, p + 3)? as f64;
        field.indent = u16le(b, p + 5)? as f64;
        field.leading = i16le(b, p + 7)? as f64;
        p += 9;
    }
    let end = index_of(b, 0, p).unwrap_or(b.len());
    field.variable = decode_text(&b[p.min(b.len())..end], utf8);
    p = end + 1;
    if flags & 0x80 != 0 {
        let end = index_of(b, 0, p).unwrap_or(b.len());
        field.text = decode_text(&b[p.min(b.len())..end.max(p.min(b.len()))], utf8);
    }
    Ok((id, field))
}

/// The fixed HTML subset of authored field text, stripped to runs.
fn parse_field_html(html: &str) -> R<(String, HashMap<String, String>)> {
    let mut attributes = HashMap::new();
    let mut out = String::new();
    let chars: Vec<char> = html.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        if chars[i] == '<' {
            let mut j = i + 1;
            if j < chars.len() && chars[j] == '/' {
                j += 1;
            }
            let start = j;
            while j < chars.len() && chars[j].is_ascii_alphabetic() {
                j += 1;
            }
            let tag: String = chars[start..j].iter().collect::<String>().to_lowercase();
            // `\b` after the tag name, then [^>]* and '>'.
            let boundary =
                j >= chars.len() || !(chars[j].is_ascii_alphanumeric() || chars[j] == '_');
            if matches!(tag.as_str(), "p" | "font" | "br") && boundary {
                if let Some(close) = chars[j..].iter().position(|&c| c == '>') {
                    let attrs: String = chars[j..j + close].iter().collect();
                    let mut rest = attrs.as_str();
                    while let Some(eq) = rest.find("=\"") {
                        let key_start = rest[..eq]
                            .rfind(|c: char| !(c.is_ascii_alphanumeric() || c == '_'))
                            .map(|k| k + 1)
                            .unwrap_or(0);
                        let key = &rest[key_start..eq];
                        let value_start = eq + 2;
                        let Some(value_end) = rest[value_start..].find('"') else {
                            break;
                        };
                        if !key.is_empty() {
                            attributes.insert(
                                key.to_lowercase(),
                                rest[value_start..value_start + value_end].to_string(),
                            );
                        }
                        rest = &rest[value_start + value_end + 1..];
                    }
                    if tag == "br" {
                        out.push('\r');
                    }
                    i = j + close + 1;
                    continue;
                }
            }
        }
        out.push(chars[i]);
        i += 1;
    }
    if out.contains(['<', '>']) {
        return Err("unsupported SWF field markup".into());
    }
    Ok((out, attributes))
}

#[derive(Clone, Debug, PartialEq)]
enum HelperValue {
    Text(String),
    Variable(String),
    Member(String, String),
    Add(Box<HelperValue>, Box<HelperValue>),
}

// Translate only a completely understood straight-line function. This is a
// semantic recognizer, not a name/hash exception: unrecognized operations
// remain in conversion evidence and therefore fail the capability check.
fn numeric_helper(
    name: &str,
    parameters: &[String],
    body: &[u8],
    pool: &[String],
) -> Option<Value> {
    let mut p = 0;
    let mut stack = Vec::<HelperValue>::new();
    let mut result = None;
    while p < body.len() {
        let code = body[p];
        p += 1;
        if code == 0 {
            if p != body.len() {
                return None;
            }
            break;
        }
        let end = if code >= 128 {
            let n = u16le(body, p).ok()?;
            p += 2;
            p.checked_add(n)?
        } else {
            p
        };
        if end > body.len() {
            return None;
        }
        match code {
            0x96 => {
                while p < end {
                    let kind = body[p];
                    p += 1;
                    let text = match kind {
                        0 => {
                            let end_text = index_of(body, 0, p)?;
                            if end_text >= end {
                                return None;
                            }
                            let text = js::latin1(&body[p..end_text]);
                            p = end_text + 1;
                            text
                        }
                        8 => {
                            let index = *body.get(p)? as usize;
                            p += 1;
                            pool.get(index)?.clone()
                        }
                        9 => {
                            let index = u16le(body, p).ok()?;
                            p += 2;
                            pool.get(index)?.clone()
                        }
                        _ => return None,
                    };
                    if p > end {
                        return None;
                    }
                    stack.push(HelperValue::Text(text));
                }
            }
            0x1c => {
                let HelperValue::Text(name) = stack.pop()? else {
                    return None;
                };
                stack.push(HelperValue::Variable(name));
            }
            0x4e => {
                let HelperValue::Text(property) = stack.pop()? else {
                    return None;
                };
                let HelperValue::Variable(target) = stack.pop()? else {
                    return None;
                };
                stack.push(HelperValue::Member(target, property));
            }
            0x47 => {
                let right = stack.pop()?;
                let left = stack.pop()?;
                stack.push(HelperValue::Add(Box::new(left), Box::new(right)));
            }
            0x4f => {
                if result.is_some() {
                    return None;
                }
                let value = stack.pop()?;
                let HelperValue::Text(property) = stack.pop()? else {
                    return None;
                };
                let HelperValue::Variable(target) = stack.pop()? else {
                    return None;
                };
                let (operation, parameter) = match value {
                    HelperValue::Add(left, right) => {
                        if *left != HelperValue::Member(target.clone(), property.clone()) {
                            return None;
                        }
                        let HelperValue::Variable(parameter) = *right else {
                            return None;
                        };
                        (1, parameter)
                    }
                    HelperValue::Variable(parameter) => (2, parameter),
                    _ => return None,
                };
                let argument = parameters.iter().position(|p| p == &parameter)?;
                if argument > 255
                    || !matches!(
                        property.as_str(),
                        "_x" | "_y" | "_width" | "_height" | "_alpha" | "_rotation"
                    )
                {
                    return None;
                }
                result = Some(
                    json!({"name": name, "target": target, "property": property, "operation": operation, "argument": argument}),
                );
            }
            _ => return None,
        }
        p = end;
    }
    if stack.is_empty() { result } else { None }
}

fn parse_actions(payload: &[u8]) -> R<(Vec<String>, bool, Vec<Value>, bool)> {
    if let Some((helpers, stop)) = helpers::lower(payload) {
        return Ok((vec!["ConvertedFunction".into()], stop, helpers, false));
    }
    let mut ops = Vec::new();
    let mut helpers = Vec::new();
    let mut pool = Vec::new();
    let (mut p, mut stop, mut top_level, mut fullscreen) = (0usize, false, true, false);
    while p < payload.len() {
        let code = payload[p];
        p += 1;
        if code == 0 {
            break;
        }
        let length = if code >= 0x80 {
            let n = u16le(payload, p)?;
            p += 2;
            n
        } else {
            0
        };
        let end = p
            .checked_add(length)
            .filter(|&end| end <= payload.len())
            .ok_or("truncated SWF action")?;
        if code == 0x07 && top_level {
            stop = true;
        }
        if code == 0x88 {
            let count = u16le(payload, p)?;
            let mut q = p + 2;
            pool.clear();
            for _ in 0..count {
                let z = index_of(payload, 0, q)
                    .filter(|&z| z < end)
                    .ok_or("truncated SWF constant pool")?;
                pool.push(js::latin1(&payload[q..z]));
                q = z + 1;
            }
            if q != end {
                return Err("trailing SWF constant pool bytes".into());
            }
        }
        if code == 0x9b || code == 0x8e {
            if length < 2 {
                return Err("truncated SWF function header".into());
            }
            let size = u16le(payload, end - 2)?;
            let body_end = end
                .checked_add(size)
                .filter(|&end| end <= payload.len())
                .ok_or("truncated SWF function body")?;
            let mut converted = false;
            if code == 0x9b && top_level {
                let z = index_of(payload, 0, p)
                    .filter(|&z| z + 3 <= end)
                    .ok_or("truncated SWF function name")?;
                let name = js::latin1(&payload[p..z]);
                let count = u16le(payload, z + 1)?;
                let mut q = z + 3;
                let mut parameters = Vec::new();
                for _ in 0..count {
                    let z = index_of(payload, 0, q)
                        .filter(|&z| z < end - 2)
                        .ok_or("truncated SWF function parameter")?;
                    parameters.push(js::latin1(&payload[q..z]));
                    q = z + 1;
                }
                if q != end - 2 {
                    return Err("trailing SWF function header bytes".into());
                }
                if !name.is_empty() {
                    if let Some(helper) =
                        numeric_helper(&name, &parameters, &payload[end..body_end], &pool)
                    {
                        helpers.push(helper);
                        converted = true;
                    }
                }
            }
            p = body_end;
            ops.push(
                if converted {
                    "ConvertedFunction"
                } else if code == 0x8e {
                    "DefineFunction2"
                } else {
                    "DefineFunction"
                }
                .to_string(),
            );
            continue;
        }
        if code == 0x83 && top_level && &payload[p..end] == b"FSCommand:fullscreen\0true\0" {
            fullscreen = true;
            ops.push("HostFullscreen".into());
        } else {
            ops.push(format!("0x{code:x}"));
        }
        if code == 0x9d || code == 0x99 {
            top_level = false;
        }
        p = end;
    }
    Ok((ops, stop, helpers, fullscreen))
}

struct Tag<'a> {
    tag: usize,
    payload: &'a [u8],
}

fn tag_stream(b: &[u8], start: usize) -> R<(Vec<Tag<'_>>, usize)> {
    let mut tags = Vec::new();
    let mut p = start;
    while p < b.len() {
        if p + 2 > b.len() {
            return Err("truncated SWF tag".into());
        }
        let header = u16le(b, p)?;
        let tag = header >> 6;
        p += 2;
        let mut length = header & 63;
        if length == 63 {
            if p + 4 > b.len() {
                return Err("truncated SWF long tag".into());
            }
            length = u32le(b, p)?;
            p += 4;
        }
        if length > b.len() - p {
            return Err("SWF tag exceeds envelope".into());
        }
        tags.push(Tag {
            tag,
            payload: &b[p..p + length],
        });
        p += length;
    }
    Ok((tags, p))
}

fn shape_version(tag: usize) -> Option<u32> {
    match tag {
        2 => Some(1),
        22 => Some(2),
        32 => Some(3),
        83 => Some(4),
        _ => None,
    }
}
const SKIPPED: [usize; 9] = [9, 24, 43, 56, 69, 73, 74, 77, 88];

#[derive(Clone, Debug, PartialEq)]
struct Cxform {
    mult: [f64; 4],
    add: [f64; 4],
}

fn read_color_transform(bits: &mut Bits) -> R<Cxform> {
    let has_add = bits.unsigned(1)? != 0.0;
    let has_mult = bits.unsigned(1)? != 0.0;
    let count = bits.unsigned(4)? as usize;
    let mut mult = [256.0; 4];
    if has_mult {
        for m in &mut mult {
            *m = bits.signed(count)?;
        }
    }
    let mut add = [0.0; 4];
    if has_add {
        for a in &mut add {
            *a = bits.signed(count)?;
        }
    }
    bits.align();
    Ok(Cxform { mult, add })
}

#[derive(Default)]
pub struct Effects {
    pub filters: u32,
    pub applied_filters: u32,
    pub blend_modes: u32,
    pub clip_actions: u32,
}

#[derive(Clone)]
struct Placement {
    id: usize,
    ratio: u16,
    filters: Vec<filters::GradientBevel>,
    matrix: Matrix,
    cxform: Option<Cxform>,
    clip: Option<usize>,
    name: Option<String>,
    placed: Option<usize>,
    generation: u32,
    clip_actions: Vec<u8>,
    blend_mode: u8,
}

fn read_placement(tag: usize, payload: &[u8], depths: &mut Depths, effects: &mut Effects) -> R<()> {
    if tag == 4 {
        if payload.len() < 5 {
            return Err("truncated SWF placement".into());
        }
        let matrix = Bits::new(payload, 4).matrix()?;
        depths.insert(
            u16le(payload, 2)?,
            Placement {
                id: u16le(payload, 0)?,
                ratio: 0,
                filters: Vec::new(),
                matrix,
                cxform: None,
                clip: None,
                name: None,
                placed: None,
                generation: 0,
                clip_actions: Vec::new(),
                blend_mode: 0,
            },
        );
        return Ok(());
    }
    let extended = tag == 70;
    let flags = byte(payload, 0)?;
    let flags2 = if extended { byte(payload, 1)? } else { 0 };
    if extended && flags2 & !3 != 0 {
        return Err("SWF placement extras need explicit conversion".into());
    }
    let mut q = if extended { 4 } else { 3 };
    let depth = u16le(payload, if extended { 2 } else { 1 })?;
    let previous = depths.get(depth).cloned();
    let id = if flags & 2 != 0 {
        Some(u16le(payload, q)?)
    } else {
        previous.as_ref().map(|p| p.id)
    };
    q += if flags & 2 != 0 { 2 } else { 0 };
    let mut matrix = previous.as_ref().map(|p| p.matrix);
    let (mut cxform, mut clip, mut name) = (None, None, None);
    if flags & 4 != 0 {
        let mut m = Bits::new(payload, q);
        matrix = Some(m.matrix()?);
        q = m.align();
    }
    if flags & 8 != 0 {
        let mut m = Bits::new(payload, q);
        cxform = Some(read_color_transform(&mut m)?);
        q = m.align();
    }
    let ratio = if flags & 16 != 0 {
        let value = u16le(payload, q)? as u16;
        q += 2;
        value
    } else {
        previous.as_ref().map_or(0, |p| p.ratio)
    };
    if flags & 32 != 0 {
        let end = index_of(payload, 0, q).ok_or("SWF placement extras need explicit conversion")?;
        name = Some(js::latin1(&payload[q..end]));
        q = end + 1;
    }
    if flags & 64 != 0 {
        clip = Some(u16le(payload, q)?);
        q += 2;
    }
    let filters = if flags2 & 1 != 0 {
        let filters = filters::read_list(payload, &mut q)?;
        effects.applied_filters += filters.len() as u32;
        filters
    } else {
        previous
            .as_ref()
            .map_or_else(Vec::new, |p| p.filters.clone())
    };
    let blend_mode = if flags2 & 2 != 0 {
        effects.blend_modes += 1;
        let mode = byte(payload, q)?;
        q += 1;
        mode
    } else {
        previous.as_ref().map_or(0, |p| p.blend_mode)
    };
    let clip_actions = if flags & 128 != 0 {
        effects.clip_actions += 1;
        let bytes = payload[q..].to_vec();
        q = payload.len();
        bytes
    } else {
        previous
            .as_ref()
            .map_or_else(Vec::new, |p| p.clip_actions.clone())
    };
    if q != payload.len() {
        return Err("SWF placement extras need explicit conversion".into());
    }
    let valid = id.is_some_and(|i| i != 0)
        && matrix.is_some()
        && !((flags & 1 == 0) && previous.is_some())
        && !((flags & 1 != 0) && previous.is_none());
    if !valid {
        return Err("invalid SWF placement sequence".into());
    }
    let id = id.unwrap();
    let restarted = flags & 2 != 0 && previous.as_ref().is_none_or(|p| p.id != id);
    depths.insert(
        depth,
        Placement {
            id,
            ratio,
            filters,
            matrix: matrix.unwrap(),
            cxform: if flags & 8 != 0 {
                cxform
            } else {
                previous.as_ref().and_then(|p| p.cxform.clone())
            },
            clip: if flags & 64 != 0 {
                clip
            } else {
                previous.as_ref().and_then(|p| p.clip)
            },
            name: if flags & 32 != 0 {
                name
            } else {
                previous.as_ref().and_then(|p| p.name.clone())
            },
            placed: if restarted {
                None
            } else {
                previous.as_ref().and_then(|p| p.placed)
            },
            generation: if restarted {
                0
            } else {
                previous.as_ref().map_or(0, |p| p.generation)
            },
            clip_actions,
            blend_mode,
        },
    );
    Ok(())
}

/// A depth table with JS Map semantics: re-setting a depth keeps its
/// position, deleting and re-adding moves it to the end.
#[derive(Clone, Default)]
struct Depths(Vec<(usize, Placement)>, u32);

impl Depths {
    fn get(&self, depth: usize) -> Option<&Placement> {
        self.0.iter().find(|e| e.0 == depth).map(|e| &e.1)
    }
    fn get_mut(&mut self, depth: usize) -> Option<&mut Placement> {
        self.0.iter_mut().find(|e| e.0 == depth).map(|e| &mut e.1)
    }
    fn insert(&mut self, depth: usize, mut value: Placement) {
        if value.generation == 0 {
            self.1 += 1;
            value.generation = self.1;
        }
        match self.get_mut(depth) {
            Some(slot) => *slot = value,
            None => self.0.push((depth, value)),
        }
    }
    fn remove(&mut self, depth: usize) -> bool {
        let before = self.0.len();
        self.0.retain(|e| e.0 != depth);
        self.0.len() != before
    }
    fn sorted(&self) -> Vec<(usize, Placement)> {
        let mut list = self.0.clone();
        list.sort_by_key(|e| e.0);
        list
    }
}

struct Sprite {
    frames: Vec<Depths>,
    hold: Option<usize>,
    evidence: Vec<(usize, Vec<String>)>,
    random_start: u8,
}

#[derive(Default)]
struct Dictionary {
    shapes: HashMap<usize, Shape>,
    morphs: HashMap<usize, morph::Morph>,
    sprites: HashMap<usize, Sprite>,
    fonts: HashMap<usize, Font>,
    texts: HashMap<usize, Text>,
    fields: HashMap<usize, Field>,
    bitmaps: HashMap<usize, std::rc::Rc<Bitmap>>,
    videos: HashMap<usize, Video>,
}

struct Video {
    width: usize,
    height: usize,
    count: usize,
    decoder: super::vp6::Decoder,
    frames: Vec<std::rc::Rc<Bitmap>>,
}
impl Video {
    fn new(b: &[u8]) -> R<(usize, Self)> {
        if b.len() != 10 {
            return Err("invalid SWF video definition length".into());
        }
        let id = u16le(b, 0)?;
        let count = u16le(b, 2)?;
        let width = u16le(b, 4)?;
        let height = u16le(b, 6)?;
        if b[8] & 0xf0 != 0 || b[8] >> 1 > 5 || count == 0 || count > 4096 {
            return Err("invalid SWF video descriptor".into());
        }
        if b[9] != 4 {
            return Err(format!("unsupported SWF video codec {}", b[9]));
        }
        if width
            .checked_mul(height)
            .and_then(|n| n.checked_mul(count * 4))
            .is_none_or(|n| n > 256 * 1024 * 1024)
        {
            return Err("SWF video frames exceed import budget".into());
        }
        Ok((
            id,
            Self {
                width,
                height,
                count,
                decoder: super::vp6::Decoder::new(width, height)?,
                frames: Vec::new(),
            },
        ))
    }
    fn packet(&mut self, b: &[u8]) -> R<()> {
        let index = u16le(b, 2)?;
        if index != self.frames.len() || index >= self.count {
            return Err("SWF video packets are missing, repeated or unordered".into());
        }
        let frame = self
            .decoder
            .decode(b.get(4..).ok_or("truncated SWF video packet")?)?;
        self.frames.push(std::rc::Rc::new(Bitmap {
            width: self.width,
            height: self.height,
            rgba: frame.rgba(),
        }));
        Ok(())
    }
    fn shape(&self, frame: usize) -> R<Shape> {
        let bitmap = self
            .frames
            .get(frame)
            .ok_or("SWF video placement references absent frame")?
            .clone();
        let w = self.width as f64 * 20.0;
        let h = self.height as f64 * 20.0;
        let points = [(0.0, 0.0), (w, 0.0), (w, h), (0.0, h), (0.0, 0.0)];
        let edges = points
            .windows(2)
            .map(|p| Edge {
                x0: p[0].0,
                y0: p[0].1,
                x1: p[1].0,
                y1: p[1].1,
                f0: 0,
                f1: 1,
                ln: 0,
            })
            .collect();
        Ok(Shape {
            fills: vec![Fill::Bitmap(BitmapFill {
                bitmap: Some(bitmap),
                matrix: Matrix {
                    a: 20.0,
                    b: 0.0,
                    c: 0.0,
                    d: 20.0,
                    tx: 0.0,
                    ty: 0.0,
                },
                clipped: true,
            })],
            lines: Vec::new(),
            edges,
        })
    }
}

impl Dictionary {
    fn has(&self, id: usize) -> bool {
        self.videos.contains_key(&id)
            || self.bitmaps.contains_key(&id)
            || self.shapes.contains_key(&id)
            || self.morphs.contains_key(&id)
            || self.sprites.contains_key(&id)
            || self.fonts.contains_key(&id)
            || self.texts.contains_key(&id)
            || self.fields.contains_key(&id)
    }
}

fn parse_sprite(
    payload: &[u8],
    dictionary: &Dictionary,
    effects: &mut Effects,
) -> R<(usize, Sprite)> {
    let id = u16le(payload, 0)?;
    let declared = u16le(payload, 2)?;
    let (tags, _) = tag_stream(payload, 4)?;
    let mut depths = Depths::default();
    let mut frames = Vec::new();
    let mut hold = None;
    let mut playing = None;
    let mut evidence = Vec::new();
    let mut random_start = 0;
    for Tag { tag, payload: body } in tags {
        if tag == 4 || tag == 26 || tag == 70 {
            read_placement(tag, body, &mut depths, effects)?;
        } else if tag == 5 || tag == 28 {
            let depth = u16le(body, if tag == 5 { 2 } else { 0 })?;
            if !depths.remove(depth) {
                return Err("SWF removal references empty depth".into());
            }
        } else if tag == 1 {
            for (_, entry) in &mut depths.0 {
                if entry.placed.is_none() {
                    entry.placed = Some(frames.len());
                }
            }
            if playing == Some(false) && hold.is_none() {
                hold = Some(frames.len());
            }
            playing = None;
            frames.push(depths.clone());
            if frames.len() > 4096 {
                return Err("SWF sprite frame bound".into());
            }
        } else if tag == 12 {
            if let Some(variants) = helpers::random_initial_phase(body) {
                if !frames.is_empty() || random_start != 0 || variants as usize != declared {
                    return Err("SWF random initial phase requires one first-frame initializer over the full child timeline".into());
                }
                random_start = variants;
                continue;
            }
            let (ops, _, _, _) = parse_actions(body)?;
            // Ordered, unconditional timeline controls. A later action on
            // the same frame can resume it; actions on unreachable frames
            // cannot advance a stopped playhead.
            if ops
                .iter()
                .all(|op| matches!(op.as_str(), "0x88" | "0x6" | "0x7"))
            {
                for op in &ops {
                    match op.as_str() {
                        "0x6" => playing = Some(true),
                        "0x7" => playing = Some(false),
                        _ => {}
                    }
                }
            } else {
                evidence.push((frames.len() + 1, ops));
            }
        } else if SKIPPED.contains(&tag) {
        } else if tag != 0 {
            return Err(format!("unsupported SWF sprite tag {tag}"));
        }
        if dictionary.sprites.contains_key(&id) || shape_version(tag).is_some() || tag == 39 {
            return Err("nested SWF sprite definitions need explicit conversion".into());
        }
    }
    if frames.len() != declared || frames.is_empty() {
        return Err("SWF sprite frame count disagrees".into());
    }
    if random_start != 0
        && (hold.is_some()
            || !evidence.is_empty()
            || frames.iter().any(|frame| {
                frame
                    .0
                    .iter()
                    .any(|(_, p)| dictionary.sprites.contains_key(&p.id))
            }))
    {
        return Err("SWF random initial phase with additional child timelines or actions requires explicit conversion".into());
    }
    Ok((
        id,
        Sprite {
            frames,
            hold,
            evidence,
            random_start,
        },
    ))
}

fn parse_bitmap(tag: usize, payload: &[u8], tables: Option<&[u8]>) -> R<Bitmap> {
    if tag == 20 || tag == 36 {
        let format = byte(payload, 2)?;
        let (width, height) = (u16le(payload, 3)?, u16le(payload, 5)?);
        let (colors, start) = if format == 3 {
            (byte(payload, 7)? as usize + 1, 8)
        } else {
            (0, 7)
        };
        let alpha = tag == 36;
        let entry = if alpha { 4 } else { 3 };
        let (pixel, stride) = match format {
            3 => (1, (width + 3) & !3),
            4 => (2, (width * 2 + 3) & !3),
            5 => (4, width * 4),
            _ => return Err(format!("unsupported SWF lossless format {format}")),
        };
        let data = miniz_oxide::inflate::decompress_to_vec_zlib_with_limit(
            &payload[start..],
            colors * entry + stride * height,
        )
        .map_err(|_| "corrupt SWF lossless bitmap")?;
        if data.len() < colors * entry + stride * height {
            return Err("truncated SWF lossless bitmap".into());
        }
        let (table, pixels) = data.split_at(colors * entry);
        // Lossless2 colors are premultiplied by their alpha.
        let straight = |r: u8, g: u8, b: u8, a: u8| -> [u8; 4] {
            if !alpha || a == 255 {
                [r, g, b, a]
            } else if a == 0 {
                [0, 0, 0, 0]
            } else {
                let un = |v: u8| ((v as u32 * 255 + a as u32 / 2) / a as u32).min(255) as u8;
                [un(r), un(g), un(b), a]
            }
        };
        let mut rgba = Vec::with_capacity(width * height);
        for y in 0..height {
            for x in 0..width {
                let at = y * stride + x * pixel;
                rgba.push(match format {
                    3 => {
                        let i = pixels[at] as usize;
                        if i >= colors {
                            [0, 0, 0, 0]
                        } else {
                            let c = &table[i * entry..];
                            straight(c[0], c[1], c[2], if alpha { c[3] } else { 255 })
                        }
                    }
                    4 => {
                        let v = u16::from_be_bytes([pixels[at], pixels[at + 1]]);
                        let five = |s: u16| (((v >> s) & 31) * 255 / 31) as u8;
                        [five(10), five(5), five(0), 255]
                    }
                    _ => straight(
                        pixels[at + 1],
                        pixels[at + 2],
                        pixels[at + 3],
                        if alpha { pixels[at] } else { 255 },
                    ),
                });
            }
        }
        return Ok(Bitmap {
            width,
            height,
            rgba,
        });
    }
    let (jpeg, alpha) = if tag == 35 {
        let offset = u32le(payload, 2)?;
        let jpeg = payload.get(6..6 + offset).ok_or("truncated SWF JPEG3")?;
        (jpeg, Some(&payload[6 + offset..]))
    } else {
        (&payload[2..], None)
    };
    let parts: Vec<&[u8]> = match (tag, tables) {
        (6, Some(tables)) => vec![tables, jpeg],
        (6, None) => return Err("SWF DefineBits without JPEGTables".into()),
        _ => vec![jpeg],
    };
    let (width, height, rgb) =
        super::bitmap::decode_jpeg(&parts).map_err(|e| format!("SWF JPEG: {e}"))?;
    let plane = match alpha {
        Some(z) if !z.is_empty() => {
            let plane = miniz_oxide::inflate::decompress_to_vec_zlib_with_limit(z, width * height)
                .map_err(|_| "corrupt SWF JPEG3 alpha")?;
            if plane.len() != width * height {
                return Err("SWF JPEG3 alpha size differs".into());
            }
            Some(plane)
        }
        _ => None,
    };
    let rgba = rgb
        .iter()
        .enumerate()
        .map(|(i, c)| [c[0], c[1], c[2], plane.as_ref().map_or(255, |a| a[i])])
        .collect();
    Ok(Bitmap {
        width,
        height,
        rgba,
    })
}

/// Nearest-neighbour sample of a bitmap fill; clipped fills repeat their
/// edge pixels, the others tile.
fn bitmap_color(f: &BitmapFill, sx: f64, sy: f64) -> R<[f64; 4]> {
    let Some(bitmap) = f.bitmap.as_ref().filter(|b| b.width > 0 && b.height > 0) else {
        return Ok([0.0; 4]);
    };
    let m = &f.matrix;
    let det = m.a * m.d - m.b * m.c;
    if det == 0.0 {
        return Err("degenerate SWF bitmap matrix".into());
    }
    let (px, py) = (sx - m.tx, sy - m.ty);
    let u = ((m.d * px - m.c * py) / det).floor();
    let v = ((m.a * py - m.b * px) / det).floor();
    let (w, h) = (bitmap.width as f64, bitmap.height as f64);
    let (u, v) = if f.clipped {
        (u.max(0.0).min(w - 1.0), v.max(0.0).min(h - 1.0))
    } else {
        (u.rem_euclid(w), v.rem_euclid(h))
    };
    let c = bitmap.rgba[v as usize * bitmap.width + u as usize];
    Ok([c[0] as f64, c[1] as f64, c[2] as f64, c[3] as f64])
}

fn gradient_color(g: &Gradient, sx: f64, sy: f64) -> R<[f64; 4]> {
    let m = &g.matrix;
    let det = m.a * m.d - m.b * m.c;
    if det == 0.0 {
        return Err("degenerate SWF gradient matrix".into());
    }
    let (px, py) = (sx - m.tx, sy - m.ty);
    let gx = (m.d * px - m.c * py) / det;
    let gy = (m.a * py - m.b * px) / det;
    let mut t = if g.radial {
        (gx * gx + gy * gy).sqrt() / 16384.0
    } else {
        (gx + 16384.0) / 32768.0
    };
    t = t.max(0.0).min(1.0) * 255.0;
    let stops = &g.stops;
    if t <= stops[0].0 {
        return Ok(stops[0].1);
    }
    for i in 1..stops.len() {
        if t <= stops[i].0 {
            let span = if stops[i].0 - stops[i - 1].0 != 0.0 {
                stops[i].0 - stops[i - 1].0
            } else {
                1.0
            };
            let mix = (t - stops[i - 1].0) / span;
            let a = stops[i - 1].1;
            let b = stops[i].1;
            return Ok([0, 1, 2, 3].map(|c| js::round(a[c] + (b[c] - a[c]) * mix)));
        }
    }
    Ok(stops[stops.len() - 1].1)
}

fn blend(rgba: &mut [u8], i: usize, color: [f64; 4]) {
    let alpha = color[3];
    if alpha == 0.0 || alpha.is_nan() {
        return;
    }
    let dst_alpha = rgba[i + 3] as f64;
    let out = alpha + dst_alpha * (255.0 - alpha) / 255.0;
    for c in 0..3 {
        rgba[i + c] = js::round(
            (color[c] * alpha + rgba[i + c] as f64 * dst_alpha * (255.0 - alpha) / 255.0) / out,
        ) as u8;
    }
    rgba[i + 3] = js::round(out) as u8;
}

fn transform_color(cxform: &Option<Cxform>, color: [f64; 4]) -> [f64; 4] {
    match cxform {
        None => color,
        Some(x) => [0, 1, 2, 3].map(|c| {
            jmin(
                255.0,
                jmax(0.0, js::round(color[c] * x.mult[c] / 256.0 + x.add[c])),
            )
        }),
    }
}

fn compose(outer: &Option<Cxform>, inner: &Option<Cxform>) -> Option<Cxform> {
    match (outer, inner) {
        (None, i) => i.clone(),
        (o, None) => o.clone(),
        (Some(o), Some(i)) => Some(Cxform {
            mult: [0, 1, 2, 3].map(|c| o.mult[c] * i.mult[c] / 256.0),
            add: [0, 1, 2, 3].map(|c| o.add[c] + o.mult[c] * i.add[c] / 256.0),
        }),
    }
}

#[derive(Clone, Copy)]
struct Canvas {
    width: usize,
    height: usize,
    origin_x: f64,
    origin_y: f64,
}

#[allow(clippy::too_many_arguments)]
fn paint_shape(
    rgba: &mut [u8],
    canvas: &Canvas,
    shape: &Shape,
    m: &Matrix,
    cxform: &Option<Cxform>,
    stencils: Option<&[Vec<u8>]>,
) -> R<()> {
    let det = m.a * m.d - m.b * m.c;
    if det == 0.0 {
        return Err("degenerate SWF placement matrix".into());
    }
    let (mut min_x, mut min_y, mut max_x, mut max_y) = (
        f64::INFINITY,
        f64::INFINITY,
        f64::NEG_INFINITY,
        f64::NEG_INFINITY,
    );
    let margin = shape
        .lines
        .iter()
        .fold(20.0f64, |most, l| most.max(l.width));
    for e in &shape.edges {
        for (ex, ey) in [(e.x0, e.y0), (e.x1, e.y1)] {
            let tx = m.a * ex + m.c * ey + m.tx;
            let ty = m.b * ex + m.d * ey + m.ty;
            min_x = min_x.min(tx);
            max_x = max_x.max(tx);
            min_y = min_y.min(ty);
            max_y = max_y.max(ty);
        }
    }
    let (w, h) = (canvas.width as f64, canvas.height as f64);
    let x0 = ((min_x - margin - canvas.origin_x) / 20.0).floor().max(0.0);
    let x1 = ((max_x + margin - canvas.origin_x) / 20.0)
        .ceil()
        .min(w - 1.0);
    let y0 = ((min_y - margin - canvas.origin_y) / 20.0).floor().max(0.0);
    let y1 = ((max_y + margin - canvas.origin_y) / 20.0)
        .ceil()
        .min(h - 1.0);
    // Axis-aligned placements keep sy constant along a row: each row's edge
    // crossings are tabulated once, with the per-pixel test's arithmetic.
    let aligned = m.b == 0.0 && m.c == 0.0;
    let mut y = y0;
    while y <= y1 {
        let row = aligned.then(|| {
            let px = canvas.origin_x + (x0 + 0.5) * 20.0 - m.tx;
            let py = canvas.origin_y + (y + 0.5) * 20.0 - m.ty;
            RowTable::new(shape, (m.a * py - m.b * px) / det)
        });
        let mut x = x0;
        while x <= x1 {
            let px = canvas.origin_x + (x + 0.5) * 20.0 - m.tx;
            let py = canvas.origin_y + (y + 0.5) * 20.0 - m.ty;
            let sx = (m.d * px - m.c * py) / det;
            let sy = (m.a * py - m.b * px) / det;
            let mut paint: Option<[f64; 4]> = None;
            for s in 1..=shape.fills.len() {
                let winding = match &row {
                    Some(table) => table.winding(s, sx),
                    None => edge_winding(shape, s, sx, sy),
                };
                if winding != 0 {
                    paint = Some(match &shape.fills[s - 1] {
                        Fill::Gradient(g) => gradient_color(g, sx, sy)?,
                        Fill::Bitmap(f) => bitmap_color(f, sx, sy)?,
                        Fill::Color(c) => *c,
                    });
                }
            }
            for s in 1..=shape.lines.len() {
                let radius = shape.lines[s - 1].width.max(20.0) / 2.0;
                let limit = radius * radius;
                let near = |e: &Edge| {
                    let (dx, dy) = (e.x1 - e.x0, e.y1 - e.y0);
                    let length_sq = dx * dx + dy * dy;
                    let t = if length_sq != 0.0 {
                        (((sx - e.x0) * dx + (sy - e.y0) * dy) / length_sq)
                            .max(0.0)
                            .min(1.0)
                    } else {
                        0.0
                    };
                    let qx = e.x0 + t * dx - sx;
                    let qy = e.y0 + t * dy - sy;
                    qx * qx + qy * qy <= limit
                };
                let hit = match &row {
                    Some(table) => table.lines[s - 1].iter().any(|&i| near(&shape.edges[i])),
                    None => shape.edges.iter().any(|e| e.ln == s && near(e)),
                };
                if hit {
                    paint = Some(shape.lines[s - 1].color);
                }
            }
            if let Some(color) = paint {
                let index = y as usize * canvas.width + x as usize;
                if !stencils.is_some_and(|all| all.iter().any(|st| st[index] == 0)) {
                    blend(rgba, index * 4, transform_color(cxform, color));
                }
            }
            x += 1.0;
        }
        y += 1.0;
    }
    Ok(())
}

fn crossing(e: &Edge, s: usize, sy: f64) -> Option<(f64, i64)> {
    let down = e.f1 == s;
    let (ax, ay, bx, by) = if down {
        (e.x0, e.y0, e.x1, e.y1)
    } else {
        (e.x1, e.y1, e.x0, e.y0)
    };
    ((ay > sy) != (by > sy)).then(|| {
        (
            ax + (sy - ay) * (bx - ax) / (by - ay),
            if by > ay { 1 } else { -1 },
        )
    })
}

fn edge_winding(shape: &Shape, s: usize, sx: f64, sy: f64) -> i64 {
    shape
        .edges
        .iter()
        .filter(|e| e.f1 == s || e.f0 == s)
        .filter_map(|e| crossing(e, s, sy))
        .filter(|&(at, _)| sx < at)
        .map(|(_, sign)| sign)
        .sum()
}

/// One scanline's crossings per fill (sorted, with suffix winding sums) and
/// the stroke edges whose padded y-range reaches it.
struct RowTable {
    fills: Vec<(Vec<f64>, Vec<i64>)>,
    lines: Vec<Vec<usize>>,
}

impl RowTable {
    fn new(shape: &Shape, sy: f64) -> Self {
        // One pass over the edges: an edge counts for each fill it bounds,
        // once only when both sides name the same fill.
        let mut hits: Vec<Vec<(f64, i64)>> = vec![Vec::new(); shape.fills.len() + 1];
        let mut lines: Vec<Vec<usize>> = vec![Vec::new(); shape.lines.len() + 1];
        for (i, e) in shape.edges.iter().enumerate() {
            let (lo, hi) = (e.y0.min(e.y1), e.y0.max(e.y1));
            if e.ln != 0 {
                let radius = shape.lines[e.ln - 1].width.max(20.0) / 2.0;
                let pad = radius + 1e-6 * (lo.abs() + hi.abs() + radius) + 1e-6;
                if sy >= lo - pad && sy <= hi + pad {
                    lines[e.ln].push(i);
                }
            }
            if sy < lo || sy > hi {
                continue;
            }
            for s in [e.f1, e.f0] {
                if s != 0 {
                    if let Some(hit) = crossing(e, s, sy) {
                        hits[s].push(hit);
                    }
                }
                if e.f0 == e.f1 {
                    break;
                }
            }
        }
        let fills = hits
            .into_iter()
            .skip(1)
            .map(|mut hits| {
                hits.sort_by(|a, b| a.0.total_cmp(&b.0));
                let mut suffix = vec![0i64; hits.len() + 1];
                for i in (0..hits.len()).rev() {
                    suffix[i] = suffix[i + 1] + hits[i].1;
                }
                (hits.into_iter().map(|h| h.0).collect(), suffix)
            })
            .collect();
        RowTable {
            fills,
            lines: lines.into_iter().skip(1).collect(),
        }
    }

    fn winding(&self, s: usize, sx: f64) -> i64 {
        let (at, suffix) = &self.fills[s - 1];
        suffix[at.partition_point(|&c| c <= sx)]
    }
}

fn glyph_for(font: &Font, code: usize) -> Option<usize> {
    font.codes.as_ref()?.iter().position(|&c| c == code)
}

fn layout_field(field: &Field, font: &Font, text: &str) -> R<Vec<(Option<usize>, f64, f64)>> {
    let scale = field.font_height / font.em;
    let ascent = font.ascent.unwrap_or(font.em * 0.8) * scale;
    let leading = field.font_height + field.leading;
    let inner_left = field.bounds[0] + 40.0 + field.left_margin + field.indent;
    let inner_right = field.bounds[1] - 40.0 - field.right_margin;
    let advance = |index: usize| font.advances.as_ref().map(|a| a[index]).unwrap_or(font.em);
    let advance_of = |index: Option<usize>| match &font.advances {
        None => font.em,
        Some(a) => index.map(|i| a[i]).unwrap_or(f64::NAN),
    };
    let width_of = |value: &str| -> R<f64> {
        let mut total = 0.0;
        for ch in value.chars() {
            let index = glyph_for(font, ch as usize)
                .ok_or_else(|| format!("SWF field glyph {} unavailable", ch as u32))?;
            total += advance(index) * scale;
        }
        Ok(total)
    };
    let mut lines: Vec<String> = Vec::new();
    for paragraph in text.split('\r') {
        // split(/(?<= )/): break after every space.
        let words: Vec<String> = if field.word_wrap {
            let mut out = Vec::new();
            let mut current = String::new();
            for ch in paragraph.chars() {
                current.push(ch);
                if ch == ' ' {
                    out.push(std::mem::take(&mut current));
                }
            }
            if !current.is_empty() || out.is_empty() {
                out.push(current);
            }
            out
        } else {
            vec![paragraph.to_string()]
        };
        let mut current = String::new();
        for word in words {
            let candidate = format!(
                "{current}{}",
                word.trim_end_matches(super::source::js_space)
            );
            if !current.is_empty()
                && field.word_wrap
                && width_of(&candidate)? > inner_right - inner_left
            {
                lines.push(std::mem::replace(&mut current, word));
            } else {
                current.push_str(&word);
            }
        }
        lines.push(current);
    }
    let mut placed = Vec::new();
    let mut baseline = field.bounds[2] + 40.0 + ascent;
    for line in lines {
        let trimmed = line.trim_end_matches(' ');
        let mut line_width = 0.0;
        for ch in trimmed.chars() {
            line_width += advance_of(glyph_for(font, ch as usize)) * scale;
        }
        let mut pen = if field.align == 2.0 {
            inner_left + (inner_right - inner_left - line_width) / 2.0
        } else if field.align == 1.0 {
            inner_right - line_width
        } else {
            inner_left
        };
        for ch in trimmed.chars() {
            let index = glyph_for(font, ch as usize);
            placed.push((index, pen, baseline));
            pen += advance_of(index) * scale;
        }
        baseline += leading;
    }
    Ok(placed)
}

pub struct Timeline {
    pub width: usize,
    pub height: usize,
    pub rate: f64,
    pub frames: Vec<Vec<u8>>,
    pub variant_count: u8,
    pub audio: Option<audio::Audio>,
    pub labels: Vec<Value>,
    pub stop_frames: Vec<Value>,
    pub effects: Effects,
    pub ignored_actions: Vec<Value>,
    pub fields: Vec<Value>,
    pub named_children: Vec<Value>,
    pub helpers: Vec<Value>,
    pub host_fullscreen: bool,
    pub layers: Vec<Value>,
    counts: (usize, usize, usize),
}

struct Renderer<'a> {
    geometric_mask: bool,
    dictionary: &'a Dictionary,
    canvas: Canvas,
    variant: usize,
}

impl Renderer<'_> {
    fn render_mask_entry(
        &self,
        rgba: &mut [u8],
        entry: &mut Placement,
        frame: usize,
        nested: bool,
    ) -> R<()> {
        let renderer = Renderer {
            dictionary: self.dictionary,
            canvas: self.canvas,
            variant: self.variant,
            geometric_mask: true,
        };
        renderer.render_entry(rgba, entry, frame, &None, &[], nested)
    }
    fn paint(
        &self,
        rgba: &mut [u8],
        shape: &Shape,
        matrix: &Matrix,
        color: &Option<Cxform>,
        stencils: Option<&[Vec<u8>]>,
    ) -> R<()> {
        if self.geometric_mask {
            let shape = Shape {
                fills: vec![Fill::Color([255.0; 4]); shape.fills.len()],
                lines: Vec::new(),
                edges: shape
                    .edges
                    .iter()
                    .map(|edge| Edge { ln: 0, ..*edge })
                    .collect(),
            };
            paint_shape(rgba, &self.canvas, &shape, matrix, &None, stencils)
        } else {
            paint_shape(rgba, &self.canvas, shape, matrix, color, stencils)
        }
    }
    fn filtered_live_text(
        &self,
        entry: &Placement,
        nested: bool,
        visiting: &mut Vec<usize>,
    ) -> bool {
        if self.dictionary.fields.get(&entry.id).is_some_and(|field| {
            !field.variable.is_empty()
                || (entry.name.as_ref().is_some_and(|name| !name.is_empty()) && !nested)
        }) {
            return true;
        }
        if visiting.contains(&entry.id) {
            return false;
        }
        visiting.push(entry.id);
        let result = self
            .dictionary
            .sprites
            .get(&entry.id)
            .is_some_and(|sprite| {
                sprite.frames.iter().any(|frame| {
                    frame
                        .sorted()
                        .iter()
                        .any(|(_, child)| self.filtered_live_text(child, true, visiting))
                })
            });
        visiting.pop();
        result
    }
    fn render_list(
        &self,
        rgba: &mut [u8],
        list: &mut [(usize, Placement)],
        frame: usize,
        cxform: &Option<Cxform>,
        stencils: &[Vec<u8>],
        nested: bool,
    ) -> R<()> {
        let mut clips: Vec<(usize, Vec<u8>)> = Vec::new();
        for (depth, entry) in list.iter_mut() {
            clips.retain(|(until, _)| *until >= *depth);
            if let Some(until) = entry.clip {
                let mut scratch = vec![0u8; self.canvas.width * self.canvas.height * 4];
                let mut copy = entry.clone();
                copy.clip = None;
                self.render_mask_entry(&mut scratch, &mut copy, frame, nested)?;
                let stencil: Vec<u8> = scratch.chunks(4).map(|p| u8::from(p[3] > 0)).collect();
                clips.push((until, stencil));
                continue;
            }
            let mut active: Vec<Vec<u8>> = stencils.to_vec();
            active.extend(clips.iter().map(|c| c.1.clone()));
            self.render_entry(rgba, entry, frame, cxform, &active, nested)?;
        }
        Ok(())
    }

    fn render_entry(
        &self,
        rgba: &mut [u8],
        entry: &mut Placement,
        frame: usize,
        cxform: &Option<Cxform>,
        stencils: &[Vec<u8>],
        nested: bool,
    ) -> R<()> {
        let d = self.dictionary;
        if !self.geometric_mask && entry.filters.iter().any(|filter| !filter.identity()) {
            if self.filtered_live_text(entry, nested, &mut Vec::new()) {
                return Err("SWF filtered live text needs runtime filtering".into());
            }
            let (pad_x, pad_y) = entry
                .filters
                .iter()
                .map(filters::GradientBevel::padding)
                .fold((0usize, 0usize), |(x, y), (dx, dy)| (x + dx, y + dy));
            let width = self.canvas.width + pad_x * 2;
            let height = self.canvas.height + pad_y * 2;
            if width
                .checked_mul(height)
                .is_none_or(|n| n > 16 * 1024 * 1024)
            {
                return Err("SWF filter scratch surface exceeds bound".into());
            }
            let renderer = Renderer {
                geometric_mask: self.geometric_mask,
                dictionary: d,
                canvas: Canvas {
                    width,
                    height,
                    origin_x: self.canvas.origin_x - pad_x as f64 * 20.0,
                    origin_y: self.canvas.origin_y - pad_y as f64 * 20.0,
                },
                variant: self.variant,
            };
            let mut isolated = vec![0; width * height * 4];
            let mut plain = entry.clone();
            plain.filters.clear();
            renderer.render_entry(&mut isolated, &mut plain, frame, cxform, &[], nested)?;
            entry.placed = plain.placed;
            for filter in &entry.filters {
                filters::apply(filter, width, height, &mut isolated)?;
            }
            for y in 0..self.canvas.height {
                for x in 0..self.canvas.width {
                    let index = y * self.canvas.width + x;
                    if stencils.iter().any(|stencil| stencil[index] == 0) {
                        continue;
                    }
                    let at = ((y + pad_y) * width + x + pad_x) * 4;
                    let pixel = &isolated[at..at + 4];
                    blend(
                        rgba,
                        index * 4,
                        [
                            pixel[0] as f64,
                            pixel[1] as f64,
                            pixel[2] as f64,
                            pixel[3] as f64,
                        ],
                    );
                }
            }
            return Ok(());
        }
        let combined = if self.geometric_mask {
            None
        } else {
            compose(cxform, &entry.cxform)
        };
        let stencils = if stencils.is_empty() {
            None
        } else {
            Some(stencils)
        };
        if let Some(shape) = d.shapes.get(&entry.id) {
            return self.paint(rgba, shape, &entry.matrix, &combined, stencils);
        }
        if let Some(video) = d.videos.get(&entry.id) {
            return self.paint(
                rgba,
                &video.shape(entry.ratio as usize)?,
                &entry.matrix,
                &combined,
                stencils,
            );
        }
        if let Some(morph) = d.morphs.get(&entry.id) {
            return self.paint(
                rgba,
                &morph.at(entry.ratio)?,
                &entry.matrix,
                &combined,
                stencils,
            );
        }
        if let Some(text) = d.texts.get(&entry.id) {
            for record in &text.records {
                let font = record
                    .font
                    .and_then(|f| d.fonts.get(&f))
                    .ok_or("SWF text references absent font")?;
                let scale = record.height / font.em;
                for &(index, x, y) in &record.glyphs {
                    let glyph = font
                        .glyphs
                        .get(index)
                        .ok_or("SWF text references absent glyph")?;
                    let m = multiply(
                        &entry.matrix,
                        &multiply(
                            &text.matrix,
                            &Matrix {
                                a: scale,
                                b: 0.0,
                                c: 0.0,
                                d: scale,
                                tx: x,
                                ty: y,
                            },
                        ),
                    );
                    let shape = Shape {
                        fills: vec![Fill::Color(record.color)],
                        lines: Vec::new(),
                        edges: glyph.edges.clone(),
                    };
                    self.paint(rgba, &shape, &m, &combined, stencils)?;
                }
            }
            return Ok(());
        }
        if let Some(field) = d.fields.get(&entry.id) {
            if self.geometric_mask && !field.embed_fonts {
                return Ok(());
            }
            if !field.variable.is_empty()
                || (entry.name.as_ref().is_some_and(|n| !n.is_empty()) && !nested)
                || field.text.is_empty()
            {
                return Ok(());
            }
            let (text, attributes) = if field.html {
                parse_field_html(&field.text)?
            } else {
                (field.text.clone(), HashMap::new())
            };
            let font = field
                .font
                .and_then(|f| d.fonts.get(&f))
                .ok_or("SWF field references absent font")?;
            let align = match attributes.get("align").map(String::as_str) {
                Some("left") => 0.0,
                Some("right") => 1.0,
                Some("center") => 2.0,
                _ => field.align,
            };
            let color = match attributes.get("color").filter(|v| !v.is_empty()) {
                Some(value) => {
                    let mut parts: Vec<f64> = Vec::new();
                    let bytes = value.as_bytes();
                    let mut i = 0;
                    while i + 1 < bytes.len() {
                        if bytes[i].is_ascii_hexdigit() && bytes[i + 1].is_ascii_hexdigit() {
                            parts.push(u8::from_str_radix(&value[i..i + 2], 16).unwrap() as f64);
                            i += 2;
                        } else {
                            i += 1;
                        }
                    }
                    parts.push(255.0);
                    parts.truncate(4);
                    let mut c = [0.0; 4];
                    for (k, v) in parts.iter().enumerate() {
                        c[k] = *v;
                    }
                    if parts.len() < 4 {
                        c[parts.len()..].fill(f64::NAN);
                    }
                    c
                }
                None => field.color,
            };
            let size = match attributes.get("size").filter(|v| !v.is_empty()) {
                Some(s) => parse_int(s) * 20.0,
                None => field.font_height,
            };
            let mut laid = field.clone();
            laid.align = align;
            laid.font_height = size;
            for (index, x, y) in layout_field(&laid, font, &text)? {
                let scale = size / font.em;
                let glyph = index
                    .and_then(|i| font.glyphs.get(i))
                    .ok_or("SWF field glyph unavailable")?;
                let m = multiply(
                    &entry.matrix,
                    &Matrix {
                        a: scale,
                        b: 0.0,
                        c: 0.0,
                        d: scale,
                        tx: x,
                        ty: y,
                    },
                );
                let shape = Shape {
                    fills: vec![Fill::Color(color)],
                    lines: Vec::new(),
                    edges: glyph.edges.clone(),
                };
                self.paint(rgba, &shape, &m, &combined, stencils)?;
            }
            return Ok(());
        }
        if let Some(sprite) = d.sprites.get(&entry.id) {
            if entry.placed.is_none() {
                entry.placed = Some(frame);
            }
            if sprite.random_start != 0 && nested {
                return Err(
                    "nested SWF random initial phase requires an independent instance clock".into(),
                );
            }
            let elapsed = frame - entry.placed.unwrap()
                + if sprite.random_start != 0 {
                    self.variant
                } else {
                    0
                };
            let (local, cycle) = match sprite.hold {
                Some(hold) if elapsed >= hold => (hold, 0),
                _ => (
                    elapsed % sprite.frames.len(),
                    elapsed / sprite.frames.len() * sprite.frames.len(),
                ),
            };
            let mut list: Vec<(usize, Placement)> = sprite.frames[local]
                .sorted()
                .into_iter()
                .map(|(depth, inner)| {
                    let mut child = inner.clone();
                    child.matrix = multiply(&entry.matrix, &inner.matrix);
                    child.placed = Some(cycle + inner.placed.unwrap_or(0));
                    (depth, child)
                })
                .collect();
            // Stopping this playhead does not stop the timelines of its
            // children. Their clocks retain their own placement origins.
            return self.render_list(
                rgba,
                &mut list,
                elapsed,
                &combined,
                stencils.unwrap_or(&[]),
                true,
            );
        }
        Err("SWF placement references absent character".into())
    }
}

type FrameSink<'a> = dyn FnMut(usize, usize, &[u8]) -> R<()> + 'a;

/// One root depth's cropped pixels. Translation is independent of the crop,
/// so moving an object exposes what was behind its original placement.
pub struct RasterLayer {
    pub width: usize,
    pub height: usize,
    pub rgba: Vec<u8>,
}
type LayerSink<'a> = dyn FnMut(&RasterLayer) -> R<String> + 'a;

fn timeline(
    b: &[u8],
    mut sink: Option<&mut FrameSink<'_>>,
    mut layer_sink: Option<&mut LayerSink<'_>>,
) -> R<Timeline> {
    if b.len() < 8 || &b[..3] != b"FWS" || u32le(b, 4)? != b.len() {
        return Err("unsupported static SWF envelope".into());
    }
    let utf8 = b[3] >= 6;
    let mut bits = Bits::new(b, 8);
    let bounds = bits.rect()?;
    let p = bits.align();
    if p + 4 > b.len() {
        return Err("truncated SWF header".into());
    }
    let rate = u16le(b, p)? as f64 / 256.0;
    let declared_frames = u16le(b, p + 2)?;
    if declared_frames == 0 || declared_frames > 4096 {
        return Err("SWF frame count exceeds conversion bounds".into());
    }
    let width = (bounds[1] - bounds[0]) / 20.0;
    let height = (bounds[3] - bounds[2]) / 20.0;
    let empty_stage = width == 0.0 && height == 0.0;
    if width.fract() != 0.0
        || height.fract() != 0.0
        || (!empty_stage && (width < 1.0 || height < 1.0))
        || width > 2048.0
        || height > 2048.0
    {
        return Err("unsupported SWF stage bounds".into());
    }
    let (tags, end) = tag_stream(b, p + 4)?;
    if end != b.len()
        || !tags
            .last()
            .is_some_and(|t| t.tag == 0 && t.payload.is_empty())
    {
        return Err("unterminated SWF timeline".into());
    }
    // The raster representation of an authored empty vector is a single
    // transparent pixel. Validate its display list below before accepting it.
    let (width, height) = ((width as usize).max(1), (height as usize).max(1));
    // Keep every depth if any root placement is named, including placements
    // before its first appearance and unnamed foreground/background objects.
    let separate_layers = layer_sink.is_some()
        && tags
            .iter()
            .any(|t| matches!(t.tag, 26 | 70) && t.payload.first().is_some_and(|f| f & 32 != 0));
    let mut dictionary = Dictionary::default();
    let mut depths = Depths::default();
    let mut frames = Vec::new();
    let mut frame_count = 0;
    let mut variant_count = 1u8;
    let mut random_identity = None;
    let mut labels = Vec::new();
    let mut stop_frames = Vec::new();
    let mut evidence: Vec<Value> = Vec::new();
    let mut field_placements: Vec<(usize, usize, Matrix, Option<String>, usize)> = Vec::new();
    let mut named: Vec<Value> = Vec::new();
    let mut helpers = Vec::new();
    let mut host_fullscreen = false;
    let mut layers = Vec::new();
    let mut effects = Effects::default();
    let mut jpeg_tables: Option<&[u8]> = None;
    let mut audio_stream = None;
    let define = |dictionary: &Dictionary, id: usize| -> R<()> {
        if dictionary.has(id) {
            Err("duplicate SWF character".into())
        } else {
            Ok(())
        }
    };
    for Tag { tag, payload } in &tags {
        let (tag, payload) = (*tag, *payload);
        if let Some(version) = shape_version(tag) {
            let (id, shape) = parse_shape(payload, version, &dictionary.bitmaps)?;
            define(&dictionary, id)?;
            dictionary.shapes.insert(id, shape);
        } else if tag == 46 {
            let (id, morph) = morph::parse(payload, &dictionary.bitmaps)?;
            define(&dictionary, id)?;
            dictionary.morphs.insert(id, morph);
        } else if tag == 8 {
            jpeg_tables = Some(payload);
        } else if matches!(tag, 6 | 21 | 35 | 20 | 36) {
            let id = u16le(payload, 0)?;
            define(&dictionary, id)?;
            let bitmap = parse_bitmap(tag, payload, jpeg_tables)?;
            dictionary.bitmaps.insert(id, std::rc::Rc::new(bitmap));
        } else if tag == 39 {
            let (id, sprite) = parse_sprite(payload, &dictionary, &mut effects)?;
            define(&dictionary, id)?;
            dictionary.sprites.insert(id, sprite);
        } else if tag == 10 || tag == 48 || tag == 75 {
            let (id, font) = parse_font(payload, tag)?;
            define(&dictionary, id)?;
            dictionary.fonts.insert(id, font);
        } else if tag == 13 {
            let font_id = u16le(payload, 0)?;
            let font = dictionary
                .fonts
                .get_mut(&font_id)
                .filter(|f| f.codes.is_none())
                .ok_or("SWF font info without its font")?;
            let name_length = byte(payload, 2)? as usize;
            let wide = byte(payload, 3 + name_length)? & 1 != 0;
            let mut codes = Vec::new();
            let mut q = 4 + name_length;
            while q < payload.len() {
                codes.push(if wide {
                    u16le(payload, q)?
                } else {
                    payload[q] as usize
                });
                q += if wide { 2 } else { 1 };
            }
            font.codes = Some(codes);
        } else if tag == 11 || tag == 33 {
            let (id, text) = parse_define_text(payload, if tag == 33 { 2 } else { 1 })?;
            define(&dictionary, id)?;
            dictionary.texts.insert(id, text);
        } else if tag == 37 {
            let (id, field) = parse_edit_text(payload, utf8)?;
            define(&dictionary, id)?;
            dictionary.fields.insert(id, field);
        } else if tag == 60 {
            let (id, video) = Video::new(payload)?;
            define(&dictionary, id)?;
            dictionary.videos.insert(id, video);
        } else if tag == 61 {
            dictionary
                .videos
                .get_mut(&u16le(payload, 0)?)
                .ok_or("SWF video packet without definition")?
                .packet(payload)?;
        } else if tag == 18 || tag == 45 {
            if audio_stream.is_some() {
                return Err("multiple SWF sound stream headers require explicit conversion".into());
            }
            audio_stream = Some(audio::Stream::new(payload)?);
        } else if tag == 19 {
            audio_stream
                .as_mut()
                .ok_or("SWF sound block without header")?
                .block(frame_count, payload)?;
        } else if tag == 4 || tag == 26 || tag == 70 {
            read_placement(tag, payload, &mut depths, &mut effects)?;
        } else if tag == 5 || tag == 28 {
            let depth = u16le(payload, if tag == 5 { 2 } else { 0 })?;
            if !depths.remove(depth) {
                return Err("SWF removal references empty depth".into());
            }
        } else if tag == 12 || tag == 59 {
            let (ops, stop, functions, fullscreen) = parse_actions(payload)?;
            helpers.extend(functions);
            host_fullscreen |= fullscreen;
            if stop {
                stop_frames.push(json!(frame_count + 1));
            }
            if ops.iter().any(|op| {
                !matches!(
                    op.as_str(),
                    "0x88" | "0x7" | "ConvertedFunction" | "HostFullscreen"
                )
            }) {
                evidence.push(json!({"frame": frame_count + 1, "ops": ops}));
            }
        } else if tag == 43 {
            labels.push(json!({"frame": frame_count + 1, "name": js::latin1(payload).trim_end_matches('\0')}));
        } else if tag == 1 || (tag == 0 && b[3] == 1 && frame_count + 1 == declared_frames
            && tags.iter().rposition(|t| t.tag == 1).is_some_and(|i|
                i + 2 < tags.len() && tags[i + 1..tags.len() - 1].iter().all(|t| t.tag == 9))) {
            // Flash 1 publications can terminate their final declared frame
            // at End, after only background-colour tags. Keep that final
            // display-list pose; do not relax counts for truncated timelines.
            if !payload.is_empty() {
                return Err("unsupported SWF frame payload".into());
            }
            if frame_count >= declared_frames {
                return Err("SWF timeline frame count disagrees: more frames than declared".into());
            }
            let random: Vec<_> = dictionary
                .sprites
                .iter()
                .filter(|(_, s)| s.random_start != 0)
                .collect();
            if !random.is_empty() {
                if random.len() != 1 || !dictionary.fields.is_empty() {
                    return Err("SWF random initial phase requires one stable root child without live fields".into());
                }
                let (&id, sprite) = random[0];
                let placements: Vec<_> = depths.0.iter().filter(|(_, p)| p.id == id).collect();
                if placements.len() != 1 || placements[0].1.placed.unwrap_or(frame_count) != 0 {
                    return Err("SWF random initial phase child is removed, replaced or starts after the root".into());
                }
                let identity = (id, placements[0].0);
                if frame_count == 0 {
                    random_identity = Some(identity);
                    variant_count = sprite.random_start;
                }
                if random_identity != Some(identity) {
                    return Err(
                        "SWF random initial phase child identity changes across root frames".into(),
                    );
                }
            } else if random_identity.is_some() {
                return Err("SWF random initial phase child is absent".into());
            }
            if sink.is_none()
                && (frame_count + 1) * variant_count as usize * width * height * 4
                    > 256 * 1024 * 1024
            {
                return Err(
                    "SWF collected frames exceed memory bound; use streamed conversion".into(),
                );
            }
            for variant in 0..variant_count as usize {
                let mut rgba = vec![0u8; width * height * 4];
                let renderer = Renderer {
                    geometric_mask: false,
                    dictionary: &dictionary,
                    canvas: Canvas {
                        width,
                        height,
                        origin_x: bounds[0],
                        origin_y: bounds[2],
                    },
                    variant,
                };
                let mut list = depths.sorted();
                renderer.render_list(&mut rgba, &mut list, frame_count, &None, &[], false)?;
                if empty_stage
                    && (rgba.chunks_exact(4).any(|p| p[3] != 0) || !dictionary.fields.is_empty())
                {
                    return Err("zero-area SWF stage contains visual or live text content".into());
                }
                if separate_layers {
                    let mut frame_layers = Vec::new();
                    let mut clips: Vec<(usize, Placement)> = Vec::new();
                    for (depth, entry) in &list {
                        clips.retain(|(until, _)| *until >= *depth);
                        if let Some(until) = entry.clip {
                            clips.push((until, entry.clone()));
                            continue;
                        }
                        if *depth == 0 || *depth >= 1000 {
                            return Err("Flash layer depth exceeds runtime profile".into());
                        }
                        let name = entry.name.as_deref().unwrap_or("");
                        let origin_x = ((entry.matrix.tx - bounds[0]) / 20.0 * 65536.0).round();
                        let origin_y = ((entry.matrix.ty - bounds[2]) / 20.0 * 65536.0).round();
                        if [origin_x, origin_y]
                            .iter()
                            .any(|&v| !v.is_finite() || v < i32::MIN as f64 || v > i32::MAX as f64)
                        {
                            return Err("Flash object origin exceeds fixed-point geometry".into());
                        }
                        let dynamic = dictionary
                            .fields
                            .get(&entry.id)
                            .filter(|f| !f.variable.is_empty() || !name.is_empty());
                        if let Some(field) = dynamic {
                            if !clips.is_empty() {
                                return Err(format!(
                                    "clipped live Flash text requires explicit conversion: field {name:?}, {} clips",
                                    clips.len()
                                ));
                            }
                            let x =
                                ((entry.matrix.tx + field.bounds[0] - bounds[0]) / 20.0).floor();
                            let y =
                                ((entry.matrix.ty + field.bounds[2] - bounds[2]) / 20.0).floor();
                            let w = ((field.bounds[1] - field.bounds[0]) / 20.0).ceil().max(1.0);
                            let h = ((field.bounds[3] - field.bounds[2]) / 20.0).ceil().max(1.0);
                            let field_name = if name.is_empty() {
                                format!("@field_{depth}")
                            } else {
                                name.to_string()
                            };
                            frame_layers.push(json!({"channel": depth, "name": field_name, "x": x as i64, "y": y as i64,
                            "width": w as usize, "height": h as usize, "originX": origin_x as i32, "originY": origin_y as i32,
                            "affine": true, "matrix": [js::num(entry.matrix.a), js::num(entry.matrix.b),
                                js::num(entry.matrix.c), js::num(entry.matrix.d),
                                js::num((entry.matrix.tx - bounds[0]) / 20.0), js::num((entry.matrix.ty - bounds[2]) / 20.0)]}));
                        } else {
                            let mut pixels = vec![0u8; width * height * 4];
                            let mut isolated: Vec<(usize, Placement)> = clips
                                .iter()
                                .enumerate()
                                .map(|(i, (_, p))| (i, p.clone()))
                                .collect();
                            isolated.push((*depth, entry.clone()));
                            renderer.render_list(
                                &mut pixels,
                                &mut isolated,
                                frame_count,
                                &None,
                                &[],
                                false,
                            )?;
                            let (mut left, mut top, mut right, mut bottom) = (width, height, 0, 0);
                            for (i, p) in pixels.chunks_exact(4).enumerate() {
                                if p[3] != 0 {
                                    left = left.min(i % width);
                                    top = top.min(i / width);
                                    right = right.max(i % width + 1);
                                    bottom = bottom.max(i / width + 1);
                                }
                            }
                            if right == 0 {
                                if name.is_empty() {
                                    continue;
                                }
                                left = 0;
                                top = 0;
                                right = 1;
                                bottom = 1;
                            }
                            let mut cropped =
                                Vec::with_capacity((right - left) * (bottom - top) * 4);
                            for y in top..bottom {
                                cropped.extend_from_slice(
                                    &pixels[(y * width + left) * 4..(y * width + right) * 4],
                                );
                            }
                            let plane = RasterLayer {
                                width: right - left,
                                height: bottom - top,
                                rgba: cropped,
                            };
                            let asset = layer_sink.as_deref_mut().unwrap()(&plane)?;
                            frame_layers.push(
                                json!({"channel": depth, "name": name, "asset": asset,
                            "x": left, "y": top, "width": plane.width, "height": plane.height,
                            "originX": origin_x as i32, "originY": origin_y as i32}),
                            );
                        }
                    }
                    layers.push(json!(frame_layers));
                }
                // Sprite first-placement frames persist on the live depth table.
                for (depth, placement) in list {
                    if let Some(live) = depths.get_mut(depth) {
                        live.placed = placement.placed;
                    }
                }
                for (depth, entry) in &depths.0 {
                    let depth = *depth;
                    if !dictionary.fields.contains_key(&entry.id) {
                        if let Some(name) = entry.name.as_ref().filter(|n| !n.is_empty()) {
                            if !named.iter().any(|c| c["name"] == name.as_str()) {
                                named.push(json!({"name": name, "depth": depth,
                                "kind": if dictionary.sprites.contains_key(&entry.id) { "sprite" } else { "shape" }}));
                            }
                        }
                        continue;
                    }
                    if !field_placements
                        .iter()
                        .any(|f| f.0 == depth && f.1 == entry.id)
                    {
                        field_placements.push((
                            depth,
                            entry.id,
                            entry.matrix,
                            entry.name.clone(),
                            frame_count + 1,
                        ));
                    }
                }
                if let Some(write) = sink.as_deref_mut() {
                    write(width, height, &rgba)?;
                } else {
                    frames.push(rgba);
                }
            } // Frame-major root poses and phase variants, including live layers.
            frame_count += 1;
        } else if SKIPPED.contains(&tag) {
        } else if tag != 0 {
            return Err(format!("unsupported static SWF tag {tag}"));
        }
    }
    if frame_count == 0 || frame_count != declared_frames {
        return Err("SWF timeline frame count disagrees".into());
    }
    let color_json = |c: [f64; 4]| Value::Array(c.iter().map(|&v| js::num(v)).collect());
    let fields = field_placements
        .iter()
        .map(|(depth, id, matrix, name, frame)| {
            let field = &dictionary.fields[id];
            let font = field.font.and_then(|f| dictionary.fonts.get(&f));
            json!({
                "frame": frame, "name": if separate_layers && name.as_ref().is_none_or(String::is_empty) && !field.variable.is_empty()
                    { Some(format!("@field_{depth}")) } else { name.clone() },
                "variable": field.variable, "text": field.text, "html": field.html,
                "fontName": font.map(|f| json!(f.name)).unwrap_or(Value::Null),
                "fontHeight": js::num(field.font_height / 20.0), "color": color_json(field.color),
                "align": js::num(field.align), "wordWrap": field.word_wrap, "multiline": field.multiline, "embedFonts": field.embed_fonts,
                "leading": js::num(field.leading / 20.0), "leftMargin": js::num(field.left_margin / 20.0),
                "rightMargin": js::num(field.right_margin / 20.0), "indent": js::num(field.indent / 20.0),
                "matrix": [js::num(matrix.a), js::num(matrix.b), js::num(matrix.c), js::num(matrix.d),
                    js::num((matrix.tx - bounds[0]) / 20.0), js::num((matrix.ty - bounds[2]) / 20.0)],
                "localBounds": {"left": js::num(field.bounds[0] / 20.0), "top": js::num(field.bounds[2] / 20.0),
                    "width": js::num((field.bounds[1] - field.bounds[0]) / 20.0),
                    "height": js::num((field.bounds[3] - field.bounds[2]) / 20.0)},
                "bounds": {
                    "left": js::num((matrix.tx + field.bounds[0] - bounds[0]) / 20.0),
                    "top": js::num((matrix.ty + field.bounds[2] - bounds[2]) / 20.0),
                    "width": js::num((field.bounds[1] - field.bounds[0]) / 20.0),
                    "height": js::num((field.bounds[3] - field.bounds[2]) / 20.0),
                },
            })
        })
        .collect();
    // Map iteration order: sprites in definition order.
    let mut sprite_ids: Vec<usize> = Vec::new();
    for Tag { tag, payload } in &tags {
        if *tag == 39 {
            sprite_ids.push(u16le(payload, 0)?);
        }
    }
    for id in sprite_ids {
        for (frame, ops) in &dictionary.sprites[&id].evidence {
            evidence.push(json!({"sprite": id, "frame": frame, "ops": ops}));
        }
    }
    let counts = (
        dictionary.sprites.len(),
        dictionary.fonts.len(),
        dictionary.fields.len(),
    );
    if dictionary
        .videos
        .values()
        .any(|v| v.frames.len() != v.count)
    {
        return Err("SWF video frame count disagrees".into());
    }
    let audio = match audio_stream {
        Some(stream) => stream.finish(frame_count)?,
        None => None,
    };
    Ok(Timeline {
        width,
        height,
        rate,
        frames,
        variant_count,
        audio,
        labels,
        stop_frames,
        effects,
        ignored_actions: evidence,
        fields,
        named_children: named,
        helpers,
        host_fullscreen,
        layers,
        counts,
    })
}

pub struct VectorShape {
    pub width: usize,
    pub height: usize,
    pub reg_x: i64,
    pub reg_y: i64,
    pub pixels: Vec<u8>,
}

pub fn vector_shape_pixels(envelope: &[u8]) -> R<VectorShape> {
    let be32 = |at: usize| u32::from_be_bytes(envelope[at..at + 4].try_into().unwrap()) as usize;
    if envelope.len() < 20
        || be32(0) != envelope.len()
        || be32(4) != 1
        || be32(8) != envelope.len() - 12
    {
        return Err("unsupported vectorShape XMED envelope".into());
    }
    let scene = timeline(&envelope[12..], None, None)?;
    if scene.frames.is_empty() || scene.frames.iter().any(|f| f != &scene.frames[0]) {
        return Err("vectorShape must have one static SWF pose".into());
    }
    if scene.counts != (0, 0, 0)
        || scene.audio.is_some()
        || !scene.helpers.is_empty()
        || scene.host_fullscreen
        || !scene.ignored_actions.is_empty()
        || !scene.stop_frames.is_empty()
        || scene.effects.filters != 0
        || scene.effects.blend_modes != 0
        || scene.effects.clip_actions != 0
    {
        return Err("vectorShape timeline needs explicit conversion".into());
    }
    let (width, height) = (scene.width, scene.height);
    let rgba = &scene.frames[0];
    let mut pixels = vec![255u8; width * height * 2];
    for i in (1..pixels.len()).step_by(2) {
        pixels[i] = 254;
    }
    for i in 0..width * height {
        if rgba[i * 4 + 3] < 128 {
            continue;
        }
        let word = ((rgba[i * 4] >> 3) as u16) << 11
            | ((rgba[i * 4 + 1] >> 3) as u16) << 6
            | ((rgba[i * 4 + 2] >> 3) as u16) << 1
            | 1;
        pixels[i * 2..i * 2 + 2].copy_from_slice(&word.to_be_bytes());
    }
    Ok(VectorShape {
        width,
        height,
        reg_x: (width / 2) as i64,
        reg_y: (height / 2) as i64,
        pixels,
    })
}

pub struct FlashFrames {
    pub version: u8,
    pub width: usize,
    pub height: usize,
    pub reg_x: i64,
    pub reg_y: i64,
    pub rate: f64,
    pub frames: Vec<Vec<u8>>,
    pub variant_count: u8,
    pub audio: Option<audio::Audio>,
    pub labels: Vec<Value>,
    pub stop_frames: Vec<Value>,
    pub ignored_actions: Vec<Value>,
    pub fields: Vec<Value>,
    pub named_children: Vec<Value>,
    pub helpers: Vec<Value>,
    pub host_fullscreen: bool,
    pub layers: Vec<Value>,
    pub effects: Effects,
    pub runtime: Option<Value>,
    pub sounds: Vec<(usize, crate::convert::d5media::Sound)>,
}

#[path = "swf_font_uses.rs"]
mod font_uses;
/// Proven finite AVM1 font uses; dynamic unknowns remain strict runtime lookups.
pub fn action_font_uses(value: &Value) -> R<Vec<Value>> {
    font_uses::actions(value)
}

#[path = "swf_fonts.rs"]
mod fonts;
/// Original, untessellated quadratic outlines for exact system-font matching.
pub fn font_outlines(swf: &[u8]) -> R<Vec<Value>> {
    fonts::outlines(swf)
}

pub fn flash_frames(swf: &[u8]) -> R<FlashFrames> {
    flash_frames_into(swf, None)
}

/// Render and consume each frame before allocating the next. Returned frames
/// are empty when a sink is supplied; timeline metadata remains complete.
pub fn flash_frames_into(swf: &[u8], sink: Option<&mut FrameSink<'_>>) -> R<FlashFrames> {
    flash_frames_with_layers(swf, sink, None)
}

pub fn flash_frames_with_layers(
    swf: &[u8],
    sink: Option<&mut FrameSink<'_>>,
    layers: Option<&mut LayerSink<'_>>,
) -> R<FlashFrames> {
    if runtime::required(swf)? {
        let layers = layers.ok_or("interactive SWF requires a retained-layer asset sink")?;
        let result = runtime::convert(swf, layers)?;
        let pixels = vec![0; result.width * result.height * 4];
        let mut frames = Vec::new();
        if let Some(sink) = sink {
            for _ in 0..result.frames {
                sink(result.width, result.height, &pixels)?;
            }
        } else {
            frames.resize(result.frames, pixels);
        }
        return Ok(FlashFrames {
            version: swf[3],
            width: result.width,
            height: result.height,
            reg_x: (result.width / 2) as i64,
            reg_y: (result.height / 2) as i64,
            rate: result.rate,
            frames,
            variant_count: 1,
            audio: result.audio,
            labels: Vec::new(),
            stop_frames: Vec::new(),
            ignored_actions: Vec::new(),
            fields: result.fields,
            named_children: Vec::new(),
            helpers: Vec::new(),
            host_fullscreen: false,
            layers: result.layers,
            effects: Effects::default(),
            sounds: result.sounds,
            runtime: Some(
                json!({"clips":result.clips,"buttons":result.buttons,"exports":result.exports,"initializers":result.initializers}),
            ),
        });
    }
    let scene = timeline(swf, sink, layers)?;
    Ok(FlashFrames {
        version: swf[3],
        width: scene.width,
        height: scene.height,
        reg_x: (scene.width / 2) as i64,
        reg_y: (scene.height / 2) as i64,
        rate: scene.rate,
        frames: scene.frames,
        variant_count: scene.variant_count,
        audio: scene.audio,
        labels: scene.labels,
        stop_frames: scene.stop_frames,
        ignored_actions: scene.ignored_actions,
        fields: scene.fields,
        named_children: scene.named_children,
        helpers: scene.helpers,
        host_fullscreen: scene.host_fullscreen,
        layers: scene.layers,
        effects: scene.effects,
        runtime: None,
        sounds: Vec::new(),
    })
}

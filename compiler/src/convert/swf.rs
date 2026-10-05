//! Offline conversion of the static SWF envelopes used by D8/D10 vectorShape
//! XMED and D10 Flash members (tools/director/swf-model.mjs). Static
//! visuals flatten: shapes (solid and gradient fills, line styles, curves),
//! nested sprite timelines, embedded font glyphs and static text. The
//! dynamic surface is recorded, never guessed. Every floating-point
//! expression keeps the reference's evaluation order, so the pixels match.

use std::collections::HashMap;

use serde_json::{json, Value};

use super::js;

type R<T> = Result<T, String>;

fn u16le(b: &[u8], at: usize) -> R<usize> {
    b.get(at..at + 2).map(|s| u16::from_le_bytes([s[0], s[1]]) as usize).ok_or_else(|| "truncated SWF record".into())
}
fn i16le(b: &[u8], at: usize) -> R<i64> {
    b.get(at..at + 2).map(|s| i16::from_le_bytes([s[0], s[1]]) as i64).ok_or_else(|| "truncated SWF record".into())
}
fn u32le(b: &[u8], at: usize) -> R<usize> {
    b.get(at..at + 4).map(|s| u32::from_le_bytes(s.try_into().unwrap()) as usize).ok_or_else(|| "truncated SWF record".into())
}
fn byte(b: &[u8], at: usize) -> R<u8> {
    b.get(at).copied().ok_or_else(|| "truncated SWF record".into())
}
fn jmax(a: f64, b: f64) -> f64 {
    if a.is_nan() || b.is_nan() { f64::NAN } else { a.max(b) }
}
fn jmin(a: f64, b: f64) -> f64 {
    if a.is_nan() || b.is_nan() { f64::NAN } else { a.min(b) }
}
/// `parseInt(value, 10)`: leading whitespace, a sign, then digits.
fn parse_int(value: &str) -> f64 {
    let t = value.trim_start_matches(super::source::js_space);
    let (sign, digits) = match t.as_bytes().first() {
        Some(b'-') => (-1.0, &t[1..]),
        Some(b'+') => (1.0, &t[1..]),
        _ => (1.0, t),
    };
    let end = digits.find(|c: char| !c.is_ascii_digit()).unwrap_or(digits.len());
    if end == 0 { f64::NAN } else { sign * digits[..end].parse::<f64>().unwrap() }
}
fn index_of(b: &[u8], value: u8, from: usize) -> Option<usize> {
    b.get(from..)?.iter().position(|&x| x == value).map(|i| i + from)
}

struct Bits<'a> {
    bytes: &'a [u8],
    bit: usize,
}

impl<'a> Bits<'a> {
    fn new(bytes: &'a [u8], offset: usize) -> Self {
        Self { bytes, bit: offset * 8 }
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
        Ok(if count > 0 && value >= 2f64.powi(count as i32 - 1) { value - 2f64.powi(count as i32) } else { value })
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
        let r = [self.signed(count)?, self.signed(count)?, self.signed(count)?, self.signed(count)?];
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

struct Styles {
    p: usize,
    fills: Vec<Fill>,
    lines: Vec<Line>,
}

fn color(b: &[u8], p: usize, alpha: bool) -> R<[f64; 4]> {
    Ok([
        byte(b, p)? as f64,
        byte(b, p + 1)? as f64,
        byte(b, p + 2)? as f64,
        if alpha { byte(b, p + 3)? as f64 } else { 255.0 },
    ])
}

fn gradient_style(b: &[u8], state: &mut Styles, version: u32, radial: bool, focal: bool) -> R<Fill> {
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
    Ok(Fill::Gradient(Gradient { matrix, stops, radial }))
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
        } else {
            return Err("SWF bitmap fill needs explicit conversion".into());
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
        state.lines.push(Line { width, color: color(b, p, alpha)? });
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
        Self { x: 0.0, y: 0.0, fill0: 0, fill1: 0, line: 0, fill_base: 0, line_base: 0, edges: Vec::new() }
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
            self.push(u * u * sx + 2.0 * u * t * qx + t * t * ex, u * u * sy + 2.0 * u * t * qy + t * t * ey)?;
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

fn parse_shape(b: &[u8], version: u32) -> R<(usize, Shape)> {
    if b.len() < 4 {
        return Err("truncated SWF shape".into());
    }
    let id = u16le(b, 0)?;
    let mut bits = Bits::new(b, 2);
    bits.rect()?;
    if version == 4 {
        bits.rect()?;
        if (bits.unsigned(8)? as u32) & !3 != 0 {
            return Err("SWF shape4 winding/reserved flags need explicit conversion".into());
        }
    }
    let mut state = Styles { p: bits.align(), fills: Vec::new(), lines: Vec::new() };
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
        let mut on_style: Option<&mut dyn FnMut(usize, &mut Collector) -> R<usize>> = Some(&mut callback);
        shape_records(&mut bits, &mut collector, &mut on_style)?;
    }
    let state = state_cell.into_inner();
    if bits.align() != b.len() || collector.edges.is_empty() {
        return Err("unconsumed or empty SWF shape".into());
    }
    for e in &collector.edges {
        if e.f0 > state.fills.len() || e.f1 > state.fills.len() || e.ln > state.lines.len() {
            return Err("SWF edge references absent style".into());
        }
    }
    Ok((id, Shape { fills: state.fills, lines: state.lines, edges: collector.edges }))
}

fn parse_glyph(b: &[u8], start: usize, end: usize) -> R<Shape> {
    let mut collector = Collector::new();
    let mut bits = Bits::new(b, start);
    shape_records(&mut bits, &mut collector, &mut None)?;
    if bits.align() > end {
        return Err("SWF glyph exceeds its extent".into());
    }
    Ok(Shape { fills: vec![Fill::Color([0.0, 0.0, 0.0, 255.0])], lines: Vec::new(), edges: collector.edges })
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
        let glyphs = (0..count).map(|i| parse_glyph(b, offsets[i], offsets[i + 1])).collect::<R<_>>()?;
        return Ok((id, Font { name: String::new(), glyphs, codes: None, advances: None, ascent: None, em: 1024.0 }));
    }
    let flags = byte(b, 2)?;
    let wide_offsets = flags & 8 != 0;
    let wide_codes = tag == 75 || flags & 4 != 0;
    let has_layout = flags & 0x80 != 0;
    let name_length = byte(b, 4)? as usize;
    let name = js::latin1(b.get(5..5 + name_length).ok_or("truncated SWF font")?).trim_end_matches('\0').to_string();
    let mut p = 5 + name_length;
    let count = u16le(b, p)?;
    p += 2;
    let table = p;
    let mut offsets = Vec::new();
    for i in 0..=count {
        offsets.push(table + if wide_offsets { u32le(b, table + i * 4)? } else { u16le(b, table + i * 2)? });
    }
    let glyphs = (0..count).map(|i| parse_glyph(b, offsets[i], offsets[i + 1])).collect::<R<_>>()?;
    p = offsets[count];
    let mut codes = Vec::new();
    for _ in 0..count {
        codes.push(if wide_codes { u16le(b, p)? } else { byte(b, p)? as usize });
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
    Ok((id, Font { name, glyphs, codes: Some(codes), advances, ascent, em: if tag == 75 { 20480.0 } else { 1024.0 } }))
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
    let (mut font, mut color_value, mut x, mut y, mut height) = (None, [0.0, 0.0, 0.0, 255.0], 0.0, 0.0, 240.0);
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
        records.push(TextRecord { font, color: color_value, height, glyphs });
    }
    Ok((id, Text { matrix, records }))
}

#[derive(Clone)]
struct Field {
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
            let boundary = j >= chars.len() || !(chars[j].is_ascii_alphanumeric() || chars[j] == '_');
            if matches!(tag.as_str(), "p" | "font" | "br") && boundary {
                if let Some(close) = chars[j..].iter().position(|&c| c == '>') {
                    let attrs: String = chars[j..j + close].iter().collect();
                    let mut rest = attrs.as_str();
                    while let Some(eq) = rest.find("=\"") {
                        let key_start = rest[..eq].rfind(|c: char| !(c.is_ascii_alphanumeric() || c == '_')).map(|k| k + 1).unwrap_or(0);
                        let key = &rest[key_start..eq];
                        let value_start = eq + 2;
                        let Some(value_end) = rest[value_start..].find('"') else { break };
                        if !key.is_empty() {
                            attributes.insert(key.to_lowercase(), rest[value_start..value_start + value_end].to_string());
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

fn parse_actions(payload: &[u8]) -> R<(Vec<String>, bool)> {
    let mut ops = Vec::new();
    let (mut p, mut stop, mut top_level) = (0usize, false, true);
    while p < payload.len() {
        let code = payload[p];
        p += 1;
        if code == 0 {
            break;
        }
        let mut length = 0;
        if code >= 0x80 {
            length = u16le(payload, p)?;
            p += 2;
        }
        if code == 0x07 && top_level {
            stop = true;
        }
        if code == 0x9b || code == 0x8e {
            let body = u16le(payload, p + length - 2)?;
            p += length + body;
            ops.push(if code == 0x8e { "DefineFunction2" } else { "DefineFunction" }.to_string());
            continue;
        }
        if code == 0x9d || code == 0x99 {
            top_level = false;
        }
        ops.push(format!("0x{code:x}"));
        p += length;
    }
    Ok((ops, stop))
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
        tags.push(Tag { tag, payload: &b[p..p + length] });
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
    pub blend_modes: u32,
    pub clip_actions: u32,
}

fn skip_filters(b: &[u8], mut p: usize, effects: &mut Effects) -> R<usize> {
    let count = byte(b, p)?;
    p += 1;
    for _ in 0..count {
        let kind = byte(b, p)?;
        p += 1;
        effects.filters += 1;
        p += match kind {
            0 => 23,
            1 => 9,
            2 => 15,
            3 => 27,
            4 | 7 => 1 + byte(b, p)? as usize * 5 + 19,
            5 => 15 + 4 * byte(b, p)? as usize * byte(b, p + 1)? as usize,
            6 => 80,
            _ => return Err("unknown SWF filter".into()),
        };
    }
    Ok(p)
}

#[derive(Clone)]
struct Placement {
    id: usize,
    matrix: Matrix,
    cxform: Option<Cxform>,
    clip: Option<usize>,
    name: Option<String>,
    placed: Option<usize>,
}

fn read_placement(tag: usize, payload: &[u8], depths: &mut Depths, effects: &mut Effects) -> R<()> {
    if tag == 4 {
        if payload.len() < 5 {
            return Err("truncated SWF placement".into());
        }
        let matrix = Bits::new(payload, 4).matrix()?;
        depths.insert(u16le(payload, 2)?, Placement { id: u16le(payload, 0)?, matrix, cxform: None, clip: None, name: None, placed: None });
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
    let id = if flags & 2 != 0 { Some(u16le(payload, q)?) } else { previous.as_ref().map(|p| p.id) };
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
    if flags & 16 != 0 {
        q += 2;
    }
    if flags & 32 != 0 {
        let end = index_of(payload, 0, q).ok_or("SWF placement extras need explicit conversion")?;
        name = Some(js::latin1(&payload[q..end]));
        q = end + 1;
    }
    if flags & 64 != 0 {
        clip = Some(u16le(payload, q)?);
        q += 2;
    }
    if flags2 & 1 != 0 {
        q = skip_filters(payload, q, effects)?;
    }
    if flags2 & 2 != 0 {
        effects.blend_modes += 1;
        q += 1;
    }
    if flags & 128 != 0 {
        effects.clip_actions += 1;
        q = payload.len();
    }
    if q != payload.len() {
        return Err("SWF placement extras need explicit conversion".into());
    }
    let valid = id.is_some_and(|i| i != 0) && matrix.is_some()
        && !((flags & 1 == 0) && previous.is_some()) && !((flags & 1 != 0) && previous.is_none());
    if !valid {
        return Err("invalid SWF placement sequence".into());
    }
    let id = id.unwrap();
    let restarted = flags & 2 != 0 && previous.as_ref().is_none_or(|p| p.id != id);
    depths.insert(depth, Placement {
        id,
        matrix: matrix.unwrap(),
        cxform: if flags & 8 != 0 { cxform } else { previous.as_ref().and_then(|p| p.cxform.clone()) },
        clip: if flags & 64 != 0 { clip } else { previous.as_ref().and_then(|p| p.clip) },
        name: if flags & 32 != 0 { name } else { previous.as_ref().and_then(|p| p.name.clone()) },
        placed: if restarted { None } else { previous.as_ref().and_then(|p| p.placed) },
    });
    Ok(())
}

/// A depth table with JS Map semantics: re-setting a depth keeps its
/// position, deleting and re-adding moves it to the end.
#[derive(Clone, Default)]
struct Depths(Vec<(usize, Placement)>);

impl Depths {
    fn get(&self, depth: usize) -> Option<&Placement> {
        self.0.iter().find(|e| e.0 == depth).map(|e| &e.1)
    }
    fn get_mut(&mut self, depth: usize) -> Option<&mut Placement> {
        self.0.iter_mut().find(|e| e.0 == depth).map(|e| &mut e.1)
    }
    fn insert(&mut self, depth: usize, value: Placement) {
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
}

#[derive(Default)]
struct Dictionary {
    shapes: HashMap<usize, Shape>,
    sprites: HashMap<usize, Sprite>,
    fonts: HashMap<usize, Font>,
    texts: HashMap<usize, Text>,
    fields: HashMap<usize, Field>,
}

impl Dictionary {
    fn has(&self, id: usize) -> bool {
        self.shapes.contains_key(&id) || self.sprites.contains_key(&id) || self.fonts.contains_key(&id)
            || self.texts.contains_key(&id) || self.fields.contains_key(&id)
    }
}

fn parse_sprite(payload: &[u8], dictionary: &Dictionary, effects: &mut Effects) -> R<(usize, Sprite)> {
    let id = u16le(payload, 0)?;
    let declared = u16le(payload, 2)?;
    let (tags, _) = tag_stream(payload, 4)?;
    let mut depths = Depths::default();
    let mut frames = Vec::new();
    let mut hold = None;
    let mut evidence = Vec::new();
    for Tag { tag, payload: body } in tags {
        if tag == 4 || tag == 26 || tag == 70 {
            read_placement(tag, body, &mut depths, effects)?;
        } else if tag == 5 || tag == 28 {
            let depth = u16le(body, if tag == 5 { 2 } else { 0 })?;
            if !depths.remove(depth) {
                return Err("SWF removal references empty depth".into());
            }
        } else if tag == 1 {
            frames.push(depths.clone());
            if frames.len() > 4096 {
                return Err("SWF sprite frame bound".into());
            }
        } else if tag == 12 {
            let (ops, stop) = parse_actions(body)?;
            if stop && hold.is_none() {
                hold = Some(frames.len());
            }
            if ops.iter().any(|op| op != "0x88" && op != "0x7") {
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
    Ok((id, Sprite { frames, hold, evidence }))
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
    let mut t = if g.radial { (gx * gx + gy * gy).sqrt() / 16384.0 } else { (gx + 16384.0) / 32768.0 };
    t = t.max(0.0).min(1.0) * 255.0;
    let stops = &g.stops;
    if t <= stops[0].0 {
        return Ok(stops[0].1);
    }
    for i in 1..stops.len() {
        if t <= stops[i].0 {
            let span = if stops[i].0 - stops[i - 1].0 != 0.0 { stops[i].0 - stops[i - 1].0 } else { 1.0 };
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
        rgba[i + c] = js::round((color[c] * alpha + rgba[i + c] as f64 * dst_alpha * (255.0 - alpha) / 255.0) / out) as u8;
    }
    rgba[i + 3] = js::round(out) as u8;
}

fn transform_color(cxform: &Option<Cxform>, color: [f64; 4]) -> [f64; 4] {
    match cxform {
        None => color,
        Some(x) => [0, 1, 2, 3].map(|c| jmin(255.0, jmax(0.0, js::round(color[c] * x.mult[c] / 256.0 + x.add[c])))),
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

struct Canvas {
    width: usize,
    height: usize,
    origin_x: f64,
    origin_y: f64,
}

#[allow(clippy::too_many_arguments)]
fn paint_shape(rgba: &mut [u8], canvas: &Canvas, shape: &Shape, m: &Matrix, cxform: &Option<Cxform>, stencils: Option<&[Vec<u8>]>) -> R<()> {
    let det = m.a * m.d - m.b * m.c;
    if det == 0.0 {
        return Err("degenerate SWF placement matrix".into());
    }
    let (mut min_x, mut min_y, mut max_x, mut max_y) = (f64::INFINITY, f64::INFINITY, f64::NEG_INFINITY, f64::NEG_INFINITY);
    let margin = shape.lines.iter().fold(20.0f64, |most, l| most.max(l.width));
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
    let x1 = ((max_x + margin - canvas.origin_x) / 20.0).ceil().min(w - 1.0);
    let y0 = ((min_y - margin - canvas.origin_y) / 20.0).floor().max(0.0);
    let y1 = ((max_y + margin - canvas.origin_y) / 20.0).ceil().min(h - 1.0);
    let mut y = y0;
    while y <= y1 {
        let mut x = x0;
        while x <= x1 {
            let px = canvas.origin_x + (x + 0.5) * 20.0 - m.tx;
            let py = canvas.origin_y + (y + 0.5) * 20.0 - m.ty;
            let sx = (m.d * px - m.c * py) / det;
            let sy = (m.a * py - m.b * px) / det;
            let mut paint: Option<[f64; 4]> = None;
            for s in 1..=shape.fills.len() {
                let mut winding = 0i64;
                for e in &shape.edges {
                    if e.f1 != s && e.f0 != s {
                        continue;
                    }
                    let down = e.f1 == s;
                    let (ax, ay, bx, by) = if down { (e.x0, e.y0, e.x1, e.y1) } else { (e.x1, e.y1, e.x0, e.y0) };
                    if (ay > sy) != (by > sy) {
                        let crossing = ax + (sy - ay) * (bx - ax) / (by - ay);
                        if sx < crossing {
                            winding += if by > ay { 1 } else { -1 };
                        }
                    }
                }
                if winding != 0 {
                    paint = Some(match &shape.fills[s - 1] {
                        Fill::Gradient(g) => gradient_color(g, sx, sy)?,
                        Fill::Color(c) => *c,
                    });
                }
            }
            for s in 1..=shape.lines.len() {
                let radius = shape.lines[s - 1].width.max(20.0) / 2.0;
                let limit = radius * radius;
                let mut hit = false;
                for e in &shape.edges {
                    if e.ln != s {
                        continue;
                    }
                    let (dx, dy) = (e.x1 - e.x0, e.y1 - e.y0);
                    let length_sq = dx * dx + dy * dy;
                    let t = if length_sq != 0.0 {
                        (((sx - e.x0) * dx + (sy - e.y0) * dy) / length_sq).max(0.0).min(1.0)
                    } else {
                        0.0
                    };
                    let qx = e.x0 + t * dx - sx;
                    let qy = e.y0 + t * dy - sy;
                    if qx * qx + qy * qy <= limit {
                        hit = true;
                        break;
                    }
                }
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
            let index = glyph_for(font, ch as usize).ok_or_else(|| format!("SWF field glyph {} unavailable", ch as u32))?;
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
            let candidate = format!("{current}{}", word.trim_end_matches(super::source::js_space));
            if !current.is_empty() && field.word_wrap && width_of(&candidate)? > inner_right - inner_left {
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
    pub labels: Vec<Value>,
    pub stop_frames: Vec<Value>,
    pub effects: Effects,
    pub ignored_actions: Vec<Value>,
    pub fields: Vec<Value>,
    pub named_children: Vec<Value>,
    counts: (usize, usize, usize),
}

struct Renderer<'a> {
    dictionary: &'a Dictionary,
    canvas: Canvas,
}

impl Renderer<'_> {
    fn render_list(&self, rgba: &mut [u8], list: &mut [(usize, Placement)], frame: usize, cxform: &Option<Cxform>, stencils: &[Vec<u8>], nested: bool) -> R<()> {
        let mut clips: Vec<(usize, Vec<u8>)> = Vec::new();
        for (depth, entry) in list.iter_mut() {
            clips.retain(|(until, _)| *until >= *depth);
            if let Some(until) = entry.clip {
                let mut scratch = vec![0u8; self.canvas.width * self.canvas.height * 4];
                let mut copy = entry.clone();
                copy.clip = None;
                self.render_entry(&mut scratch, &mut copy, frame, &None, &[], nested)?;
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

    fn render_entry(&self, rgba: &mut [u8], entry: &mut Placement, frame: usize, cxform: &Option<Cxform>, stencils: &[Vec<u8>], nested: bool) -> R<()> {
        let d = self.dictionary;
        let combined = compose(cxform, &entry.cxform);
        let stencils = if stencils.is_empty() { None } else { Some(stencils) };
        if let Some(shape) = d.shapes.get(&entry.id) {
            return paint_shape(rgba, &self.canvas, shape, &entry.matrix, &combined, stencils);
        }
        if let Some(text) = d.texts.get(&entry.id) {
            for record in &text.records {
                let font = record.font.and_then(|f| d.fonts.get(&f)).ok_or("SWF text references absent font")?;
                let scale = record.height / font.em;
                for &(index, x, y) in &record.glyphs {
                    let glyph = font.glyphs.get(index).ok_or("SWF text references absent glyph")?;
                    let m = multiply(&entry.matrix, &multiply(&text.matrix, &Matrix { a: scale, b: 0.0, c: 0.0, d: scale, tx: x, ty: y }));
                    let shape = Shape { fills: vec![Fill::Color(record.color)], lines: Vec::new(), edges: glyph.edges.clone() };
                    paint_shape(rgba, &self.canvas, &shape, &m, &combined, stencils)?;
                }
            }
            return Ok(());
        }
        if let Some(field) = d.fields.get(&entry.id) {
            if !field.variable.is_empty() || (entry.name.as_ref().is_some_and(|n| !n.is_empty()) && !nested) || field.text.is_empty() {
                return Ok(());
            }
            let (text, attributes) = if field.html { parse_field_html(&field.text)? } else { (field.text.clone(), HashMap::new()) };
            let font = field.font.and_then(|f| d.fonts.get(&f)).ok_or("SWF field references absent font")?;
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
                let glyph = index.and_then(|i| font.glyphs.get(i)).ok_or("SWF field glyph unavailable")?;
                let m = multiply(&entry.matrix, &Matrix { a: scale, b: 0.0, c: 0.0, d: scale, tx: x, ty: y });
                let shape = Shape { fills: vec![Fill::Color(color)], lines: Vec::new(), edges: glyph.edges.clone() };
                paint_shape(rgba, &self.canvas, &shape, &m, &combined, stencils)?;
            }
            return Ok(());
        }
        if let Some(sprite) = d.sprites.get(&entry.id) {
            if entry.placed.is_none() {
                entry.placed = Some(frame);
            }
            let local = frame - entry.placed.unwrap();
            let local = match sprite.hold {
                Some(hold) if local >= hold => hold.max(1) - 1,
                _ => local % sprite.frames.len(),
            };
            let mut list: Vec<(usize, Placement)> = sprite.frames[local]
                .sorted()
                .into_iter()
                .map(|(depth, inner)| {
                    let mut child = inner.clone();
                    child.matrix = multiply(&entry.matrix, &inner.matrix);
                    child.placed = Some(0);
                    (depth, child)
                })
                .collect();
            return self.render_list(rgba, &mut list, 0, &combined, stencils.unwrap_or(&[]), true);
        }
        Err("SWF placement references absent character".into())
    }
}

fn timeline(b: &[u8]) -> R<Timeline> {
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
    let width = (bounds[1] - bounds[0]) / 20.0;
    let height = (bounds[3] - bounds[2]) / 20.0;
    if width.fract() != 0.0 || height.fract() != 0.0 || width < 1.0 || height < 1.0 || width > 2048.0 || height > 2048.0 {
        return Err("unsupported SWF stage bounds".into());
    }
    let (tags, end) = tag_stream(b, p + 4)?;
    if end != b.len() || !tags.last().is_some_and(|t| t.tag == 0 && t.payload.is_empty()) {
        return Err("unterminated SWF timeline".into());
    }
    let (width, height) = (width as usize, height as usize);
    let mut dictionary = Dictionary::default();
    let mut depths = Depths::default();
    let mut frames = Vec::new();
    let mut labels = Vec::new();
    let mut stop_frames = Vec::new();
    let mut evidence: Vec<Value> = Vec::new();
    let mut field_placements: Vec<(usize, usize, Matrix, Option<String>, usize)> = Vec::new();
    let mut named: Vec<Value> = Vec::new();
    let mut effects = Effects::default();
    let define = |dictionary: &Dictionary, id: usize| -> R<()> {
        if dictionary.has(id) { Err("duplicate SWF character".into()) } else { Ok(()) }
    };
    for Tag { tag, payload } in &tags {
        let (tag, payload) = (*tag, *payload);
        if let Some(version) = shape_version(tag) {
            let (id, shape) = parse_shape(payload, version)?;
            define(&dictionary, id)?;
            dictionary.shapes.insert(id, shape);
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
            let font = dictionary.fonts.get_mut(&font_id).filter(|f| f.codes.is_none()).ok_or("SWF font info without its font")?;
            let name_length = byte(payload, 2)? as usize;
            let wide = byte(payload, 3 + name_length)? & 1 != 0;
            let mut codes = Vec::new();
            let mut q = 4 + name_length;
            while q < payload.len() {
                codes.push(if wide { u16le(payload, q)? } else { payload[q] as usize });
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
        } else if tag == 4 || tag == 26 || tag == 70 {
            read_placement(tag, payload, &mut depths, &mut effects)?;
        } else if tag == 5 || tag == 28 {
            let depth = u16le(payload, if tag == 5 { 2 } else { 0 })?;
            if !depths.remove(depth) {
                return Err("SWF removal references empty depth".into());
            }
        } else if tag == 12 || tag == 59 {
            let (ops, stop) = parse_actions(payload)?;
            if stop {
                stop_frames.push(json!(frames.len() + 1));
            }
            if ops.iter().any(|op| op != "0x88" && op != "0x7") {
                evidence.push(json!({"frame": frames.len() + 1, "ops": ops}));
            }
        } else if tag == 43 {
            labels.push(json!({"frame": frames.len() + 1, "name": js::latin1(payload).trim_end_matches('\0')}));
        } else if tag == 1 {
            if !payload.is_empty() {
                return Err("unsupported SWF frame payload".into());
            }
            if frames.len() >= 128 {
                return Err("SWF frame count exceeds conversion bounds".into());
            }
            let mut rgba = vec![0u8; width * height * 4];
            let renderer = Renderer { dictionary: &dictionary, canvas: Canvas { width, height, origin_x: bounds[0], origin_y: bounds[2] } };
            let mut list = depths.sorted();
            renderer.render_list(&mut rgba, &mut list, frames.len(), &None, &[], false)?;
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
                if !field_placements.iter().any(|f| f.0 == depth && f.1 == entry.id) {
                    field_placements.push((depth, entry.id, entry.matrix, entry.name.clone(), frames.len() + 1));
                }
            }
            frames.push(rgba);
        } else if SKIPPED.contains(&tag) {
        } else if tag != 0 {
            return Err(format!("unsupported static SWF tag {tag}"));
        }
    }
    if frames.is_empty() || frames.len() != declared_frames {
        return Err("SWF timeline frame count disagrees".into());
    }
    let color_json = |c: [f64; 4]| Value::Array(c.iter().map(|&v| js::num(v)).collect());
    let fields = field_placements
        .iter()
        .map(|(_, id, matrix, name, frame)| {
            let field = &dictionary.fields[id];
            let font = field.font.and_then(|f| dictionary.fonts.get(&f));
            json!({
                "frame": frame, "name": name, "variable": field.variable, "text": field.text, "html": field.html,
                "fontName": font.map(|f| json!(f.name)).unwrap_or(Value::Null),
                "fontHeight": js::num(field.font_height / 20.0), "color": color_json(field.color),
                "align": js::num(field.align), "wordWrap": field.word_wrap, "multiline": field.multiline,
                "leading": js::num(field.leading / 20.0), "leftMargin": js::num(field.left_margin / 20.0),
                "rightMargin": js::num(field.right_margin / 20.0), "indent": js::num(field.indent / 20.0),
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
    let counts = (dictionary.sprites.len(), dictionary.fonts.len(), dictionary.fields.len());
    Ok(Timeline {
        width,
        height,
        rate,
        frames,
        labels,
        stop_frames,
        effects,
        ignored_actions: evidence,
        fields,
        named_children: named,
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
    if envelope.len() < 20 || be32(0) != envelope.len() || be32(4) != 1 || be32(8) != envelope.len() - 12 {
        return Err("unsupported vectorShape XMED envelope".into());
    }
    let scene = timeline(&envelope[12..])?;
    if scene.frames.len() != 1 {
        return Err("vectorShape must have one SWF frame".into());
    }
    if scene.counts != (0, 0, 0) || !scene.ignored_actions.is_empty() || !scene.stop_frames.is_empty()
        || scene.effects.filters != 0 || scene.effects.blend_modes != 0 || scene.effects.clip_actions != 0
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
        let word = ((rgba[i * 4] >> 3) as u16) << 11 | ((rgba[i * 4 + 1] >> 3) as u16) << 6 | ((rgba[i * 4 + 2] >> 3) as u16) << 1 | 1;
        pixels[i * 2..i * 2 + 2].copy_from_slice(&word.to_be_bytes());
    }
    Ok(VectorShape { width, height, reg_x: (width / 2) as i64, reg_y: (height / 2) as i64, pixels })
}

pub struct FlashFrames {
    pub width: usize,
    pub height: usize,
    pub reg_x: i64,
    pub reg_y: i64,
    pub rate: f64,
    pub frames: Vec<Vec<u8>>,
    pub labels: Vec<Value>,
    pub stop_frames: Vec<Value>,
    pub ignored_actions: Vec<Value>,
    pub fields: Vec<Value>,
    pub named_children: Vec<Value>,
    pub effects: Effects,
}

pub fn flash_frames(swf: &[u8]) -> R<FlashFrames> {
    let scene = timeline(swf)?;
    Ok(FlashFrames {
        width: scene.width,
        height: scene.height,
        reg_x: (scene.width / 2) as i64,
        reg_y: (scene.height / 2) as i64,
        rate: scene.rate,
        frames: scene.frames,
        labels: scene.labels,
        stop_frames: scene.stop_frames,
        ignored_actions: scene.ignored_actions,
        fields: scene.fields,
        named_children: scene.named_children,
        effects: scene.effects,
    })
}

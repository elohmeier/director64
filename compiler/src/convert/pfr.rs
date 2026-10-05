//! PFR1 fonts, the portable font resources Director 8.5+ embeds in font
//! cast members. The container follows Bitstream's published PFR layout
//! (header, logical and physical font records); the character table and the
//! glyph program strings use PFR1's compact encodings, which this module
//! implements from the behaviour of the original Bitstream TrueDoc player
//! linked into Director's Font Xtra. Correctness is anchored by emulating
//! that player (tools/fonts/pfr1-verify.py), not by any other decoder.
//!
//! Output is the outline model the OpenType writer (cff.rs) consumes:
//! contours of moveto/lineto/curveto commands in outline units, left open
//! (a contour implicitly closes back to its start).

use std::fmt::Write as _;

pub type R<T> = Result<T, String>;

#[derive(Clone, Debug, PartialEq)]
pub enum Command {
    Move(f64, f64),
    Line(f64, f64),
    /// End point, then the two control points.
    Curve(f64, f64, f64, f64, f64, f64),
}

#[derive(Clone, Debug)]
pub struct Glyph {
    pub codepoint: u32,
    pub advance: i64,
    pub source_offset: usize,
    pub source_length: usize,
    pub contours: Vec<Vec<Command>>,
}

#[derive(Clone, Debug)]
pub struct Font {
    pub name: String,
    pub units_per_em: i64,
    pub metrics_resolution: i64,
    pub bbox: [i64; 4],
    pub matrix: [i64; 4],
    pub glyphs: Vec<Glyph>,
}

impl Font {
    pub fn ascender(&self) -> i64 {
        self.bbox[3]
    }
    pub fn descender(&self) -> i64 {
        self.bbox[1]
    }
}

struct Bytes<'a> {
    b: &'a [u8],
    p: usize,
}

impl<'a> Bytes<'a> {
    fn at(b: &'a [u8], p: usize) -> Self {
        Self { b, p }
    }
    fn uint(&mut self, n: usize) -> R<u64> {
        let s = self.b.get(self.p..self.p + n).ok_or("truncated PFR")?;
        self.p += n;
        Ok(s.iter().fold(0u64, |v, &x| v << 8 | x as u64))
    }
    fn int(&mut self, n: usize) -> R<i64> {
        let v = self.uint(n)? as i64;
        let bits = 8 * n as u32;
        Ok(if v >= 1 << (bits - 1) { v - (1 << bits) } else { v })
    }
    fn u8(&mut self) -> R<u8> {
        Ok(self.uint(1)? as u8)
    }
    fn skip(&mut self, n: usize) -> R<()> {
        if self.p + n > self.b.len() {
            return Err("truncated PFR".into());
        }
        self.p += n;
        Ok(())
    }
}

/// Parses a PFR1 font resource.
pub fn parse(b: &[u8]) -> R<Font> {
    if b.len() < 58 || &b[..4] != b"PFR1" || &b[6..8] != b"\r\n" {
        return Err("Expected original PFR1 XMED".into());
    }
    let mut r = Bytes::at(b, 12);
    let directory = r.uint(2)? as usize;
    let gps_section = {
        let mut h = Bytes::at(b, 35);
        h.uint(3)? as usize
    };
    let mut r2 = Bytes::at(b, directory);
    if r2.uint(2)? < 1 {
        return Err("PFR has no logical font".into());
    }
    r2.skip(2)?;
    let logical = r2.uint(3)? as usize;
    let high_size = b[41] != 0;
    let mut l = Bytes::at(b, logical);
    let mut matrix = [0i64; 4];
    for m in &mut matrix {
        *m = l.int(3)?;
    }
    let flags = l.u8()?;
    if flags & 0x3f != 0 {
        return Err("unsupported PFR logical font effects".into());
    }
    if flags & 0x40 != 0 {
        for _ in 0..l.u8()? {
            let size = l.u8()? as usize;
            l.skip(size + 1)?;
        }
    }
    let mut physical_size = l.uint(2)? as usize;
    let physical = l.uint(3)? as usize;
    if high_size {
        physical_size += l.u8()? as usize * 65536;
    }
    let mut p = Bytes::at(b, physical);
    p.skip(2)?;
    let units_per_em = p.uint(2)? as i64;
    let metrics_resolution = p.uint(2)? as i64;
    let mut bbox = [0i64; 4];
    for v in &mut bbox {
        *v = p.int(2)?;
    }
    let pflags = p.u8()?;
    if pflags & 0x04 == 0 {
        p.skip(2)?;
    }
    let mut name = None;
    if pflags & 0x80 != 0 {
        for _ in 0..p.u8()? {
            let size = p.u8()? as usize;
            let kind = p.u8()?;
            let data = b.get(p.p..p.p + size).ok_or("truncated PFR")?;
            match kind {
                1 => return Err("unsupported PFR outline font".into()),
                2 => {
                    let end = data.iter().position(|&c| c == 0).unwrap_or(data.len());
                    name = Some(&data[..end]);
                }
                _ => {}
            }
            p.skip(size)?;
        }
    }
    let aux = p.uint(3)? as usize;
    p.skip(aux)?;
    let blues = p.u8()? as usize;
    p.skip(blues * 2 + 6)?;
    let count = p.uint(2)? as usize;
    if count == 0 || count > 4096 {
        return Err("unsupported PFR outline font".into());
    }
    let name = name.ok_or("PFR font has no name")?;
    if name.iter().any(|&c| !(32..=126).contains(&c)) {
        return Err("non-ASCII PFR font name".into());
    }
    // PFR1 character records: a flag byte, then only the fields that do not
    // follow from the previous record.
    let (mut code, mut advance, mut offset, mut size) = (-1i64, 0i64, 0usize, 0usize);
    let mut records = Vec::with_capacity(count);
    for _ in 0..count {
        let f = p.u8()?;
        if f & 0x20 != 0 {
            return Err("unsupported PFR character record".into());
        }
        code += 1 + match f & 3 {
            1 => p.u8()? as i64,
            2 => p.uint(2)? as i64,
            3 => return Err("unsupported PFR character record".into()),
            _ => 0,
        };
        match f & 0x0c {
            0x0c => advance = p.int(2)?,
            0x04 => advance += p.u8()? as i64,
            0x08 => advance -= p.u8()? as i64,
            _ => {}
        }
        offset += size;
        size = p.u8()? as usize + if f & 0x10 != 0 { 256 } else { 0 };
        match f & 0xc0 {
            0x80 => offset = p.uint(2)? as usize,
            0x40 => offset += p.u8()? as usize,
            0xc0 => return Err("unsupported PFR character record".into()),
            _ => {}
        }
        records.push((code, advance, offset, size));
    }
    if p.p != physical + physical_size {
        return Err("PFR physical font record length disagrees".into());
    }
    let mut glyphs = Vec::with_capacity(count);
    let mut previous = -1i64;
    for (code, advance, offset, size) in records {
        if code <= previous || code > 0x10ffff {
            return Err("Invalid PFR Unicode mapping".into());
        }
        previous = code;
        let start = gps_section + offset;
        if start + size > b.len() {
            return Err("PFR glyph program exceeds the resource".into());
        }
        let contours = if size == 0 { Vec::new() } else { glyph(b, gps_section, start, size, 0)? };
        if size > 1 && contours.is_empty() {
            return Err(format!("missing PFR outline: {code}"));
        }
        glyphs.push(Glyph { codepoint: code as u32, advance, source_offset: start, source_length: size, contours });
    }
    Ok(Font {
        name: String::from_utf8(name.to_vec()).unwrap(),
        units_per_em,
        metrics_resolution,
        bbox,
        matrix,
        glyphs,
    })
}

/// A glyph program: simple, or compound of transformed simple elements.
fn glyph(b: &[u8], gps: usize, start: usize, size: usize, depth: u32) -> R<Vec<Vec<Command>>> {
    let end = start + size;
    if b[start] & 0x80 == 0 {
        return simple(b, start, end);
    }
    if depth > 0 {
        return Err("nested PFR compound glyph".into());
    }
    let mut out = Vec::new();
    for e in elements(b, start, end, start - gps)? {
        let from = gps.checked_add_signed(e.offset).filter(|&s| e.size > 0 && s + e.size <= b.len())
            .ok_or("PFR compound element outside the resource")?;
        for contour in glyph(b, gps, from, e.size, depth + 1)? {
            let x = |v: f64| round9(v * e.x_scale as f64 / 4096.0 + e.x_pos as f64);
            let y = |v: f64| round9(v * e.y_scale as f64 / 4096.0 + e.y_pos as f64);
            out.push(contour.into_iter().map(|c| match c {
                Command::Move(a, b) => Command::Move(x(a), y(b)),
                Command::Line(a, b) => Command::Line(x(a), y(b)),
                Command::Curve(a, b, c, d, e2, f) => Command::Curve(x(a), y(b), x(c), y(d), x(e2), y(f)),
            }).collect());
        }
    }
    Ok(out)
}

/// Nine significant digits: the precision transformed coordinates are kept at.
fn round9(v: f64) -> f64 {
    if v == v.trunc() {
        return v;
    }
    format!("{v:.8e}").parse().unwrap()
}

struct Element {
    x_scale: i64,
    y_scale: i64,
    x_pos: i64,
    y_pos: i64,
    offset: isize,
    size: usize,
}

/// Compound elements. One code byte per element packs the x and y
/// transform forms (base 6) and the form of the element's location.
/// Elements usually sit immediately before their compound, so the
/// location counts backwards from the compound's own offset.
fn elements(b: &[u8], start: usize, end: usize, own_offset: usize) -> R<Vec<Element>> {
    let flags = b[start];
    let mut r = Bytes::at(&b[..end], start + 1);
    if flags & 0x40 != 0 {
        for _ in 0..r.u8()? {
            let size = r.u8()? as usize;
            r.skip(size + 1)?;
        }
    }
    let mut tracker = own_offset as isize;
    let mut out = Vec::new();
    for _ in 0..flags & 0x3f {
        let code = r.u8()? as i64;
        let mut axes = [(0i64, 0i64); 2];
        for (axis, mode) in [code % 6, code / 6 % 6].into_iter().enumerate() {
            let scale = match mode {
                0..=2 => 0x1000,
                3 | 4 => r.int(2)?,
                _ => 0,
            };
            let pos = match mode {
                1 | 3 => r.int(1)?,
                2 | 4 => r.int(2)?,
                _ => 0,
            };
            axes[axis] = (scale, pos);
        }
        let (offset, size) = match code / 36 {
            form @ 0..=2 => {
                let size = match form {
                    0 => r.u8()? as usize,
                    1 => r.u8()? as usize + 256,
                    _ => r.uint(2)? as usize,
                };
                tracker -= size as isize;
                (tracker, size)
            }
            3 => {
                let v = r.uint(3)?;
                ((tracker - (v & 0x7fff) as isize), (v >> 15) as usize)
            }
            4 => {
                let v = r.uint(3)?;
                ((v & 0x7fff) as isize, (v >> 15) as usize)
            }
            5 => {
                let v = r.uint(4)?;
                ((v & 0x7f_ffff) as isize, (v >> 23) as usize)
            }
            _ => {
                let size = r.uint(2)? as usize;
                (r.uint(3)? as isize, size)
            }
        };
        // A zero scale is the player's unit scale with hinting disabled.
        let unit = |s: i64| if s == 0 { 0x1000 } else { s };
        out.push(Element {
            x_scale: unit(axes[0].0),
            y_scale: unit(axes[1].0),
            x_pos: axes[0].1,
            y_pos: axes[1].1,
            offset,
            size,
        });
    }
    if r.p != end {
        return Err("PFR compound glyph length disagrees".into());
    }
    Ok(out)
}

fn s16(v: i64) -> i64 {
    v as i16 as i64
}
fn s8(v: u8) -> i64 {
    v as i8 as i64
}

/// A nibble-addressed reader: `high` is set once a byte's high nibble has
/// been consumed and the low nibble is next.
struct Nibbles<'a> {
    b: &'a [u8],
    p: usize,
    high: bool,
}

impl Nibbles<'_> {
    fn byte_at(&self, p: usize) -> R<u8> {
        self.b.get(p).copied().ok_or_else(|| "truncated PFR glyph program".into())
    }
    fn nibble(&mut self) -> R<u8> {
        self.high = !self.high;
        let v = self.byte_at(self.p)?;
        if self.high {
            Ok(v >> 4)
        } else {
            self.p += 1;
            Ok(v & 15)
        }
    }
    /// Eight bits starting at the current nibble.
    fn byte(&mut self) -> R<u8> {
        if !self.high {
            let v = self.byte_at(self.p)?;
            self.p += 1;
            return Ok(v);
        }
        let v = (self.byte_at(self.p)? << 4) | (self.byte_at(self.p + 1)? >> 4);
        self.p += 1;
        Ok(v)
    }
    /// A signed 12-bit value, widened to 20 bits when it would fit in 8.
    fn value(&mut self) -> R<i64> {
        let mut v = (s8(self.byte()?) << 4) + self.nibble()? as i64;
        if (-128..128).contains(&v) {
            v = (v << 8) + self.byte()? as i64;
        }
        Ok(v)
    }
}

/// The controlled-coordinate tables (orus) a glyph's points may snap to.
fn controls(r: &mut Nibbles, flags: u8) -> R<(Vec<i64>, Vec<i64>)> {
    let (nx, ny) = match flags & 3 {
        0 => (0, 0),
        1 => {
            let v = r.byte()?;
            ((v & 15) as usize, (v >> 4) as usize)
        }
        _ => (r.byte()? as usize, r.byte()? as usize),
    };
    let wide = flags & 3 == 3;
    let mut tables = [Vec::with_capacity(nx), Vec::with_capacity(ny + 2)];
    let (mut format, mut left) = (0u8, 0);
    for (dim, n) in [nx, ny].into_iter().enumerate() {
        let mut previous = 0;
        for i in 0..n {
            let long = if i == 0 {
                flags >> (4 + dim) & 1 == 1
            } else if flags & 0x40 != 0 {
                if left == 0 {
                    format = r.nibble()?;
                    left = 3;
                } else {
                    format >>= 1;
                    left -= 1;
                }
                format & 1 == 1
            } else {
                false
            };
            let delta = if !long {
                r.byte()? as i64
            } else if wide {
                (r.byte()? as i64) << 8 | r.byte()? as i64
            } else {
                (s8(r.byte()?) << 4) | r.nibble()? as i64
            };
            previous = s16(previous + delta);
            tables[dim].push(previous);
        }
    }
    let [xs, mut ys] = tables;
    if flags & 4 != 0 {
        // The player counts the duplicate even without a first entry to
        // copy, leaving it undefined; no authored font does that.
        let first = *ys.first().ok_or("PFR glyph duplicates a missing control value")?;
        ys.insert(0, first);
    }
    if ys.len() % 2 == 1 {
        ys.push(*ys.last().unwrap());
    }
    if r.high {
        r.p += 1;
        r.high = false;
    }
    Ok((xs, ys))
}

/// The control value `steps` entries from `current` in `table`; with no
/// step given, in the direction the outline is travelling.
fn lookup(table: &[i64], current: i64, previous: i64, steps: i64) -> i64 {
    let steps = match steps {
        0 if current > previous => 1,
        0 if current < previous => -1,
        0 => return current,
        s => s,
    };
    let n = table.len() as i64;
    if steps < 0 {
        match (0..n).rev().find(|&i| table[i as usize] < current) {
            Some(i) => table[(i + steps + 1).max(0) as usize],
            None => current,
        }
    } else {
        match (0..n).find(|&i| table[i as usize] > current) {
            Some(i) => table[(i + steps - 1).min(n - 1) as usize],
            None => current,
        }
    }
}

// Opcode record kinds and the packed point-format words of the curve
// shorthands (three nibbles: first control, second control, end point).
const KIND: [u8; 16] = [1, 1, 1, 1, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2];
const SMOOTH: [u16; 16] =
    [0xfff, 0x3aa, 0xcaa, 0xaa3, 0xaac, 0xaaa, 0x2aa, 0x8aa, 0xaa2, 0xaa8, 0xaa, 0x555, 0x155, 0x455, 0x551, 0x554];
const HV: [u16; 16] =
    [0x451, 0x452, 0x461, 0x462, 0x491, 0x492, 0x4a1, 0x4a2, 0x851, 0x852, 0x861, 0x862, 0x891, 0x892, 0x8a1, 0x8a2];
const VH: [u16; 16] =
    [0x154, 0x158, 0x164, 0x168, 0x194, 0x198, 0x1a4, 0x1a8, 0x254, 0x258, 0x264, 0x268, 0x294, 0x298, 0x2a4, 0x2a8];
const PAIR: [u16; 8] = [1, 2, 4, 5, 6, 8, 9, 10];
const SINGLE: [u16; 4] = [5, 6, 9, 10];

struct Pen<'a> {
    xs: &'a [i64],
    ys: &'a [i64],
    x: i64,
    y: i64,
    px: i64,
    py: i64,
}

impl Pen<'_> {
    fn look(&self, dim: usize, steps: i64) -> i64 {
        if dim == 0 { lookup(self.xs, self.x, self.px, steps) } else { lookup(self.ys, self.y, self.py, steps) }
    }
    fn coordinate(&mut self, r: &mut Nibbles, dim: usize, form: u16, value: i64) -> R<i64> {
        let current = if dim == 0 { self.x } else { self.y };
        Ok(match form {
            1 => s16(current + r.nibble()? as i64 - 8),
            2 => {
                let v = s8(r.byte()?);
                if (-8..8).contains(&v) { self.look(dim, if v >= 0 { v + 1 } else { v }) } else { s16(current + v) }
            }
            3 => s16(current + r.value()?),
            _ => value,
        })
    }
    /// Reads one point in the given two-bit x/y forms; the pen follows it.
    fn point(&mut self, r: &mut Nibbles, form: u16, at: (i64, i64)) -> R<(i64, i64)> {
        let x = self.coordinate(r, 0, form & 3, at.0)?;
        (self.px, self.x) = (self.x, x);
        let y = self.coordinate(r, 1, form >> 2 & 3, at.1)?;
        (self.py, self.y) = (self.y, y);
        Ok((x, y))
    }
    fn to(&mut self, x: i64, y: i64) {
        (self.px, self.py, self.x, self.y) = (self.x, self.y, x, y);
    }
}

fn simple(b: &[u8], start: usize, end: usize) -> R<Vec<Vec<Command>>> {
    let flags = b[start];
    let data = &b[..end];
    let mut r = Nibbles { b: data, p: start + 1, high: false };
    let (xs, ys) = controls(&mut r, flags)?;
    if flags & 8 != 0 {
        let mut p = r.p;
        let count = *data.get(p).ok_or("truncated PFR glyph program")?;
        p += 1;
        for _ in 0..count {
            p += *data.get(p).ok_or("truncated PFR glyph program")? as usize + 2;
        }
        r.p = p;
    }
    let mut pen = Pen { xs: &xs, ys: &ys, x: 0, y: 0, px: 0, py: 0 };
    let mut contours: Vec<Vec<Command>> = Vec::new();
    let mut pending: Option<(i64, i64)> = None;
    let mut first = true;
    // The program runs to its last whole byte.
    while r.p + 1 < end || (r.p + 1 == end && !r.high) {
        let op = if first { 6 } else { r.nibble()? as usize };
        first = false;
        let at = (pen.x, pen.y);
        let points: Vec<(i64, i64)> = match op {
            0 => {
                let n = r.nibble()?;
                let steps = if n & 4 != 0 { (n & 7) as i64 - 8 } else { (n & 7) as i64 + 1 };
                let p = if n & 8 != 0 { (at.0, pen.look(1, steps)) } else { (pen.look(0, steps), at.1) };
                pen.to(p.0, p.1);
                vec![p]
            }
            1..=4 => {
                let v = if op < 3 { s8(r.byte()?) } else { r.value()? };
                let p = if op % 2 == 1 { (s16(at.0 + v), at.1) } else { (at.0, s16(at.1 + v)) };
                pen.to(p.0, p.1);
                vec![p]
            }
            5 | 6 => {
                let form = r.nibble()? as u16;
                vec![pen.point(&mut r, form, at)?]
            }
            _ => {
                let form = match op {
                    7 => 0x8a2,
                    8 => 0x2a8,
                    9 => HV[r.nibble()? as usize],
                    10 => VH[r.nibble()? as usize],
                    11 => {
                        let c = r.byte()? as u16;
                        (c & 3) | (c & 0x3c) << 2 | (c & 0xc0) << 4
                    }
                    12 => (r.byte()? as u16) << 2,
                    13 => SMOOTH[r.nibble()? as usize],
                    14 => {
                        let c = r.byte()? as usize;
                        (((PAIR[c >> 5 & 7] << 4) + SINGLE[c >> 3 & 3]) << 4) + PAIR[c & 7]
                    }
                    _ => {
                        let n = r.nibble()? as u16;
                        (n << 8) + r.byte()? as u16
                    }
                };
                match op {
                    // Starts horizontal: the second control shares the first's
                    // y, the end its x; defaults snap in the travel direction.
                    7 | 9 | 11 => {
                        let p1 = pen.point(&mut r, form, at)?;
                        let p2 = pen.point(&mut r, form >> 4, (pen.look(0, 0), p1.1))?;
                        let p3 = pen.point(&mut r, form >> 8, (p2.0, pen.look(1, 0)))?;
                        vec![p1, p2, p3]
                    }
                    8 | 10 | 12 => {
                        let p1 = pen.point(&mut r, form, at)?;
                        let p2 = pen.point(&mut r, form >> 4, (p1.0, pen.look(1, 0)))?;
                        let p3 = pen.point(&mut r, form >> 8, (pen.look(0, 0), p2.1))?;
                        vec![p1, p2, p3]
                    }
                    // Smooth: the first control continues the previous tangent.
                    _ => {
                        let start = (s16(at.0 + pen.x - pen.px), s16(at.1 + pen.y - pen.py));
                        let p1 = pen.point(&mut r, form, start)?;
                        let p2 = pen.point(&mut r, form >> 4, p1)?;
                        let p3 = pen.point(&mut r, form >> 8, p2)?;
                        vec![p1, p2, p3]
                    }
                }
            }
        };
        match KIND[op] {
            0 => pending = Some(points[0]),
            kind => {
                if let Some((x, y)) = pending.take() {
                    contours.push(vec![Command::Move(x as f64, y as f64)]);
                }
                let contour = contours.last_mut().ok_or("PFR contour lacks initial moveto")?;
                let f = |v: i64| v as f64;
                contour.push(if kind == 1 {
                    Command::Line(f(points[0].0), f(points[0].1))
                } else {
                    Command::Curve(f(points[2].0), f(points[2].1), f(points[0].0), f(points[0].1), f(points[1].0), f(points[1].1))
                });
            }
        }
    }
    Ok(contours)
}

/// One simple glyph program on its own, as the outline JSON writes it
/// (for differential testing against the original player).
pub fn glyph_json(program: &[u8]) -> R<String> {
    if program.is_empty() || program[0] & 0x80 != 0 {
        return Err("not a simple glyph program".into());
    }
    let mut out = String::from("[");
    for (j, contour) in simple(program, 0, program.len())?.iter().enumerate() {
        if j > 0 {
            out.push(',');
        }
        out.push('[');
        for (k, c) in contour.iter().enumerate() {
            if k > 0 {
                out.push(',');
            }
            command_json(c, &mut out);
        }
        out.push(']');
    }
    out.push(']');
    Ok(out)
}

/// A number as Python's json module writes it.
pub fn python_number(v: f64) -> String {
    if v == v.trunc() && v.abs() < 1e16 {
        return format!("{}", v as i64);
    }
    // Shortest round-trip digits, laid out like repr().
    let e = format!("{v:e}");
    let (mantissa, exponent) = e.split_once('e').unwrap();
    let exponent: i32 = exponent.parse().unwrap();
    if !(-4..16).contains(&exponent) {
        let sign = if exponent < 0 { '-' } else { '+' };
        return format!("{mantissa}e{sign}{:02}", exponent.abs());
    }
    format!("{v}")
}

fn command_json(c: &Command, out: &mut String) {
    let n = python_number;
    let _ = match *c {
        Command::Move(x, y) => write!(out, "[0,{},{}]", n(x), n(y)),
        Command::Line(x, y) => write!(out, "[1,{},{}]", n(x), n(y)),
        Command::Curve(x, y, a, b, c2, d) => {
            write!(out, "[2,{},{},{},{},{},{}]", n(x), n(y), n(a), n(b), n(c2), n(d))
        }
    };
}

/// The recovered outlines as compact JSON with sorted keys (the
/// `.outlines.json` provenance file).
pub fn outlines_json(font: &Font) -> String {
    let mut out = String::new();
    let [x0, y0, x1, y1] = font.bbox;
    let _ = write!(
        out,
        "{{\"ascender\":{},\"bbox\":[{x0},{y0},{x1},{y1}],\"descender\":{},\"glyphs\":[",
        font.ascender(),
        font.descender()
    );
    for (i, g) in font.glyphs.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        let _ = write!(out, "{{\"advance\":{},\"codepoint\":{},\"contours\":[", g.advance, g.codepoint);
        for (j, contour) in g.contours.iter().enumerate() {
            if j > 0 {
                out.push(',');
            }
            out.push('[');
            for (k, c) in contour.iter().enumerate() {
                if k > 0 {
                    out.push(',');
                }
                command_json(c, &mut out);
            }
            out.push(']');
        }
        let _ = write!(out, "],\"sourceLength\":{},\"sourceOffset\":{}}}", g.source_length, g.source_offset);
    }
    let m = font.matrix;
    let _ = write!(
        out,
        "],\"matrix\":[{},{},{},{}],\"metricsResolution\":{},\"name\":{},\"unitsPerEm\":{}}}\n",
        m[0],
        m[1],
        m[2],
        m[3],
        font.metrics_resolution,
        serde_json::Value::String(font.name.clone()),
        font.units_per_em
    );
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn decode(hex: &str) -> R<Vec<Vec<Command>>> {
        let b: Vec<u8> = (0..hex.len()).step_by(2).map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap()).collect();
        simple(&b, 0, b.len())
    }

    #[test]
    fn a_stem_from_two_control_tables() {
        // Two x and two y orus (nibble counts), then a move and three
        // lines that snap to them; the contour stays open.
        use Command::*;
        assert_eq!(
            decode("45224e25e002c220108070f0").unwrap(),
            [vec![Move(172.0, 0.0), Line(172.0, 706.0), Line(78.0, 706.0), Line(78.0, 0.0)]]
        );
    }

    #[test]
    fn lookups_step_through_the_table_and_infer_direction_from_travel() {
        let table = [0, 100, 200, 300];
        assert_eq!(lookup(&table, 150, 0, 1), 200);
        assert_eq!(lookup(&table, 150, 0, 2), 300);
        assert_eq!(lookup(&table, 150, 0, -1), 100);
        assert_eq!(lookup(&table, 150, 0, -3), 0);
        assert_eq!(lookup(&table, 150, 100, 0), 200);
        assert_eq!(lookup(&table, 150, 190, 0), 100);
        assert_eq!(lookup(&table, 150, 150, 0), 150);
        assert_eq!(lookup(&table, 400, 0, 1), 400);
    }

    #[test]
    fn values_widen_when_they_would_fit_a_byte() {
        let mut r = Nibbles { b: &[0x2e, 0x00], p: 0, high: false };
        assert_eq!(r.value().unwrap(), 0x2e0);
        let mut r = Nibbles { b: &[0x01, 0x23, 0x40], p: 0, high: false };
        assert_eq!(r.value().unwrap(), (0x12 << 8) + 0x34);
    }

    #[test]
    fn an_undefined_duplicated_control_value_fails_closed() {
        assert!(decode("4432e025f0d46c562010830d2083dfb0f000f0").unwrap_err().contains("missing control value"));
    }

    #[test]
    fn compound_elements_count_back_from_the_compound() {
        // Two unscaled elements of 19 and 41 bytes stored just before it.
        let b = [0x82, 0x00, 0x13, 0x00, 0x29];
        let e = elements(&b, 0, 5, 84).unwrap();
        assert_eq!((e[0].offset, e[0].size, e[1].offset, e[1].size), (65, 19, 24, 41));
        assert_eq!((e[0].x_scale, e[0].y_scale, e[0].x_pos), (0x1000, 0x1000, 0));
        // Scaled and shifted, with an explicit back-delta and size.
        let b = [0x81, 0x81, 0x0f, 0x1b, 0xc1, 0x0e, 0xb8, 0x00, 0x05, 0x83, 0x92];
        let e = elements(&b, 0, 11, 1000).unwrap();
        assert_eq!((e[0].x_scale, e[0].x_pos, e[0].y_scale, e[0].y_pos), (0x0f1b, -63, 0x0eb8, 0));
        assert_eq!((e[0].offset, e[0].size), (1000 - 0x392, 11));
    }

    #[test]
    fn numbers_print_as_python_json() {
        assert_eq!(python_number(405.0), "405");
        assert_eq!(python_number(319.357178), "319.357178");
        assert_eq!(python_number(-0.0), "0");
        assert_eq!(python_number(1e-5), "1e-05");
    }
}

//! DefineMorphShape (46), from Adobe SWF File Format Specification v10,
//! "Shape morphing". Pair original edges before flattening: a straight
//! edge may morph into a curve, whose control point starts at its midpoint.
use super::*;

#[derive(Clone)]
struct Curve {
    points: [f64; 6], // start, quadratic control, end
    curved: bool,
    styles: [usize; 3],
}

pub(super) struct Morph {
    fills: Vec<(Fill, Fill)>,
    lines: Vec<(Line, Line)>,
    edges: Vec<(Curve, Curve)>,
}

fn count(b: &[u8], p: &mut usize) -> R<usize> {
    let n = byte(b, *p)?;
    *p += 1;
    if n == 255 {
        let n = u16le(b, *p)?;
        *p += 2;
        Ok(n)
    } else {
        Ok(n as usize)
    }
}

fn matrices(b: &[u8], p: &mut usize) -> R<(Matrix, Matrix)> {
    let mut bits = Bits::new(b, *p);
    let pair = (bits.matrix()?, bits.matrix()?);
    *p = bits.align();
    Ok(pair)
}

fn curves(b: &[u8], styles_allowed: bool, fills: usize, lines: usize) -> R<Vec<Curve>> {
    let mut bits = Bits::new(b, 0);
    let fill_bits = bits.unsigned(4)? as usize;
    let line_bits = bits.unsigned(4)? as usize;
    let (mut x, mut y) = (0.0, 0.0);
    let mut styles = [0; 3];
    let mut edges = Vec::new();
    loop {
        if bits.unsigned(1)? == 0.0 {
            let flags = bits.unsigned(5)? as u8;
            if flags == 0 {
                break;
            }
            if flags & 16 != 0 || (!styles_allowed && flags & !1 != 0) {
                return Err("invalid SWF morph style change".into());
            }
            if flags & 1 != 0 {
                let n = bits.unsigned(5)? as usize;
                x = bits.signed(n)?;
                y = bits.signed(n)?;
            }
            for (i, flag) in [2, 4, 8].into_iter().enumerate() {
                if flags & flag != 0 {
                    styles[i] = bits.unsigned(if i == 2 { line_bits } else { fill_bits })? as usize;
                    if styles[i] > if i == 2 { lines } else { fills } {
                        return Err("SWF morph references absent style".into());
                    }
                }
            }
            continue;
        }
        let curved = bits.unsigned(1)? == 0.0;
        let n = bits.unsigned(4)? as usize + 2;
        let (cx, cy, ex, ey) = if curved {
            let (cx, cy) = (x + bits.signed(n)?, y + bits.signed(n)?);
            (cx, cy, cx + bits.signed(n)?, cy + bits.signed(n)?)
        } else {
            let (dx, dy) = if bits.unsigned(1)? != 0.0 {
                (bits.signed(n)?, bits.signed(n)?)
            } else if bits.unsigned(1)? != 0.0 {
                (0.0, bits.signed(n)?)
            } else {
                (bits.signed(n)?, 0.0)
            };
            (x + dx / 2.0, y + dy / 2.0, x + dx, y + dy)
        };
        edges.push(Curve {
            points: [x, y, cx, cy, ex, ey],
            curved,
            styles,
        });
        if edges.len() > 32768 {
            return Err("SWF morph edge count exceeds conversion bounds".into());
        }
        (x, y) = (ex, ey);
    }
    if bits.align() != b.len() {
        return Err("SWF morph edge stream has trailing data".into());
    }
    Ok(edges)
}

pub(super) fn parse(b: &[u8], bitmaps: &HashMap<usize, std::rc::Rc<Bitmap>>) -> R<(usize, Morph)> {
    let id = u16le(b, 0)?;
    // Some Flash authoring output stores empty RECTs in fixed 18-byte
    // slots. Recover only this fully empty structure: both rectangles
    // are zero, the offset names four bytes of empty style/start data,
    // and both edge streams are empty. Any nonzero content uses the
    // standard parser and its bounds checks.
    if b.len() == 48
        && b[2] == 8
        && b[20] == 8
        && b[3..20].iter().chain(&b[21..38]).all(|&v| v == 0)
        && u32le(b, 38)? == 4
        && b[42..].iter().all(|&v| v == 0)
    {
        return Ok((
            id,
            Morph {
                fills: Vec::new(),
                lines: Vec::new(),
                edges: Vec::new(),
            },
        ));
    }
    let mut bits = Bits::new(b, 2);
    bits.rect()?;
    bits.rect()?;
    let mut p = bits.align();
    let end = p
        .checked_add(4)
        .and_then(|p| p.checked_add(u32le(b, p - 4).ok()?))
        .filter(|&end| end < b.len())
        .ok_or("invalid SWF morph edge offset")?;
    p += 4;
    let mut fills = Vec::new();
    for _ in 0..count(b, &mut p)? {
        let kind = byte(b, p)?;
        p += 1;
        fills.push(match kind {
            0 => {
                let pair = (
                    Fill::Color(color(b, p, true)?),
                    Fill::Color(color(b, p + 4, true)?),
                );
                p += 8;
                pair
            }
            0x10 | 0x12 => {
                let (start, end) = matrices(b, &mut p)?;
                let n = byte(b, p)? as usize;
                p += 1;
                if n == 0 || n > 15 {
                    return Err("invalid SWF morph gradient count".into());
                }
                let (mut a, mut z) = (Vec::new(), Vec::new());
                for _ in 0..n {
                    a.push((byte(b, p)? as f64, color(b, p + 1, true)?));
                    z.push((byte(b, p + 5)? as f64, color(b, p + 6, true)?));
                    p += 10;
                }
                (
                    Fill::Gradient(Gradient {
                        matrix: start,
                        stops: a,
                        radial: kind == 0x12,
                    }),
                    Fill::Gradient(Gradient {
                        matrix: end,
                        stops: z,
                        radial: kind == 0x12,
                    }),
                )
            }
            0x40..=0x43 => {
                let id = u16le(b, p)?;
                p += 2;
                let bitmap = match bitmaps.get(&id) {
                    Some(bitmap) => Some(bitmap.clone()),
                    None if id == 0xffff => None,
                    None => return Err("SWF morph references absent bitmap".into()),
                };
                let (start, end) = matrices(b, &mut p)?;
                (
                    Fill::Bitmap(BitmapFill {
                        bitmap: bitmap.clone(),
                        matrix: start,
                        clipped: kind & 1 != 0,
                    }),
                    Fill::Bitmap(BitmapFill {
                        bitmap,
                        matrix: end,
                        clipped: kind & 1 != 0,
                    }),
                )
            }
            _ => return Err(format!("unsupported SWF morph fill {kind:#x}")),
        });
    }
    let mut lines = Vec::new();
    for _ in 0..count(b, &mut p)? {
        lines.push((
            Line {
                width: u16le(b, p)? as f64,
                color: color(b, p + 4, true)?,
            },
            Line {
                width: u16le(b, p + 2)? as f64,
                color: color(b, p + 8, true)?,
            },
        ));
        p += 12;
    }
    let start = curves(
        b.get(p..end).ok_or("SWF morph styles exceed edge offset")?,
        true,
        fills.len(),
        lines.len(),
    )?;
    let finish = curves(&b[end..], false, fills.len(), lines.len())?;
    if start.len() != finish.len() {
        return Err("SWF morph edge counts disagree".into());
    }
    Ok((
        id,
        Morph {
            fills,
            lines,
            edges: start.into_iter().zip(finish).collect(),
        },
    ))
}

impl Morph {
    pub(super) fn at(&self, ratio: u16) -> R<Shape> {
        let t = ratio as f64 / 65535.0;
        let mix = |a: f64, b: f64| a + (b - a) * t;
        let color = |a: &[f64; 4], b: &[f64; 4]| [0, 1, 2, 3].map(|i| js::round(mix(a[i], b[i])));
        let matrix = |a: &Matrix, b: &Matrix| Matrix {
            a: mix(a.a, b.a),
            b: mix(a.b, b.b),
            c: mix(a.c, b.c),
            d: mix(a.d, b.d),
            tx: mix(a.tx, b.tx),
            ty: mix(a.ty, b.ty),
        };
        let fills = self
            .fills
            .iter()
            .map(|(a, b)| match (a, b) {
                (Fill::Color(a), Fill::Color(b)) => Fill::Color(color(a, b)),
                (Fill::Gradient(a), Fill::Gradient(b)) => Fill::Gradient(Gradient {
                    matrix: matrix(&a.matrix, &b.matrix),
                    radial: a.radial,
                    stops: a
                        .stops
                        .iter()
                        .zip(&b.stops)
                        .map(|(a, b)| (mix(a.0, b.0), color(&a.1, &b.1)))
                        .collect(),
                }),
                (Fill::Bitmap(a), Fill::Bitmap(b)) => Fill::Bitmap(BitmapFill {
                    bitmap: a.bitmap.clone(),
                    matrix: matrix(&a.matrix, &b.matrix),
                    clipped: a.clipped,
                }),
                _ => unreachable!("morph styles are parsed as matching pairs"),
            })
            .collect();
        let lines = self
            .lines
            .iter()
            .map(|(a, b)| Line {
                width: mix(a.width, b.width),
                color: color(&a.color, &b.color),
            })
            .collect();
        let mut collector = Collector::new();
        for (a, b) in &self.edges {
            let p = [0, 1, 2, 3, 4, 5].map(|i| mix(a.points[i], b.points[i]));
            collector.x = p[0];
            collector.y = p[1];
            [collector.fill0, collector.fill1, collector.line] = a.styles;
            if a.curved || b.curved {
                collector.curve(p[2] - p[0], p[3] - p[1], p[4] - p[2], p[5] - p[3])?;
            } else {
                collector.push(p[4], p[5])?;
            }
        }
        Ok(Shape {
            fills,
            lines,
            edges: collector.edges,
        })
    }
}

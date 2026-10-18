//! Exact font outline diagnostics. Keep original quadratic controls rather
//! than the static rasterizer's line tessellation, for source-font matching.
use super::*;
fn glyph(b: &[u8], start: usize, end: usize) -> R<Vec<Value>> {
    let mut bits = Bits::new(b, start);
    let fb = bits.unsigned(4)? as usize;
    let lb = bits.unsigned(4)? as usize;
    let (mut x, mut y) = (0.0, 0.0);
    let mut commands = Vec::new();
    loop {
        if commands.len() > 65536 {
            return Err("SWF font glyph exceeds outline bounds".into());
        }
        if bits.unsigned(1)? != 0.0 {
            let straight = bits.unsigned(1)? != 0.0;
            let n = bits.unsigned(4)? as usize + 2;
            if straight {
                if bits.unsigned(1)? != 0.0 {
                    x += bits.signed(n)?;
                    y += bits.signed(n)?;
                } else if bits.unsigned(1)? != 0.0 {
                    y += bits.signed(n)?;
                } else {
                    x += bits.signed(n)?;
                }
                commands.push(json!(["L", x, y]));
            } else {
                let cx = x + bits.signed(n)?;
                let cy = y + bits.signed(n)?;
                x = cx + bits.signed(n)?;
                y = cy + bits.signed(n)?;
                commands.push(json!(["Q", cx, cy, x, y]));
            }
        } else {
            let flags = bits.unsigned(5)? as u8;
            if flags == 0 {
                break;
            }
            if flags & 16 != 0 {
                return Err("SWF glyph has a new style table".into());
            }
            if flags & 1 != 0 {
                let n = bits.unsigned(5)? as usize;
                x = bits.signed(n)?;
                y = bits.signed(n)?;
                commands.push(json!(["M", x, y]));
            }
            if flags & 2 != 0 {
                bits.unsigned(fb)?;
            }
            if flags & 4 != 0 {
                bits.unsigned(fb)?;
            }
            if flags & 8 != 0 {
                bits.unsigned(lb)?;
            }
        }
        if bits.bit.div_ceil(8) > end {
            return Err("SWF glyph outline exceeds its extent".into());
        }
    }
    if bits.align() > end {
        return Err("SWF glyph outline exceeds its extent".into());
    }
    Ok(commands)
}
pub(super) fn outlines(swf: &[u8]) -> R<Vec<Value>> {
    if swf.len() < 8 || &swf[..3] != b"FWS" {
        return Err("unsupported SWF font envelope".into());
    }
    let mut bits = Bits::new(swf, 8);
    bits.rect()?;
    let (tags, _) = tag_stream(swf, bits.align() + 4)?;
    let mut result = Vec::new();
    for tag in tags.iter().filter(|t| matches!(t.tag, 48 | 75)) {
        let b = tag.payload;
        let (id, font) = parse_font(b, tag.tag)?;
        let count = font.glyphs.len();
        let table = 7 + byte(b, 4)? as usize;
        let wide = byte(b, 2)? & 8 != 0;
        let mut offsets = Vec::new();
        for n in 0..=count {
            offsets.push(
                table
                    + if wide {
                        u32le(b, table + n * 4)?
                    } else {
                        u16le(b, table + n * 2)?
                    },
            );
        }
        let mut glyphs = Vec::new();
        for n in 0..count {
            glyphs.push(json!({"codepoint":font.codes.as_ref().unwrap()[n],
                "advance":font.advances.as_ref().map(|a|a[n]),"commands":glyph(b,offsets[n],offsets[n+1])?}));
        }
        result.push(
            json!({"id":id,"name":font.name,"unitsPerEm":font.em,"ascent":font.ascent,
            "yAxis":"down","hasLayout":byte(b,2)?&128!=0,"glyphs":glyphs}),
        );
    }
    Ok(result)
}

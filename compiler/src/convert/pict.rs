//! Indexed PackBits PICT2 cast pictures. This bounded raster subset is
//! corroborated by the four Christmas PICT resources. Format reference:
//! ScummVM 41ac2b31847622d0662d22c03fe6979e3b43cfbc, image/pict.cpp and
//! Apple's QuickDraw picture opcodes. No reference implementation is copied.

use super::bitmap::{Meta, bitmap_pixels};

#[derive(Debug)]
pub struct Picture {
    pub width: usize,
    pub height: usize,
    pub pixels: Vec<u8>,
}

struct Reader<'a> {
    bytes: &'a [u8],
    at: usize,
}
impl Reader<'_> {
    fn take(&mut self, count: usize) -> Result<&[u8], String> {
        let bytes = self
            .bytes
            .get(self.at..self.at + count)
            .ok_or("truncated PICT data")?;
        self.at += count;
        Ok(bytes)
    }
    fn word(&mut self) -> Result<u16, String> {
        Ok(u16::from_be_bytes(self.take(2)?.try_into().unwrap()))
    }
    fn rect(&mut self) -> Result<[i16; 4], String> {
        Ok([
            self.word()? as i16,
            self.word()? as i16,
            self.word()? as i16,
            self.word()? as i16,
        ])
    }
}

pub fn decode(bytes: &[u8]) -> Result<Picture, String> {
    let mut r = Reader { bytes, at: 2 }; // Historical picSize is only sixteen bits.
    let bounds = r.rect()?;
    if bounds[0] != 0 || bounds[1] != 0 || bounds[2] <= 0 || bounds[3] <= 0 {
        return Err("unsupported PICT bounds".into());
    }
    let (width, height) = (bounds[3] as usize, bounds[2] as usize);
    if width * height > 16_777_216 || r.take(6)? != [0, 17, 2, 255, 12, 0] {
        return Err("unsupported PICT version or extent".into());
    }
    r.take(24)?; // Extended version2 header: resolution and bounds.
    let mut pixels = None;
    loop {
        if r.at % 2 != 0 {
            r.take(1)?;
        }
        let opcode = r.word()?;
        match opcode {
            0 | 0x1e => {}
            0x01 => {
                if r.word()? != 10 || r.rect()? != bounds {
                    return Err("unsupported PICT clipping region".into());
                }
            }
            0xa0 => {
                r.take(2)?;
            }
            0xa1 => {
                r.take(2)?;
                let length = r.word()? as usize;
                r.take(length)?;
            }
            0xff => {
                if r.at != bytes.len() {
                    return Err("trailing PICT data".into());
                }
                return Ok(Picture {
                    width,
                    height,
                    pixels: pixels.ok_or("PICT without raster")?,
                });
            }
            0x98 if pixels.is_none() => {
                let row_bytes = r.word()?;
                let stride = (row_bytes & 0x7fff) as usize;
                if row_bytes & 0x8000 == 0
                    || r.rect()? != bounds
                    || stride < width
                    || stride > width + 3
                {
                    return Err("unsupported PICT raster layout".into());
                }
                let version = r.word()?;
                let packing = r.word()?;
                r.take(12)?; // Packed byte count and horizontal/vertical resolution.
                let (pixel_type, depth, components, component_bits) =
                    (r.word()?, r.word()?, r.word()?, r.word()?);
                r.take(12)?; // Plane bytes, table pointer and reserved.
                if version != 0
                    || packing != 0
                    || (pixel_type, depth, components, component_bits) != (0, 8, 1, 8)
                {
                    return Err("unsupported PICT indexed pixels".into());
                }
                r.take(4)?; // Color-table seed.
                let flags = r.word()?;
                let colors = r.word()? as usize + 1;
                if colors > 256 {
                    return Err("oversized PICT palette".into());
                }
                let mut palette = vec![0; colors * 6];
                let mut seen = vec![false; colors];
                for ordinal in 0..colors {
                    let entry = r.word()? as usize;
                    let index = if flags & 0x8000 != 0 { ordinal } else { entry };
                    if index >= colors || seen[index] {
                        return Err("invalid PICT palette entry".into());
                    }
                    seen[index] = true;
                    palette[index * 6..index * 6 + 6].copy_from_slice(r.take(6)?);
                }
                if r.rect()? != bounds || r.rect()? != bounds || r.word()? != 0 {
                    return Err("unsupported PICT raster transfer".into());
                }
                let mut unpacked = Vec::with_capacity(stride * height);
                for _ in 0..height {
                    if stride < 8 {
                        unpacked.extend_from_slice(r.take(stride)?);
                        continue;
                    }
                    let length = if stride > 250 {
                        r.word()? as usize
                    } else {
                        r.take(1)?[0] as usize
                    };
                    let mut row = Reader {
                        bytes: r.take(length)?,
                        at: 0,
                    };
                    let start = unpacked.len();
                    while row.at < row.bytes.len() {
                        let control = row.take(1)?[0];
                        match control {
                            0..=127 => unpacked.extend_from_slice(row.take(control as usize + 1)?),
                            128 => {}
                            _ => unpacked.extend(std::iter::repeat_n(
                                row.take(1)?[0],
                                257 - control as usize,
                            )),
                        }
                        if unpacked.len() - start > stride {
                            return Err("PICT run exceeds row".into());
                        }
                    }
                    if unpacked.len() - start != stride {
                        return Err("short PICT row".into());
                    }
                }
                let meta = Meta {
                    width: width as i64,
                    height: height as i64,
                    depth: 8,
                    pitch: stride as i64,
                    palette: 0,
                    palette_cast: 0,
                    reg_x: 0,
                    reg_y: 0,
                    use_alpha: false,
                    alpha_threshold: 0,
                    update_flags: 0,
                };
                pixels = Some(bitmap_pixels(&meta, &unpacked, Some(&palette), true)?);
            }
            _ => return Err(format!("unsupported PICT opcode {opcode:#06x}")),
        }
    }
}

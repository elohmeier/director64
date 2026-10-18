//! Bitmaps, palettes and the score model: tools/director/director-bitmap.mjs
//! and director-model.mjs, ported. Layout reference: ScummVM
//! 41ac2b31847622d0662d22c03fe6979e3b43cfbc, engines/director/{frame.cpp,
//! score.cpp,spriteinfo.h}; independently implemented.

use serde_json::{Map, Value, json};

use super::js::{self, latin1, unhex};

fn be16(b: &[u8], at: usize) -> Result<u16, String> {
    b.get(at..at + 2)
        .map(|s| u16::from_be_bytes([s[0], s[1]]))
        .ok_or_else(|| "read outside buffer".into())
}
fn bei16(b: &[u8], at: usize) -> Result<i64, String> {
    Ok(be16(b, at)? as i16 as i64)
}
fn be32(b: &[u8], at: usize) -> Result<u32, String> {
    b.get(at..at + 4)
        .map(|s| u32::from_be_bytes(s.try_into().unwrap()))
        .ok_or_else(|| "read outside buffer".into())
}

pub struct Decoded {
    pub pixels: Vec<u8>,
    pub compressed: bool,
}

/// BITD PackBits, bounded.
pub fn decode_packbits(data: &[u8], required: usize) -> Result<Decoded, String> {
    if required == 0 || required > 64 * 1024 * 1024 {
        return Err("BITD decoded size is outside conversion bounds".into());
    }
    if data.len() == required {
        return Ok(Decoded {
            pixels: data.to_vec(),
            compressed: false,
        });
    }
    let mut pixels = vec![0u8; required];
    let (mut source, mut target) = (0usize, 0usize);
    while source < data.len() {
        let control = data[source];
        source += 1;
        let repeated = control & 0x80 != 0;
        let count = if repeated {
            257 - control as usize
        } else {
            control as usize + 1
        };
        let consumed = if repeated { 1 } else { count };
        if consumed > data.len() - source || count > required - target {
            return Err("BITD compressed run exceeds source or decoded bounds".into());
        }
        if repeated {
            pixels[target..target + count].fill(data[source]);
        } else {
            pixels[target..target + count].copy_from_slice(&data[source..source + count]);
        }
        source += consumed;
        target += count;
    }
    if target != required {
        return Err(format!("BITD decoded {target} bytes; expected {required}"));
    }
    Ok(Decoded {
        pixels,
        compressed: true,
    })
}

pub fn decode_palette(data: &[u8]) -> Result<Vec<[u8; 3]>, String> {
    if data.is_empty() || data.len() % 6 != 0 || data.len() > 256 * 6 {
        return Err("unsupported CLUT size".into());
    }
    Ok(data.chunks(6).map(|c| [c[0], c[2], c[4]]).collect())
}

#[derive(Clone, Debug)]
pub struct Meta {
    pub width: i64,
    pub height: i64,
    pub depth: i64,
    pub pitch: i64,
    pub palette: i64,
    pub palette_cast: i64,
    pub reg_x: i64,
    pub reg_y: i64,
    pub use_alpha: bool,
    pub alpha_threshold: i64,
    pub update_flags: i64,
}

impl Meta {
    /// The object bitmapMetadata returns, in its property order.
    pub fn to_json(&self) -> Map<String, Value> {
        let mut out = Map::new();
        for (k, v) in [
            ("width", self.width),
            ("height", self.height),
            ("depth", self.depth),
            ("pitch", self.pitch),
            ("palette", self.palette),
            ("paletteCast", self.palette_cast),
            ("regX", self.reg_x),
            ("regY", self.reg_y),
        ] {
            out.insert(k.into(), json!(v));
        }
        if self.use_alpha {
            out.insert("useAlpha".into(), json!(true));
            out.insert("alphaThreshold".into(), json!(self.alpha_threshold));
            out.insert("bitmapUpdateFlags".into(), json!(self.update_flags));
        }
        out
    }
}

pub fn bitmap_metadata(cast: &[u8], version: i64) -> Result<Meta, String> {
    if cast.len() < 12 {
        return Err("truncated CASt".into());
    }
    let start = 12 + be32(cast, 4)? as usize;
    let length = be32(cast, 8)? as usize;
    if length < 22 || start > cast.len() || length > cast.len() - start {
        return Err("invalid bitmap CASt range".into());
    }
    let b = &cast[start..start + length];
    let pitch_raw = be16(b, 0)? as i64;
    let color = if version < 600 {
        length > 22
    } else {
        pitch_raw & 0x8000 != 0
    };
    if color && length < 28 {
        return Err("truncated color bitmap metadata".into());
    }
    let top = bei16(b, 2)?;
    let left = bei16(b, 4)?;
    let width = bei16(b, 8)? - left;
    let height = bei16(b, 6)? - top;
    let depth = if color {
        if b[23] == 0 { 1 } else { b[23] as i64 }
    } else {
        1
    };
    let palette = if color { bei16(b, 26)? } else { 0 };
    if width < 0
        || height < 0
        || (width == 0) != (height == 0)
        || width > 4096
        || height > 4096
        || ![1, 2, 4, 8, 16, 24, 32].contains(&depth)
    {
        return Err(format!(
            "unsupported bitmap dimensions/depth {width}x{height}x{depth}"
        ));
    }
    let use_alpha = version >= 700 && depth == 32 && b[22] & 0x10 != 0;
    Ok(Meta {
        width,
        height,
        depth,
        pitch: pitch_raw & if version < 600 { 0xfff } else { 0x3fff },
        palette,
        palette_cast: if color { bei16(b, 24)? } else { 0 },
        reg_x: bei16(b, 20)? - left,
        reg_y: bei16(b, 18)? - top,
        use_alpha,
        alpha_threshold: if use_alpha { b[10] as i64 } else { 0 },
        update_flags: if use_alpha { b[22] as i64 } else { 0 },
    })
}

/// Original eight-bit palette slots, stripped only of row padding. VWTL
/// textures follow the active stage palette; round-tripping through RGB loses
/// those identities (including distinct entries with the same RGB color).
pub fn bitmap_indices(meta: &Meta, bitd: &[u8]) -> Result<Vec<u8>, String> {
    if meta.depth != 8 || meta.width <= 0 || meta.height <= 0 || meta.pitch < meta.width {
        return Err("indexed pattern source requires an eight-bit bitmap".into());
    }
    let (w, h, pitch) = (
        meta.width as usize,
        meta.height as usize,
        meta.pitch as usize,
    );
    let extent = pitch.checked_mul(h).ok_or("bitmap index extent overflow")?;
    let decoded = decode_packbits(bitd, extent)?;
    Ok(decoded
        .pixels
        .chunks_exact(pitch)
        .flat_map(|row| row[..w].iter().copied())
        .collect())
}

/// The complete first BITD plane of a FollowAlpha bitmap.
pub fn bitmap_source_alpha(meta: &Meta, bitd: &[u8]) -> Result<Vec<u8>, String> {
    if !meta.use_alpha || meta.depth != 32 {
        return Err("bitmap has no authored alpha channel".into());
    }
    let (w, h, pitch) = (
        meta.width as usize,
        meta.height as usize,
        meta.pitch as usize,
    );
    let decoded = decode_packbits(bitd, pitch * h)?;
    let mut alpha = vec![0u8; w * h];
    for y in 0..h {
        for x in 0..w {
            alpha[y * w + x] =
                decoded.pixels[y * pitch + if decoded.compressed { x } else { x * 4 }];
        }
    }
    Ok(alpha)
}

/// Decodes a JPEG split over table and image parts, as Flash and Shockwave
/// store them: stray SOI/EOI markers before the scan are dropped, so a
/// leading `FFD9 FFD8` header or separate tables join into one stream.
pub fn decode_jpeg(parts: &[&[u8]]) -> Result<(usize, usize, Vec<[u8; 3]>), String> {
    let mut stream = vec![0xFF, 0xD8];
    'parts: for data in parts {
        let mut p = 0;
        while p + 1 < data.len() {
            if data[p] != 0xFF {
                continue 'parts;
            }
            match data[p + 1] {
                0xD8 | 0xD9 => p += 2,
                0xFF => p += 1,
                0xDA => {
                    stream.extend_from_slice(&data[p..]);
                    break 'parts;
                }
                _ => {
                    let len = u16::from_be_bytes([
                        data[p + 2],
                        *data.get(p + 3).ok_or("truncated JPEG segment")?,
                    ]) as usize;
                    let end = (p + 2 + len).min(data.len());
                    stream.extend_from_slice(&data[p..end]);
                    p = end;
                }
            }
        }
    }
    let mut decoder = jpeg_decoder::Decoder::new(stream.as_slice());
    let pixels = decoder.decode().map_err(|e| e.to_string())?;
    let info = decoder.info().ok_or("JPEG without a frame")?;
    let (w, h) = (info.width as usize, info.height as usize);
    let rgb = (0..w * h)
        .map(|i| match info.pixel_format {
            jpeg_decoder::PixelFormat::L8 => [pixels[i]; 3],
            jpeg_decoder::PixelFormat::L16 => [pixels[i * 2]; 3],
            jpeg_decoder::PixelFormat::RGB24 => {
                [pixels[i * 3], pixels[i * 3 + 1], pixels[i * 3 + 2]]
            }
            jpeg_decoder::PixelFormat::CMYK32 => {
                let k = 255 - pixels[i * 4 + 3] as u32;
                let c = |v: u8| ((255 - v as u32) * k / 255) as u8;
                [c(pixels[i * 4]), c(pixels[i * 4 + 1]), c(pixels[i * 4 + 2])]
            }
        })
        .collect();
    Ok((w, h, rgb))
}

/// A Shockwave bitmap stored as JPEG (`ediM`) beside an optional packed
/// alpha plane (`ALFA`): the member's 32-bit metadata and an uncompressed
/// ARGB BITD the ordinary path decodes.
pub fn jpeg_bitd(meta: &Meta, jpeg: &[u8], alfa: Option<&[u8]>) -> Result<(Meta, Vec<u8>), String> {
    // Imported PNGs keep their file in ediM too.
    let png = jpeg.starts_with(b"\x89PNG\r\n\x1a\n");
    let (w, h, rgba) = if png {
        decode_png(jpeg).map_err(|e| format!("ediM PNG: {e}"))?
    } else {
        let (w, h, rgb) = decode_jpeg(&[jpeg]).map_err(|e| format!("ediM JPEG: {e}"))?;
        (
            w,
            h,
            rgb.into_iter().map(|[r, g, b]| [r, g, b, 255]).collect(),
        )
    };
    // The image's own size wins: some imported PNGs keep a stale member rect.
    if w > 4096 || h > 4096 {
        return Err("unsupported ediM image dimensions".into());
    }
    let alpha = match alfa {
        Some(plane) => Some(decode_packbits(plane, ((w + 1) & !1) * h)?.pixels),
        None => None,
    };
    let mut out = vec![0u8; w * h * 4];
    for y in 0..h {
        for x in 0..w {
            let i = y * w + x;
            let [r, g, b, a] = rgba[i];
            out[i * 4] = alpha
                .as_ref()
                .map_or(a, |plane| plane[y * ((w + 1) & !1) + x]);
            out[i * 4 + 1..i * 4 + 4].copy_from_slice(&[r, g, b]);
        }
    }
    let mut argb = meta.clone();
    argb.depth = 32;
    argb.pitch = (w * 4) as i64;
    (argb.width, argb.height) = (w as i64, h as i64);
    if png && rgba.iter().any(|p| p[3] != 255) {
        argb.use_alpha = true;
    }
    Ok((argb, out))
}

/// A non-interlaced PNG of any standard color type and depth, as RGBA.
pub fn decode_png(b: &[u8]) -> Result<(usize, usize, Vec<[u8; 4]>), String> {
    let be32 = |at: usize| -> Result<usize, String> {
        Ok(u32::from_be_bytes(
            b.get(at..at + 4)
                .ok_or("truncated PNG")?
                .try_into()
                .unwrap(),
        ) as usize)
    };
    let (mut p, mut header, mut palette, mut trns, mut idat) =
        (8, None, Vec::new(), Vec::new(), Vec::new());
    while p + 8 <= b.len() {
        let length = be32(p)?;
        let kind = &b[p + 4..p + 8];
        let body = b.get(p + 8..p + 8 + length).ok_or("truncated PNG chunk")?;
        match kind {
            b"IHDR" => {
                header = Some((
                    be32(p + 8)?,
                    be32(p + 12)?,
                    body[8] as usize,
                    body[9],
                    body[12],
                ))
            }
            b"PLTE" => palette = body.chunks(3).map(|c| [c[0], c[1], c[2]]).collect(),
            b"tRNS" => trns = body.to_vec(),
            b"IDAT" => idat.extend_from_slice(body),
            b"IEND" => break,
            _ => {}
        }
        p += 12 + length;
    }
    let (w, h, depth, color, interlace) = header.ok_or("PNG without IHDR")?;
    if interlace != 0 {
        return Err("interlaced PNG".into());
    }
    let channels = match color {
        0 | 3 => 1,
        2 => 3,
        4 => 2,
        6 => 4,
        _ => return Err(format!("PNG color type {color}")),
    };
    if w == 0 || h == 0 || w > 4096 || h > 4096 || ![1, 2, 4, 8, 16].contains(&depth) {
        return Err("unsupported PNG dimensions/depth".into());
    }
    let bits = channels * depth;
    let stride = (w * bits + 7) / 8;
    let bpp = bits.div_ceil(8);
    let raw = miniz_oxide::inflate::decompress_to_vec_zlib_with_limit(&idat, (stride + 1) * h)
        .map_err(|_| "corrupt PNG data")?;
    if raw.len() < (stride + 1) * h {
        return Err("truncated PNG data".into());
    }
    let mut rows = vec![0u8; stride * h];
    for y in 0..h {
        let filter = raw[y * (stride + 1)];
        for x in 0..stride {
            let v = raw[y * (stride + 1) + 1 + x];
            let a = if x >= bpp {
                rows[y * stride + x - bpp]
            } else {
                0
            } as i32;
            let up = if y > 0 { rows[(y - 1) * stride + x] } else { 0 } as i32;
            let c = if x >= bpp && y > 0 {
                rows[(y - 1) * stride + x - bpp]
            } else {
                0
            } as i32;
            let predicted = match filter {
                0 => 0,
                1 => a,
                2 => up,
                3 => (a + up) / 2,
                4 => {
                    let base = a + up - c;
                    let (pa, pb, pc) = ((base - a).abs(), (base - up).abs(), (base - c).abs());
                    if pa <= pb && pa <= pc {
                        a
                    } else if pb <= pc {
                        up
                    } else {
                        c
                    }
                }
                _ => return Err(format!("PNG filter {filter}")),
            };
            rows[y * stride + x] = (v as i32 + predicted) as u8;
        }
    }
    let sample = |row: &[u8], index: usize| -> u16 {
        match depth {
            16 => u16::from_be_bytes([row[index * 2], row[index * 2 + 1]]),
            8 => row[index] as u16,
            _ => {
                let per = 8 / depth;
                ((row[index / per] >> (8 - depth * (index % per + 1))) & ((1 << depth) - 1) as u8)
                    as u16
            }
        }
    };
    let scale = |v: u16| -> u8 {
        match depth {
            16 => (v >> 8) as u8,
            8 => v as u8,
            _ => (v as u32 * 255 / ((1u32 << depth) - 1)) as u8,
        }
    };
    let key = |at: usize| {
        trns.get(at..at + 2)
            .map(|k| u16::from_be_bytes([k[0], k[1]]))
    };
    let mut out = Vec::with_capacity(w * h);
    for y in 0..h {
        let row = &rows[y * stride..(y + 1) * stride];
        for x in 0..w {
            let s = |c: usize| sample(row, x * channels + c);
            out.push(match color {
                0 => {
                    let v = s(0);
                    let g = scale(v);
                    [g, g, g, if key(0) == Some(v) { 0 } else { 255 }]
                }
                2 => {
                    let (r, g, bl) = (s(0), s(1), s(2));
                    let clear = key(0) == Some(r) && key(2) == Some(g) && key(4) == Some(bl);
                    [scale(r), scale(g), scale(bl), if clear { 0 } else { 255 }]
                }
                3 => {
                    let i = s(0) as usize;
                    let [r, g, bl] = *palette.get(i).ok_or("PNG palette index out of range")?;
                    [r, g, bl, trns.get(i).copied().unwrap_or(255)]
                }
                4 => {
                    let g = scale(s(0));
                    [g, g, g, scale(s(1))]
                }
                _ => [scale(s(0)), scale(s(1)), scale(s(2)), scale(s(3))],
            });
        }
    }
    Ok((w, h, out))
}

/// D10 ALFA planes must agree with the bitmap's own alpha.
pub fn verify_alfa_plane(meta: &Meta, alpha: &[u8], alfa: &[u8]) -> Result<(), String> {
    let (w, h) = (meta.width as usize, meta.height as usize);
    let stride = (w + 1) & !1;
    let plane = decode_packbits(alfa, stride * h)?.pixels;
    for y in 0..h {
        for x in 0..w {
            if plane[y * stride + x] != alpha[y * w + x] {
                return Err("ALFA plane disagrees with bitmap alpha".into());
            }
        }
    }
    Ok(())
}

fn rgb_bytes(colors: &[u32]) -> Vec<u8> {
    colors
        .iter()
        .flat_map(|&n| {
            let (r, g, b) = ((n >> 16) as u8, (n >> 8 & 255) as u8, (n & 255) as u8);
            [r, r, g, g, b, b]
        })
        .collect()
}

/// Director's builtin palettes; operands are one greater than the runtime ID.
pub fn builtin_palette(operand: i64, depth: i64) -> Result<Vec<u8>, String> {
    if depth == 2 {
        return Ok([255u8, 163, 101, 0].iter().flat_map(|&n| [n; 6]).collect());
    }
    if operand == -100 {
        return windows_palette(depth, false);
    }
    if operand == -101 {
        return windows_palette(depth, true);
    }
    if operand == 0 && depth == 4 {
        // Original Director 8.5.1r104 image(16,1,4,#systemMac), each
        // palette index copied into a 32-bit image without stage conversion.
        // Reproduce with tools/reference/palette-mac-four.lingo. This is
        // Director's four-bit lookup, not the Macintosh OS CLUT resource.
        return Ok(rgb_bytes(&[
            0xffffff, 0xbebebe, 0xffe600, 0x808080, 0x9fd8ff, 0x7c601b, 0x009e00, 0x8ed200,
            0xde00bf, 0x8d00a3, 0xfa0039, 0xff8c32, 0x0000ea, 0x00f0db, 0x646464, 0x000000,
        ]));
    }
    if ![1, 4, 8].contains(&depth) {
        return Err("unsupported builtin palette depth".into());
    }
    let mut colors: Vec<[u8; 3]> = Vec::new();
    if operand == -2 {
        let count = 1i64 << depth;
        for i in 0..count {
            let v = js::round(255.0 * (count - 1 - i) as f64 / (count - 1) as f64) as u8;
            colors.push([v, v, v]);
        }
    } else if operand == 0 && depth == 8 {
        for r in [255u8, 204, 153, 102, 51, 0] {
            for g in [255u8, 204, 153, 102, 51, 0] {
                for b in [255u8, 204, 153, 102, 51, 0] {
                    if r != 0 || g != 0 || b != 0 {
                        colors.push([r, g, b]);
                    }
                }
            }
        }
        for channel in 0..4 {
            for value in [238u8, 221, 187, 170, 136, 119, 85, 68, 34, 17] {
                let pick = |i: usize| {
                    if channel == 3 || channel == i {
                        value
                    } else {
                        0
                    }
                };
                colors.push([pick(0), pick(1), pick(2)]);
            }
        }
        colors.push([0, 0, 0]);
    } else {
        return Err(format!("unsupported builtin palette {operand}/{depth}"));
    }
    Ok(colors
        .iter()
        .flat_map(|c| [c[0], c[0], c[1], c[1], c[2], c[2]])
        .collect())
}

/// Director's indexed Windows system colors.
pub fn windows_palette(depth: i64, modern: bool) -> Result<Vec<u8>, String> {
    if depth == 4 {
        return Ok(rgb_bytes(&[
            0xffffff,
            0x00ffff,
            0xff00ff,
            0x0000ff,
            0xffff00,
            0x00ff00,
            0xff0000,
            0x808080,
            if modern { 0xa0a0a4 } else { 0xc0c0c0 },
            0x008080,
            0x800080,
            0x000080,
            0x808000,
            0x008000,
            0x800000,
            0x000000,
        ]));
    }
    let mut colors: Vec<u32> = vec![
        0xffffff, 0x00ffff, 0xff00ff, 0x0000ff, 0xffff00, 0x00ff00, 0xff0000, 0x808080, 0xa0a0a4,
        0xfffbf0, 0x333333, 0x996600, 0x336633, 0x003399, 0xcc00ff, 0x880000, 0xffcc66, 0xff99cc,
        0xdddddd, 0xff9900,
    ];
    let steps = [255u32, 204, 153, 102, 51, 0];
    for g in [102u32, 51] {
        for b in steps {
            colors.push(0xff0000 | g << 8 | b);
        }
    }
    for b in [204u32, 153, 102, 51] {
        colors.push(0xff0000 | b);
    }
    for r in [204u32, 153, 102, 51] {
        for g in steps {
            for b in steps {
                colors.push(r << 16 | g << 8 | b);
            }
        }
    }
    for b in [204u32, 153, 102, 51] {
        colors.push(0xff00 | b);
    }
    for g in [204u32, 153, 102, 51] {
        for b in steps {
            colors.push(g << 8 | b);
        }
    }
    for b in [204u32, 153, 102, 51] {
        colors.push(b);
    }
    for shift in [16u32, 8, 0] {
        for n in [238u32, 221, 170, 136, 119, 85, 68, 34, 17] {
            colors.push(n << shift);
        }
    }
    colors.extend([
        0x222230, 0xff9999, 0xffccff, 0x99d4ff, 0x99d499, 0xffff99, 0xf0f0f0, 0xa4c8f0, 0xc0dcc0,
        0xc0c0c0, 0x00bfbf, 0xbf00bf, 0x0000bf, 0xbfbf00, 0x00bf00, 0xbf0000, 0,
    ]);
    for (i, n) in [
        (66, 0xd408ff),
        (95, 0xa16600),
        (166, 0x336e33),
        (172, 0x33333b),
        (204, 0x0033a1),
        (215, 0x900000),
    ] {
        colors[i] = n;
    }
    if modern {
        for (i, n) in [
            (246, 0xa6caf0),
            (249, 0x008080),
            (250, 0x800080),
            (251, 0x000080),
            (252, 0x808000),
            (253, 0x008000),
            (254, 0x800000),
        ] {
            colors[i] = n;
        }
    }
    if colors.len() != 256 {
        return Err("system palette construction".into());
    }
    Ok(rgb_bytes(&colors))
}

/// RGBA pixels of a bitmap; `full_alpha` yields RGBA8888, otherwise RGBA5551
/// words. Matte-white edges clear unless the bitmap has its own alpha.
pub fn bitmap_pixels(
    meta: &Meta,
    bitd: &[u8],
    palette_bytes: Option<&[u8]>,
    full_alpha: bool,
) -> Result<Vec<u8>, String> {
    let (width, height, depth, pitch) = (
        meta.width as usize,
        meta.height as usize,
        meta.depth,
        meta.pitch as usize,
    );
    if (pitch as f64) < ((width as i64 * depth) as f64 / 8.0).ceil() {
        return Err("bitmap pitch too short".into());
    }
    let palette: Option<Vec<[u8; 3]>> = if depth == 1 {
        Some(vec![[255, 255, 255], [0, 0, 0]])
    } else if depth <= 8 {
        Some(decode_palette(
            palette_bytes.ok_or("unsupported CLUT size")?,
        )?)
    } else {
        None
    };
    let decoded = decode_packbits(bitd, pitch * height)?;
    let (pixels, compressed) = (&decoded.pixels, decoded.compressed);
    let stride = if full_alpha { 4 } else { 2 };
    let mut rgba = vec![0u8; width * height * stride];
    let mut white = vec![0u8; width * height];
    for y in 0..height {
        for x in 0..width {
            let rgb: [f64; 3] = if depth <= 8 {
                let d = depth as usize;
                let byte = pixels[y * pitch + x * d / 8];
                let index = (byte >> (8 - d - (x * d % 8))) as usize & ((1 << d) - 1);
                let c = palette
                    .as_ref()
                    .unwrap()
                    .get(index)
                    .ok_or_else(|| format!("palette index {index} absent"))?;
                [c[0] as f64, c[1] as f64, c[2] as f64]
            } else if depth == 16 {
                let row = y * pitch;
                let n = if compressed {
                    pixels[row + x] as u32 * 256 + pixels[row + width + x] as u32
                } else {
                    pixels[row + x * 2] as u32 * 256 + pixels[row + x * 2 + 1] as u32
                };
                [
                    (n >> 10 & 31) as f64 * 255.0 / 31.0,
                    (n >> 5 & 31) as f64 * 255.0 / 31.0,
                    (n & 31) as f64 * 255.0 / 31.0,
                ]
            } else {
                let row = y * pitch;
                if compressed {
                    [
                        pixels[row + width + x] as f64,
                        pixels[row + width * 2 + x] as f64,
                        pixels[row + width * 3 + x] as f64,
                    ]
                } else {
                    let p = row + x * 4;
                    [
                        pixels[p + 1] as f64,
                        pixels[p + 2] as f64,
                        pixels[p + 3] as f64,
                    ]
                }
            };
            let i = y * width + x;
            white[i] = u8::from(rgb[0] == 255.0 && rgb[1] == 255.0 && rgb[2] == 255.0);
            let alpha = if meta.use_alpha {
                pixels[y * pitch + if compressed { x } else { x * 4 }]
            } else {
                255
            };
            if full_alpha {
                for c in 0..3 {
                    rgba[i * 4 + c] = js::round(rgb[c]) as u8;
                }
                rgba[i * 4 + 3] = alpha;
            } else {
                // `>>` truncates the double to an int32 first.
                let word = ((rgb[0] as i64 >> 3) << 11
                    | (rgb[1] as i64 >> 3) << 6
                    | (rgb[2] as i64 >> 3) << 1
                    | i64::from(alpha >= 128)) as u16;
                rgba[i * 2..i * 2 + 2].copy_from_slice(&word.to_be_bytes());
            }
        }
    }
    if meta.use_alpha {
        return Ok(rgba);
    }
    // Matte ink removes edge-connected white, preserving enclosed white.
    let mut queue = vec![0u32; width * height];
    let (mut head, mut tail) = (0usize, 0usize);
    let visit = |i: usize,
                 white: &mut Vec<u8>,
                 rgba: &mut Vec<u8>,
                 queue: &mut Vec<u32>,
                 tail: &mut usize| {
        if white[i] != 0 {
            white[i] = 0;
            queue[*tail] = i as u32;
            *tail += 1;
            if full_alpha {
                rgba[i * 4 + 3] = 0;
            } else {
                rgba[i * 2 + 1] &= 254;
            }
        }
    };
    for x in 0..width {
        visit(x, &mut white, &mut rgba, &mut queue, &mut tail);
        visit(
            (height - 1) * width + x,
            &mut white,
            &mut rgba,
            &mut queue,
            &mut tail,
        );
    }
    for y in 0..height {
        visit(y * width, &mut white, &mut rgba, &mut queue, &mut tail);
        visit(
            y * width + width - 1,
            &mut white,
            &mut rgba,
            &mut queue,
            &mut tail,
        );
    }
    while head < tail {
        let i = queue[head] as usize;
        head += 1;
        let (x, y) = (i % width, i / width);
        if x > 0 {
            visit(i - 1, &mut white, &mut rgba, &mut queue, &mut tail);
        }
        if x + 1 < width {
            visit(i + 1, &mut white, &mut rgba, &mut queue, &mut tail);
        }
        if y > 0 {
            visit(i - width, &mut white, &mut rgba, &mut queue, &mut tail);
        }
        if y + 1 < height {
            visit(i + width, &mut white, &mut rgba, &mut queue, &mut tail);
        }
    }
    Ok(rgba)
}

fn record_bytes(record: &Value) -> Result<Vec<u8>, String> {
    unhex(record["hex"].as_str().unwrap_or(""))
}

fn labels_model(labels: Option<&Value>) -> Result<Vec<Value>, String> {
    let mut out = Vec::new();
    for label in labels
        .and_then(|l| l["labels"].as_array())
        .into_iter()
        .flatten()
    {
        out.push(json!({"frame": label["frame"], "name": latin1(&unhex(label["text"]["hex"].as_str().unwrap_or(""))?)}));
    }
    Ok(out)
}

/// The changed-byte mask of one channel within a frame's deltas.
fn changed_mask(frame: &Value, channel: usize, size: usize) -> Result<Vec<u8>, String> {
    let mut mask = vec![0u8; size];
    for delta in frame["deltas"].as_array().into_iter().flatten() {
        let offset = delta["channel_offset"].as_u64().unwrap_or(0) as usize;
        let length = delta["payload"]["length"].as_u64().unwrap_or(0) as usize;
        let start = (channel * size).max(offset);
        let end = ((channel + 1) * size).min(offset + length);
        if end > start {
            mask[start - channel * size..end - channel * size].fill(1);
        }
    }
    Ok(mask)
}

fn byte_array(b: &[u8]) -> Value {
    Value::Array(b.iter().map(|&v| json!(v)).collect())
}

/// Normalized score frames from the structural decoder's model.
pub fn score_model(score: &Value, labels: Option<&Value>) -> Result<Value, String> {
    let version = score["fields"]["frames_version"].as_i64().unwrap_or(0);
    let record_size = score["fields"]["channel_record_size"].as_i64().unwrap_or(0);
    if version == 7 && record_size == 24 {
        return score_model_d5(score, labels);
    }
    if !(version == 11 && record_size == 24 || version == 13 && record_size == 48) {
        return Err("only verified D6/D8 score layouts are supported".into());
    }
    let records = score["records"].as_array().cloned().unwrap_or_default();
    let detail = |index: u32| -> Result<Vec<Value>, String> {
        if index == 0 {
            return Ok(Vec::new());
        }
        let record = records.get(index as usize + 1);
        let valid = record.is_some_and(|r| r["length"].as_u64().is_some_and(|l| l % 8 == 0));
        if !valid {
            return Err(format!("invalid behavior detail {index}"));
        }
        let b = record_bytes(record.unwrap())?;
        let mut result = Vec::new();
        for p in (0..b.len()).step_by(8) {
            let initializer = be32(&b, p + 4)?;
            let mut parameters = String::new();
            if initializer != 0 {
                let data = records.get(initializer as usize);
                let bytes = match data {
                    Some(d) => record_bytes(d)?,
                    None => Vec::new(),
                };
                let end = bytes.iter().position(|&v| v == 0).unwrap_or(bytes.len());
                parameters = latin1(&bytes[..end]);
            }
            let mut entry = Map::new();
            entry.insert("cast".into(), json!(be16(&b, p)?));
            entry.insert("member".into(), json!(be16(&b, p + 2)?));
            if initializer != 0 && records.get(initializer as usize).is_none() {
                entry.insert("missingInitializer".into(), json!(initializer));
            }
            if !parameters.is_empty() {
                entry.insert("parameters".into(), json!(parameters));
            }
            result.push(Value::Object(entry));
        }
        if result.len() > if version == 13 { 8 } else { 4 } {
            return Err("behavior count exceeds native cap".into());
        }
        Ok(result)
    };
    let size = record_size as usize;
    let mut frames = Vec::new();
    for frame in score["frames"].as_array().into_iter().flatten() {
        let mut channels = Vec::new();
        for channel in frame["changed_channels"].as_array().into_iter().flatten() {
            let b = unhex(channel["record_hex"].as_str().unwrap_or(""))?;
            let c = channel["index"].as_u64().unwrap_or(0) as usize;
            if b.len() != size {
                return Err("score channel record size mismatch".into());
            }
            let mask = changed_mask(frame, c, size)?;
            let index = if c >= 6 {
                be32(&b, 8)?
            } else if c == 1 {
                be32(&b, 0)?
            } else if c == 5 {
                be32(&b, 16)?
            } else {
                be32(&b, 4)?
            };
            let behaviors = if c == 0 || c >= 6 {
                detail(index)?
            } else {
                Vec::new()
            };
            channels.push(json!({"channel": c, "bytes": byte_array(&b), "changed": byte_array(&mask), "behaviors": behaviors}));
        }
        frames.push(json!({"number": frame["number"], "channels": channels}));
    }
    Ok(json!({
        "version": version, "recordSize": record_size, "frames": frames, "labels": labels_model(labels)?,
        "defaults": "zero-initialized Director frame record",
        "unknown_tail_bytes": score["fields"]["unindexed_tail_length"],
    }))
}

/// D5: a 48-byte main record and 48 sprites, normalized into six main
/// channels while preserving byte ownership.
fn score_model_d5(score: &Value, labels: Option<&Value>) -> Result<Value, String> {
    let mut frames = Vec::new();
    for frame in score["frames"].as_array().into_iter().flatten() {
        let mut channels = Vec::new();
        for channel in frame["changed_channels"].as_array().into_iter().flatten() {
            let b = unhex(channel["record_hex"].as_str().unwrap_or(""))?;
            let c = channel["index"].as_u64().unwrap_or(0) as usize;
            if b.len() != 24 || c >= 50 {
                return Err("invalid D5 score channel".into());
            }
            let changed = changed_mask(frame, c, 24)?;
            let mut emit = |index: usize,
                            mapping: &[(usize, usize, usize)],
                            script: Option<usize>,
                            tempo: bool|
             -> Result<(), String> {
                let mut bytes = [0u8; 24];
                let mut mask = [0u8; 24];
                for &(dst, src, length) in mapping {
                    bytes[dst..dst + length].copy_from_slice(&b[src..src + length]);
                    mask[dst..dst + length].copy_from_slice(&changed[src..src + length]);
                }
                let behaviors = match script {
                    Some(offset) if be16(&b, offset + 2)? != 0 => {
                        vec![json!({"cast": be16(&b, offset)?, "member": be16(&b, offset + 2)?})]
                    }
                    _ => Vec::new(),
                };
                if mask.iter().any(|&v| v != 0) {
                    let mut entry = Map::new();
                    entry.insert("channel".into(), json!(index));
                    entry.insert("bytes".into(), byte_array(&bytes));
                    entry.insert("changed".into(), byte_array(&mask));
                    entry.insert("behaviors".into(), Value::Array(behaviors));
                    if tempo {
                        entry.insert("tempoRaw".into(), json!(b[21]));
                    }
                    channels.push(Value::Object(entry));
                }
                Ok(())
            };
            if c == 0 {
                emit(0, &[(0, 0, 4)], Some(0), false)?;
                emit(1, &[(6, 21, 1)], None, true)?;
                emit(4, &[(0, 4, 4)], None, false)?;
                emit(3, &[(0, 8, 4)], None, false)?;
                emit(2, &[(0, 12, 4)], None, false)?;
            } else if c == 1 {
                emit(5, &[(0, 0, 24)], None, false)?;
            } else {
                emit(
                    c + 4,
                    &[(0, 0, 2), (2, 10, 2), (4, 2, 4), (8, 6, 4), (12, 12, 12)],
                    Some(6),
                    false,
                )?;
            }
        }
        frames.push(json!({"number": frame["number"], "channels": channels}));
    }
    Ok(json!({
        "version": 7, "recordSize": 24, "frames": frames, "labels": labels_model(labels)?,
        "defaults": "zero-initialized Director 5 frame record", "unknown_tail_bytes": 0,
    }))
}

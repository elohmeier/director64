//! The stored image format (tools/director/fdi-model.mjs): FDI1 RGBA5551,
//! FDIA RGB5551 plus an exact A8 plane, FDI2 RGBA8888; a fixed 32-byte
//! big-endian header; images past the texture limit tile-packed as
//! runtime/director/bitmap_tiles.h reads them.

use super::js;

pub const TEXTURE_LIMIT: usize = 1024;
pub const TILE: usize = 32;

pub fn tiled(width: usize, height: usize) -> bool {
    width > TEXTURE_LIMIT || height > TEXTURE_LIMIT
}
pub fn tile_pixels(width: usize, height: usize) -> usize {
    width.div_ceil(TILE) * height.div_ceil(TILE) * TILE * TILE
}
pub fn tile_index(width: usize, x: usize, y: usize) -> usize {
    ((y / TILE) * width.div_ceil(TILE) + x / TILE) * TILE * TILE + (y % TILE) * TILE + x % TILE
}
pub fn plane_pixels(width: usize, height: usize) -> usize {
    if tiled(width, height) {
        tile_pixels(width, height)
    } else {
        width * height
    }
}

#[derive(Clone, Default)]
pub struct FdiMeta {
    pub width: usize,
    pub height: usize,
    pub reg_x: i64,
    pub reg_y: i64,
    pub use_alpha: bool,
    pub alpha_threshold: Option<u8>,
    pub rgba32: bool,
    /// Original bitmap bit depth, for Director's one-bit Copy exception.
    /// Zero denotes a generated surface or legacy input with unknown depth.
    pub source_depth: u8,
    /// RTE2 text retains coverage for Background Transparent and otherwise
    /// composites against this authored background before applying ink.
    pub rich_background: Option<[u8; 3]>,
}

/// `fdiImage`: RGBA8888 pixels (row-major) to the stored format.
pub fn fdi_image(meta: &FdiMeta, rgba: &[u8]) -> Result<Vec<u8>, String> {
    if rgba.len() != meta.width * meta.height * 4 {
        return Err("FDI pixel extent".into());
    }
    let soft = rgba
        .iter()
        .enumerate()
        .any(|(i, &v)| i % 4 == 3 && v != 0 && v != 255);
    let count = meta.width * meta.height;
    let rgba32 = soft && meta.rgba32;
    let is_tiled = tiled(meta.width, meta.height);
    let stored = plane_pixels(meta.width, meta.height);
    let alpha_offset = (32 + stored * 2 + 7) & !7;
    let length = if soft {
        if rgba32 {
            32 + stored * 4
        } else {
            alpha_offset + stored
        }
    } else {
        32 + stored * 2
    };
    let mut data = vec![0u8; length];
    data[..4].copy_from_slice(if soft {
        if rgba32 { b"FDI2" } else { b"FDIA" }
    } else {
        b"FDI1"
    });
    data[4..8].copy_from_slice(&(length as u32).to_be_bytes());
    data[8..10].copy_from_slice(&(meta.width as u16).to_be_bytes());
    data[10..12].copy_from_slice(&(meta.height as u16).to_be_bytes());
    data[12..14].copy_from_slice(&(meta.reg_x as i16).to_be_bytes());
    data[14..16].copy_from_slice(&(meta.reg_y as i16).to_be_bytes());
    if meta.use_alpha || soft {
        data[16] = 1;
    }
    if data[16] != 0 {
        data[17] = meta.alpha_threshold.unwrap_or(1);
    }
    data[19] = meta.source_depth;
    if let Some(background) = meta.rich_background {
        data[16] |= 2;
        data[24..27].copy_from_slice(&background);
    }
    for i in 0..count {
        let p = i * 4;
        let slot = if is_tiled {
            tile_index(meta.width, i % meta.width, i / meta.width)
        } else {
            i
        };
        if rgba32 {
            data[32 + slot * 4..32 + slot * 4 + 4].copy_from_slice(&rgba[p..p + 4]);
        } else {
            let word = ((rgba[p] >> 3) as u16) << 11
                | ((rgba[p + 1] >> 3) as u16) << 6
                | ((rgba[p + 2] >> 3) as u16) << 1
                | u16::from(rgba[p + 3] > 0);
            data[32 + slot * 2..32 + slot * 2 + 2].copy_from_slice(&word.to_be_bytes());
            if soft {
                data[alpha_offset + slot] = rgba[p + 3];
            }
        }
    }
    if soft && !rgba32 {
        data[20..24].copy_from_slice(&(alpha_offset as u32).to_be_bytes());
    }
    Ok(data)
}

fn be16(d: &[u8], at: usize) -> usize {
    u16::from_be_bytes([d[at], d[at + 1]]) as usize
}

/// One logical pixel as RGBA8888 under an ink (fdiPixel).
pub fn fdi_pixel(data: &[u8], index: usize, ink: i64) -> [u8; 4] {
    let magic = &data[..4];
    let soft = magic == b"FDI2";
    let width = be16(data, 8);
    let slot = if tiled(width, be16(data, 10)) {
        tile_index(width, index % width, index / width)
    } else {
        index
    };
    let p = 32 + slot * if soft { 4 } else { 2 };
    let mut rgba = if magic == b"FDIC" {
        let bits = data[18];
        let colors = be16(data, 20);
        let offset = (32 + colors * 2 + 7) & !7;
        let index = if bits == 8 {
            data[offset + slot] as usize
        } else {
            let logical = if tiled(width, be16(data, 10)) {
                slot
            } else {
                (index / width) * width.div_ceil(2) * 2 + index % width
            };
            let byte = data[offset + logical / 2];
            (if logical % 2 == 0 {
                byte >> 4
            } else {
                byte & 15
            }) as usize
        };
        let word = be16(data, 32 + index * 2) as u32;
        let expand = |v: u32| ((v << 3) | (v >> 2)) as u8;
        [
            expand(word >> 11 & 31),
            expand(word >> 6 & 31),
            expand(word >> 1 & 31),
            if word & 1 != 0 { 255 } else { 0 },
        ]
    } else if soft {
        [data[p], data[p + 1], data[p + 2], data[p + 3]]
    } else {
        let v = be16(data, p) as u32;
        let expand = |c: u32| ((c << 3) | (c >> 2)) as u8;
        let alpha = if magic == b"FDIA" {
            data[u32::from_be_bytes(data[20..24].try_into().unwrap()) as usize + slot]
        } else {
            ((v & 1) * 255) as u8
        };
        [
            expand((v >> 11) & 31),
            expand((v >> 6) & 31),
            expand((v >> 1) & 31),
            alpha,
        ]
    };
    if data[16] & 2 != 0 {
        if ink >= 0 && ink != 36 {
            let alpha = rgba[3] as u32;
            for c in 0..3 {
                rgba[c] = ((rgba[c] as u32 * alpha + data[24 + c] as u32 * (255 - alpha) + 127)
                    / 255) as u8;
            }
            rgba[3] = 255;
        }
        return rgba;
    }
    if data[16] & 1 == 0 && (ink == 0 || ink == 32) {
        rgba[3] = 255;
    }
    if ink == 36 && rgba[0] == 255 && rgba[1] == 255 && rgba[2] == 255 {
        rgba[3] = 0;
    }
    rgba
}

/// Source-over into an RGBA8888 destination (fdiComposite).
pub fn fdi_composite(destination: &mut [u8], offset: usize, source: [u8; 4]) {
    let sa = source[3] as f64;
    let da = destination[offset + 3] as f64;
    if sa == 0.0 {
        return;
    }
    let alpha = sa * 255.0 + da * (255.0 - sa);
    for c in 0..3 {
        destination[offset + c] = js::round(
            (source[c] as f64 * sa * 255.0 + destination[offset + c] as f64 * da * (255.0 - sa))
                / alpha,
        ) as u8;
    }
    destination[offset + 3] = js::round(alpha / 255.0) as u8;
}

/// A flattened film-loop pose cut to the rectangle its coverage spans, its
/// registration point moved with it so it draws where the full pose did;
/// None when it would not shrink by a tenth or holds no alpha. An empty
/// pose keeps one transparent pixel.
pub fn trim_image(data: &[u8]) -> Result<Option<Vec<u8>>, String> {
    let magic = &data[..4];
    if !(magic == b"FDI1" || magic == b"FDI2" || magic == b"FDIA")
        || data[16] & 1 == 0
        || data[16] & 2 != 0
    {
        return Ok(None);
    }
    let (w, h) = (be16(data, 8), be16(data, 10));
    let (mut l, mut t, mut r, mut b) = (w, h, 0, 0);
    for y in 0..h {
        for x in 0..w {
            if fdi_pixel(data, y * w + x, -1)[3] != 0 {
                l = l.min(x);
                r = r.max(x + 1);
                t = t.min(y);
                b = b.max(y + 1);
            }
        }
    }
    if r <= l {
        (l, t, r, b) = (0, 0, 1, 1);
    }
    let (cw, ch) = (r - l, b - t);
    if cw * ch * 10 > w * h * 9 {
        return Ok(None);
    }
    let mut rgba = vec![0u8; cw * ch * 4];
    for y in 0..ch {
        for x in 0..cw {
            let pixel = fdi_pixel(data, (t + y) * w + l + x, -1);
            rgba[(y * cw + x) * 4..(y * cw + x) * 4 + 4].copy_from_slice(&pixel);
        }
    }
    let reg = |at: usize| i16::from_be_bytes([data[at], data[at + 1]]) as i64;
    Ok(Some(fdi_image(
        &FdiMeta {
            width: cw,
            height: ch,
            reg_x: reg(12) - l as i64,
            reg_y: reg(14) - t as i64,
            use_alpha: true,
            alpha_threshold: Some(data[17]),
            rgba32: magic == b"FDI2",
            source_depth: data[19],
            rich_background: (data[16] & 2 != 0).then(|| data[24..27].try_into().unwrap()),
        },
        &rgba,
    )?))
}

/// Prescales a finished image to the 640x480 stage (4/5 of 800x600);
/// returns None when the image keeps its pixels.
pub fn prescale_image(data: &[u8]) -> Result<Option<Vec<u8>>, String> {
    let magic = &data[..4];
    if !(magic == b"FDI1" || magic == b"FDI2" || magic == b"FDIA") {
        return Ok(None);
    }
    let (w, h) = (be16(data, 8), be16(data, 10));
    if w <= 16 && h <= 16 {
        return Ok(None);
    }
    let sw = ((w * 4 + 2) / 5).max(1);
    let sh = ((h * 4 + 2) / 5).max(1);
    let mut src = vec![0u8; w * h * 4];
    for i in 0..w * h {
        src[i * 4..i * 4 + 4].copy_from_slice(&fdi_pixel(data, i, -1));
    }
    let mut out = vec![0u8; sw * sh * 4];
    for y in 0..sh {
        for x in 0..sw {
            let fx = ((x as f64 + 0.5) * w as f64 / sw as f64 - 0.5)
                .max(0.0)
                .min((w - 1) as f64);
            let fy = ((y as f64 + 0.5) * h as f64 / sh as f64 - 0.5)
                .max(0.0)
                .min((h - 1) as f64);
            let (x0, y0) = (fx.floor() as usize, fy.floor() as usize);
            let (x1, y1) = ((x0 + 1).min(w - 1), (y0 + 1).min(h - 1));
            let (ax, ay) = (fx - x0 as f64, fy - y0 as f64);
            for c in 0..4 {
                let v00 = src[(y0 * w + x0) * 4 + c] as f64;
                let v10 = src[(y0 * w + x1) * 4 + c] as f64;
                let v01 = src[(y1 * w + x0) * 4 + c] as f64;
                let v11 = src[(y1 * w + x1) * 4 + c] as f64;
                out[(y * sw + x) * 4 + c] = js::round(
                    v00 * (1.0 - ax) * (1.0 - ay)
                        + v10 * ax * (1.0 - ay)
                        + v01 * (1.0 - ax) * ay
                        + v11 * ax * ay,
                ) as u8;
            }
        }
    }
    if magic == b"FDI1" {
        for i in 0..sw * sh {
            out[i * 4 + 3] = if out[i * 4 + 3] >= 128 { 255 } else { 0 };
        }
    } else {
        let (mut low, mut transparent) = (0usize, 0usize);
        for i in 0..sw * sh {
            let a = out[i * 4 + 3];
            if a < 240 {
                low += 1;
            }
            if a < 16 {
                transparent += 1;
            }
        }
        if transparent == 0 && (low as f64) <= (4.0f64).max((sw * sh) as f64 / 500.0) {
            for i in 0..sw * sh {
                out[i * 4 + 3] = 255;
            }
        }
    }
    let reg =
        |at: usize| js::round(i16::from_be_bytes([data[at], data[at + 1]]) as f64 * 0.8) as i64;
    Ok(Some(fdi_image(
        &FdiMeta {
            width: sw,
            height: sh,
            reg_x: reg(12),
            reg_y: reg(14),
            use_alpha: data[16] & 1 != 0,
            alpha_threshold: Some(data[17]),
            rgba32: magic == b"FDI2",
            source_depth: data[19],
            rich_background: (data[16] & 2 != 0).then(|| data[24..27].try_into().unwrap()),
        },
        &out,
    )?))
}

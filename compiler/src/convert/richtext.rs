//! Director 5–10 rich-text cast rendering data. Format facts corroborated
//! against the eleven recovered Workshop/Willy RTE2 resources and ScummVM
//! 41ac2b31847622d0662d22c03fe6979e3b43cfbc (rte.cpp, richtext.cpp).
//! This decoder is independently implemented; no reference code is copied.

pub struct RichText {
    pub width: usize,
    pub height: usize,
    pub background: [u8; 3],
    pub pixels: Vec<u8>,
}

pub fn decode(metadata: &[u8], image: &[u8]) -> Result<RichText, String> {
    if metadata.len() != 34 || image.len() < 6 {
        return Err("truncated rich-text metadata or bitmap".into());
    }
    let word = |bytes: &[u8], at: usize| u16::from_be_bytes([bytes[at], bytes[at + 1]]) as usize;
    let (width, height, depth) = (word(image, 0), word(image, 2), word(image, 4));
    if width == 0 || height == 0 || width * height > 16_777_216 || !(1..=4).contains(&depth) {
        return Err("unsupported rich-text bitmap bounds or coverage depth".into());
    }
    let signed = |at| i16::from_be_bytes([metadata[at], metadata[at + 1]]) as i32;
    if signed(6) - signed(2) != width as i32
        || signed(4) - signed(0) != height as i32
        || word(metadata, 18) != 0
    {
        return Err("unsupported rich-text bitmap viewport".into());
    }
    let mut color = [metadata[25], metadata[26], metadata[27]];
    let background = [metadata[28], metadata[30], metadata[32]];
    let maximum = (1u8 << depth) - 1;
    let mut pixels = vec![0; width * height * 4];
    let mut cursor = 6;
    let next = |cursor: &mut usize| -> Result<u8, String> {
        let value = *image
            .get(*cursor)
            .ok_or("truncated rich-text bitmap command")?;
        *cursor += 1;
        Ok(value)
    };
    for row in pixels.chunks_exact_mut(width * 4) {
        let mut x = 0;
        while x < width {
            let coverage = next(&mut cursor)?;
            if coverage == 31 {
                color = [next(&mut cursor)?, next(&mut cursor)?, next(&mut cursor)?];
                continue;
            }
            if coverage > maximum {
                return Err("invalid rich-text bitmap coverage".into());
            }
            let mut count = if coverage == 0 || coverage == maximum {
                next(&mut cursor)? as usize
            } else {
                1
            };
            if count == 0 && coverage == 0 {
                count = width - x;
            } else if count == 0 || count > width - x {
                return Err("invalid rich-text bitmap run".into());
            }
            for pixel in row[x * 4..(x + count) * 4].chunks_exact_mut(4) {
                pixel[..3].copy_from_slice(&color);
                pixel[3] = (coverage as u16 * 255 / maximum as u16) as u8;
            }
            x += count;
        }
    }
    if cursor != image.len() {
        return Err("trailing rich-text bitmap data".into());
    }
    Ok(RichText {
        width,
        height,
        background,
        pixels,
    })
}

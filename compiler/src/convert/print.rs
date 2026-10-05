//! Löwenzahn's print documents (games/loewenzahn-1/host/printing.py): the
//! recovered chapter text of its two books and the original print artwork,
//! which the browser player lays out for the browser's print dialog. The
//! console shows a QR link to the same documents as hosted PDFs instead.

use serde_json::{Value, json};

type R<T> = Result<T, String>;

/// The documents, in print order: book BAS then REZ, chapters 1 to 8, as
/// printing.documents lists them.
pub fn documents(model: &Value) -> R<Value> {
    let mut out = Vec::new();
    for book in ["BAS", "REZ"] {
        let movie = model["movies"]
            .as_array()
            .into_iter()
            .flatten()
            .find(|m| m["name"] == format!("{book}.DXR"))
            .ok_or(format!("{book}.DXR missing"))?;
        if movie["directorVersion"] != 500 {
            return Err("expected Director 5 print fields".into());
        }
        let field = |name: &str| -> R<String> {
            movie["members"]
                .as_array()
                .into_iter()
                .flatten()
                .find(|m| m["name"].as_str().is_some_and(|n| n.eq_ignore_ascii_case(name)))
                .and_then(|m| m["text"].as_str().map(str::to_string))
                .ok_or(format!("{book}.DXR {name} missing"))
        };
        for chapter in 1..=8u32 {
            let illustrated = book == "BAS" && matches!(chapter, 2 | 3 | 6 | 7);
            out.push(json!({
                "book": book,
                "chapter": chapter,
                "title": field(&format!("titel{chapter}"))?,
                "body": field(&format!("text{chapter}"))?,
                "logo": format!("{book}_LOGO.png"),
                "logo_width": if book == "BAS" { 122 } else { 80 },
                "illustration": if illustrated { json!(format!("BAS{chapter}.png")) } else { Value::Null },
                "illustration_x": if chapter == 6 { 6 } else { 16 },
            }));
        }
    }
    Ok(Value::Array(out))
}

/// The observed single-raster 1-bit 200-dpi PICT subset as a grayscale PNG
/// (printing.decode_pict, after ScummVM image/pict.cpp): anything else is
/// refused rather than guessed at. Returns the PNG, width and height.
pub fn pict_png(data: &[u8]) -> R<(Vec<u8>, u32, u32)> {
    let fail = || "unsupported or truncated print PICT".to_string();
    let require = |test: bool| if test { Ok(()) } else { Err(fail()) };
    let u16_at = |at: usize| -> R<u16> { data.get(at..at + 2).map(|b| u16::from_be_bytes([b[0], b[1]])).ok_or_else(fail) };
    let i16_at = |at: usize| -> R<i16> { Ok(u16_at(at)? as i16) };
    let u32_at = |at: usize| -> R<u32> { data.get(at..at + 4).map(|b| u32::from_be_bytes(b.try_into().unwrap())).ok_or_else(fail) };
    require(data.len() >= 656)?;
    require(data[522..528] == [0x00, 0x11, 0x02, 0xff, 0x0c, 0x00])?;
    let mut expected = vec![0x00, 0x1e, 0x00, 0x01, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00];
    expected.extend_from_slice(&data[544..548]);
    expected.extend_from_slice(&[0x00, 0x98]);
    require(data[552..568] == expected[..])?;
    let q = 568;
    let stride = (u16_at(q)? & 0x7fff) as usize;
    let (top, left, height, width) = (i16_at(q + 2)?, i16_at(q + 4)?, i16_at(q + 6)?, i16_at(q + 8)?);
    require(top == 0 && left == 0 && width > 0 && width <= 1254 && height > 0 && height <= 990)?;
    let (width, height) = (width as usize, height as usize);
    require((width + 7) / 8 <= stride && stride <= (width + 15) / 8)?;
    require(u16_at(q + 12)? == 0)?;
    require(u32_at(q + 18)? == 200 << 16 && u32_at(q + 22)? == 200 << 16)?;
    require((u16_at(q + 26)?, u16_at(q + 28)?, u16_at(q + 30)?, u16_at(q + 32)?) == (0, 1, 1, 1))?;
    let mut p = q + 46;
    require(u16_at(p + 6)? == 1)?;
    p += 8;
    let colors = [(0, 65535, 65535, 65535), (0, 0, 0, 0)];
    for (i, color) in colors.iter().enumerate() {
        let at = p + i * 8;
        require((u16_at(at)?, u16_at(at + 2)?, u16_at(at + 4)?, u16_at(at + 6)?) == *color)?;
    }
    p += 16;
    for at in [p, p + 8] {
        require((i16_at(at)?, i16_at(at + 2)?, i16_at(at + 4)?, i16_at(at + 6)?) == (0, 0, height as i16, width as i16))?;
    }
    require(u16_at(p + 16)? == 0)?;
    p += 18;
    let mut raw = Vec::with_capacity(height * (1 + (width + 7) / 8));
    for _ in 0..height {
        let length = *data.get(p).ok_or_else(fail)? as usize;
        p += 1;
        let packed = data.get(p..p + length).ok_or_else(fail)?;
        p += length;
        let (mut i, mut row) = (0, Vec::with_capacity(stride));
        while i < length {
            let code = packed[i] as usize;
            i += 1;
            if code <= 127 {
                require(i + code < length)?;
                row.extend_from_slice(&packed[i..i + code + 1]);
                i += code + 1;
            } else if code > 128 {
                require(i < length)?;
                row.extend(std::iter::repeat_n(packed[i], 257 - code));
                i += 1;
            }
            require(row.len() <= stride)?;
        }
        require(row.len() == stride)?;
        // PNG grayscale 1 is white; this PICT's palette index 1 is black.
        raw.push(0);
        raw.extend(row[..(width + 7) / 8].iter().map(|v| v ^ 0xff));
    }
    p += p % 2;
    require(data.get(p..) == Some(&[0x00, 0xff][..]))?;
    Ok((png_gray1(width as u32, height as u32, &raw), width as u32, height as u32))
}

fn crc32(data: &[u8]) -> u32 {
    let mut crc = !0u32;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = if crc & 1 != 0 { 0xedb8_8320 ^ (crc >> 1) } else { crc >> 1 };
        }
    }
    !crc
}

fn png_gray1(width: u32, height: u32, filtered_rows: &[u8]) -> Vec<u8> {
    let chunk = |out: &mut Vec<u8>, kind: &[u8; 4], body: &[u8]| {
        out.extend_from_slice(&(body.len() as u32).to_be_bytes());
        let mut tagged = kind.to_vec();
        tagged.extend_from_slice(body);
        out.extend_from_slice(&tagged);
        out.extend_from_slice(&crc32(&tagged).to_be_bytes());
    };
    let mut out = b"\x89PNG\r\n\x1a\n".to_vec();
    let mut header = width.to_be_bytes().to_vec();
    header.extend_from_slice(&height.to_be_bytes());
    header.extend_from_slice(&[1, 0, 0, 0, 0]);
    chunk(&mut out, b"IHDR", &header);
    // 200 dpi, as pixels per metre.
    let mut phys = 7874u32.to_be_bytes().to_vec();
    phys.extend_from_slice(&7874u32.to_be_bytes());
    phys.push(1);
    chunk(&mut out, b"pHYs", &phys);
    chunk(&mut out, b"IDAT", &miniz_oxide::deflate::compress_to_vec_zlib(filtered_rows, 9));
    chunk(&mut out, b"IEND", &[]);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn png_chunks_carry_valid_crcs() {
        let png = png_gray1(8, 1, &[0, 0xff]);
        assert_eq!(&png[..8], b"\x89PNG\r\n\x1a\n");
        // IHDR's CRC over type and body.
        assert_eq!(crc32(&png[12..29]), u32::from_be_bytes(png[29..33].try_into().unwrap()));
    }

    #[test]
    fn other_picts_are_refused() {
        assert!(pict_png(&[0; 700]).is_err());
    }
}

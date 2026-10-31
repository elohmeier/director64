//! Original vectorShape descriptor facts retained for live straight polygons.
//! The FLSH v23/v26 descriptors store stroke/fill colors and a Lingo value list;
//! Point values use Director's on-disk vertical/horizontal order. Raster SWF
//! remains the original static presentation for shapes outside this subset.
use serde_json::{Value, json};

type R<T> = Result<T, String>;
struct Reader<'a> {
    bytes: &'a [u8],
    at: usize,
    symbols: Vec<String>,
}
impl Reader<'_> {
    fn word(&mut self) -> R<u32> {
        let b = self
            .bytes
            .get(self.at..self.at + 4)
            .ok_or("truncated vector descriptor")?;
        self.at += 4;
        Ok(u32::from_be_bytes(b.try_into().unwrap()))
    }
    fn symbol(&mut self) -> R<String> {
        if self.word()? != 2 {
            return Err("vector property key is not a symbol".into());
        }
        let size = self.word()?;
        if size & 0x80000000 != 0 {
            return self
                .symbols
                .get((size & 0x7fffffff) as usize)
                .cloned()
                .ok_or("invalid vector symbol reference".into());
        }
        let b = self
            .bytes
            .get(self.at..self.at + size as usize)
            .ok_or("truncated vector symbol")?;
        self.at += size as usize;
        let text = std::str::from_utf8(b)
            .map_err(|_| "invalid vector symbol")?
            .to_string();
        self.symbols.push(text.clone());
        Ok(text)
    }
}
/// None means the static SWF is supported but live polygon editing needs a
/// descriptor/version/curve implementation. It must never be guessed from pixels.
pub fn vector_shape_model(payload: &[u8]) -> R<Option<Value>> {
    let word = |at: usize| {
        payload
            .get(at..at + 4)
            .map(|b| u32::from_be_bytes(b.try_into().unwrap()))
    };
    let version = word(8);
    if payload.get(..4) != Some(b"FLSH") || !matches!(version, Some(23 | 26)) {
        return Ok(None);
    }
    if word(4) != Some(payload.len() as u32) || payload.len() < 236 {
        return Err("invalid vector descriptor length".into());
    }
    let color = |at: usize| -> R<u32> {
        if word(at) != Some(18) {
            return Err("invalid vector color value".into());
        }
        let rgb = [word(at + 4), word(at + 8), word(at + 12)];
        if rgb.iter().any(|c| c.is_none_or(|n| n > 255)) {
            return Err("invalid vector color channel".into());
        }
        Ok((rgb[0].unwrap() << 16) | (rgb[1].unwrap() << 8) | rgb[2].unwrap())
    };
    let width = f32::from_bits(word(132).unwrap()) as f64;
    let closed = word(128).unwrap();
    let mode = word(136).unwrap();
    if !width.is_finite() || !(0.0..=255.0).contains(&width) || closed > 1 || mode > 2 {
        return Err("invalid vector stroke/fill properties".into());
    }
    let (stroke, fill) = (color(164)?, color(180)?);
    if mode == 2 {
        return Ok(None);
    } // Gradient fill remains static.
    let mut r = Reader {
        bytes: payload,
        at: 228,
        symbols: Vec::new(),
    };
    if r.word()? != 7 {
        return Err("vector vertices are not a list".into());
    }
    let count = r.word()?;
    if count > 1024 {
        return Err("invalid vector vertex count".into());
    }
    let mut vertices = Vec::new();
    let mut curved = false;
    let mut compound = false;
    for _ in 0..count {
        let kind = r.word()?;
        if kind == 7 {
            // Director stores a contour boundary as the one-element list
            // [#newCurve], interspersed with vertex property lists. The
            // original XMED SWF already retains its compound fill and holes.
            if r.word()? != 1 || r.symbol()? != "newCurve" {
                return Err("invalid vector contour separator".into());
            }
            compound = true;
            continue;
        }
        if kind != 10 {
            return Err("vector vertex is not a property list".into());
        }
        let properties = r.word()?;
        if !(1..=3).contains(&properties) {
            return Err("invalid vector vertex properties".into());
        }
        let mut point = None;
        let mut handles = [false; 2];
        for _ in 0..properties {
            let key = r.symbol()?;
            if r.word()? != 8 {
                return Err("invalid vector vertex point".into());
            }
            let y = r.word()? as i32;
            let x = r.word()? as i32;
            if !(-65535..=65535).contains(&x) || !(-65535..=65535).contains(&y) {
                return Err("vector coordinate out of range".into());
            }
            match key.as_str() {
                "vertex" if point.is_none() => point = Some((x, y)),
                "handle1" if !handles[0] => handles[0] = true,
                "handle2" if !handles[1] => handles[1] = true,
                _ => return Err("unknown or repeated vector vertex property".into()),
            }
        }
        let (x, y) = point.ok_or("vector vertex point is absent")?;
        curved |= handles.iter().any(|handle| *handle);
        vertices.push(json!([x, y]));
    }
    if version == Some(26) {
        // Selected Director10.1 source descriptors retain the same header
        // and integer-point list as v23, then a marker and typed symbol.
        // Only these fully audited editor discriminators are supported.
        if r.word()? > 1 {
            return Err("invalid vector v26 trailer flag".into());
        }
        let discriminator = r.symbol()?;
        if r.at != payload.len() {
            return Err("trailing vector v26 descriptor bytes".into());
        }
        if !matches!(discriminator.as_str(), "vectorShape" | "none") {
            return Ok(None);
        }
    }
    // Empty/single-point editor templates and compound/Bezier paths are
    // legitimate authored descriptors, not corrupt polygons. Their exact
    // static SWF remains usable; live edits still require the corresponding
    // topology support and fail explicitly instead of inventing geometry.
    if vertices.len() < 2 || curved || compound {
        return Ok(None);
    }
    Ok(Some(
        json!({"vertices":vertices,"closed":closed != 0,"filled":mode == 1,
        "strokeWidth":width,"strokeColor":stroke,"fillColor":fill}),
    ))
}

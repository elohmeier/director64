//! Director's eight texture fills are media: read them from the selected
//! engine's PE bitmap resources, never embed recovered pixels in source.
//! VWTL layout corroborated with clean ScummVM41ac2b31847622d0662d22c03fe6979e3b43cfbc;
//! PE resource layout follows Microsoft's PE/COFF directory representation.
use super::{
    fdi,
    files::{self, Files},
    film, movie,
    source::sha256,
};
use serde_json::{Value, json};

type R<T> = Result<T, String>;
fn u16le(b: &[u8], p: usize) -> R<u16> {
    Ok(u16::from_le_bytes(
        b.get(p..p.checked_add(2).ok_or("PE offset overflow")?)
            .ok_or("truncated PE/DIB word")?
            .try_into()
            .unwrap(),
    ))
}
fn u32le(b: &[u8], p: usize) -> R<u32> {
    Ok(u32::from_le_bytes(
        b.get(p..p.checked_add(4).ok_or("PE offset overflow")?)
            .ok_or("truncated PE/DIB long")?
            .try_into()
            .unwrap(),
    ))
}

/// Uncompressed Windows PJ00/PJ01 runtime component table. The footer
/// locates a 24-byte header followed by 52-byte component records; each
/// record stores its absolute file offset, byte count and NUL-terminated
/// basename. Reject other projector layouts rather than scanning for MZ.
pub fn embedded_library<'a>(bytes: &'a [u8], name: &str) -> R<&'a [u8]> {
    let footer = bytes
        .len()
        .checked_sub(4)
        .ok_or("truncated projector footer")?;
    let at = u32le(bytes, footer)? as usize;
    let header = bytes
        .get(at..at.checked_add(24).ok_or("projector header extent")?)
        .ok_or("truncated projector header")?;
    if &header[..4] != b"00JP" && &header[..4] != b"10JP" {
        return Err("unsupported embedded projector layout".into());
    }
    let count = u32le(header, 20)? as usize;
    if count == 0 || count > 128 {
        return Err("projector runtime component count".into());
    }
    let records = bytes
        .get(
            at + 24
                ..(at + 24)
                    .checked_add(count * 52)
                    .ok_or("projector table extent")?,
        )
        .ok_or("truncated projector runtime table")?;
    let mut found = None;
    for record in records.chunks_exact(52) {
        let label = &record[8..41];
        let end = label
            .iter()
            .position(|v| *v == 0)
            .ok_or("unterminated projector runtime name")?;
        if !label[..end].eq_ignore_ascii_case(name.as_bytes()) {
            continue;
        }
        if found.is_some() {
            return Err("ambiguous projector runtime component".into());
        }
        let offset = u32le(record, 0)? as usize;
        let len = u32le(record, 4)? as usize;
        if offset < at + 24 + records.len() || len < 64 {
            return Err("invalid projector runtime extent".into());
        }
        let end = offset
            .checked_add(len)
            .ok_or("projector runtime overflow")?;
        if end > footer {
            return Err("projector runtime overlaps footer".into());
        }
        let dll = bytes
            .get(offset..end)
            .ok_or("truncated projector runtime component")?;
        if dll.get(..2) != Some(b"MZ") {
            return Err("compressed or invalid projector runtime component".into());
        }
        found = Some(dll);
    }
    found.ok_or_else(|| format!("projector runtime component {name} missing"))
}

/// Return a numeric RT_BITMAP resource. Resource offsets are relative to
/// the resource directory; payload RVAs are mapped through section bounds.
pub fn pe_bitmap(bytes: &[u8], bitmap: u16) -> R<&[u8]> {
    if bytes.get(..2) != Some(b"MZ") {
        return Err("pattern library is not PE".into());
    }
    let pe = u32le(bytes, 0x3c)? as usize;
    if bytes.get(pe..pe.checked_add(4).ok_or("PE header overflow")?) != Some(b"PE\0\0") {
        return Err("invalid pattern PE signature".into());
    }
    bytes
        .get(pe..pe.checked_add(24).ok_or("PE header extent")?)
        .ok_or("truncated PE header")?;
    let count = u16le(bytes, pe + 6)? as usize;
    let optional = u16le(bytes, pe + 20)? as usize;
    if count == 0 || count > 96 {
        return Err("pattern PE section count".into());
    }
    let opt = pe.checked_add(24).ok_or("PE optional offset")?;
    let dir = match u16le(bytes, opt)? {
        0x10b => 96,
        0x20b => 112,
        _ => return Err("pattern PE optional magic".into()),
    };
    if optional < dir + 24 || u32le(bytes, opt + dir - 4)? < 3 {
        return Err("pattern PE lacks resource directory".into());
    }
    let sections = opt.checked_add(optional).ok_or("PE section offset")?;
    bytes
        .get(
            sections
                ..sections
                    .checked_add(count * 40)
                    .ok_or("PE sections extent")?,
        )
        .ok_or("truncated PE sections")?;
    let map = |rva: u32, len: u32| -> R<&[u8]> {
        for n in 0..count {
            let s = sections.checked_add(n * 40).ok_or("PE section extent")?;
            let address = u32le(bytes, s + 12)?;
            let size = u32le(bytes, s + 16)?;
            let Some(offset) = rva.checked_sub(address) else {
                continue;
            };
            if offset <= size && len <= size - offset {
                let p = u32le(bytes, s + 20)?
                    .checked_add(offset)
                    .ok_or("PE raw offset")? as usize;
                return bytes
                    .get(p..p.checked_add(len as usize).ok_or("PE resource extent")?)
                    .ok_or_else(|| "pattern PE resource outside file".into());
            }
        }
        Err("pattern PE resource RVA outside sections".into())
    };
    let resource = map(u32le(bytes, opt + dir + 16)?, u32le(bytes, opt + dir + 20)?)?;
    let entries = |at: usize| -> R<Vec<(u32, u32)>> {
        resource
            .get(at..at.checked_add(16).ok_or("PE directory extent")?)
            .ok_or("truncated PE directory")?;
        let named = u16le(resource, at + 12)? as usize;
        let numbered = u16le(resource, at + 14)? as usize;
        if named + numbered > 4096 {
            return Err("pattern PE directory count".into());
        }
        (0..named + numbered)
            .map(|n| {
                let p = at.checked_add(16 + n * 8).ok_or("PE directory extent")?;
                Ok((u32le(resource, p)?, u32le(resource, p + 4)?))
            })
            .collect()
    };
    let child = |at: usize, id: u32| -> R<usize> {
        let matching: Vec<_> = entries(at)?.into_iter().filter(|e| e.0 == id).collect();
        if matching.len() != 1 || matching[0].1 & 0x80000000 == 0 {
            return Err(format!("pattern PE directory missing or ambiguous id {id}"));
        }
        Ok((matching[0].1 & 0x7fffffff) as usize)
    };
    let languages = entries(child(child(0, 2)?, bitmap as u32)?)?;
    if languages.is_empty() {
        return Err("pattern PE bitmap has no language".into());
    }
    let mut answer = None;
    for (_, leaf) in languages {
        if leaf & 0x80000000 != 0 {
            return Err("pattern PE unexpected resource depth".into());
        }
        let leaf = leaf as usize;
        resource
            .get(leaf..leaf.checked_add(16).ok_or("PE leaf extent")?)
            .ok_or("truncated PE data entry")?;
        let data = map(u32le(resource, leaf)?, u32le(resource, leaf + 4)?)?;
        if answer.is_some_and(|old| old != data) {
            return Err("pattern PE bitmap languages differ".into());
        }
        answer = Some(data);
    }
    Ok(answer.unwrap())
}

/// Uncompressed indexed Windows DIB, including bottom-up rows and padding.
pub fn dib(bytes: &[u8]) -> R<(usize, usize, Vec<u8>)> {
    if u32le(bytes, 0)? != 40
        || u16le(bytes, 12)? != 1
        || u16le(bytes, 14)? != 8
        || u32le(bytes, 16)? != 0
    {
        return Err("unsupported pattern DIB layout".into());
    }
    let width = u32le(bytes, 4)? as i32;
    let height = u32le(bytes, 8)? as i32;
    if width <= 0 || width > 1024 || height == 0 || height.unsigned_abs() > 1024 {
        return Err("pattern DIB dimensions".into());
    }
    let (w, h) = (width as usize, height.unsigned_abs() as usize);
    let colors = match u32le(bytes, 32)? {
        0 => 256,
        n => n as usize,
    };
    if colors > 256 {
        return Err("pattern DIB palette count".into());
    }
    let pixels = 40 + colors * 4;
    let stride = (w + 3) & !3;
    if bytes.len() < pixels + stride * h {
        return Err("truncated pattern DIB".into());
    }
    let mut rgba = vec![255; w * h * 4];
    for y in 0..h {
        for x in 0..w {
            let row = if height > 0 { h - 1 - y } else { y };
            let index = bytes[pixels + row * stride + x] as usize;
            if index >= colors {
                return Err("pattern DIB palette index".into());
            }
            let p = 40 + index * 4;
            let out = (y * w + x) * 4;
            rgba[out..out + 3].copy_from_slice(&[bytes[p + 2], bytes[p + 1], bytes[p]]);
        }
    }
    Ok((w, h, rgba))
}

#[cfg(test)]
fn rgba_asset(w: usize, h: usize, rgba: &[u8]) -> R<Vec<u8>> {
    if w == 0 || h == 0 || w > 1024 || h > 1024 || rgba.len() != w * h * 4 {
        return Err("pattern texture extent".into());
    }
    let mut out = vec![0; 32 + w * h * 4];
    out[..4].copy_from_slice(b"FDI2");
    let len = out.len() as u32;
    out[4..8].copy_from_slice(&len.to_be_bytes());
    out[8..10].copy_from_slice(&(w as u16).to_be_bytes());
    out[10..12].copy_from_slice(&(h as u16).to_be_bytes());
    out[19] = 8;
    out[32..].copy_from_slice(rgba);
    Ok(out)
}

/// Indexed brush pixels retain their palette slots: the original Director 7
/// and 8.5 renderers recolor the same brush when the stage palette changes.
/// The embedded CLUT is only a Windows fallback for offline previews.
fn brush_asset(w: usize, h: usize, rgba: &[u8]) -> R<Vec<u8>> {
    if w == 0 || h == 0 || w > 1024 || h > 1024 || rgba.len() != w * h * 4 {
        return Err("pattern texture extent".into());
    }
    let indices: Vec<u8> = rgba
        .chunks_exact(4)
        .map(super::texture_palette::index)
        .collect();
    indexed_brush_asset(w, h, &indices)
}
fn indexed_brush_asset(w: usize, h: usize, indices: &[u8]) -> R<Vec<u8>> {
    if w == 0 || h == 0 || w > 1024 || h > 1024 || indices.len() != w * h {
        return Err("pattern index extent".into());
    }
    let palette = super::bitmap::windows_palette(8, true)?;
    let mut out = vec![0u8; 544 + w * h];
    out[..4].copy_from_slice(b"FDIC");
    let length = out.len() as u32;
    out[4..8].copy_from_slice(&length.to_be_bytes());
    out[8..10].copy_from_slice(&(w as u16).to_be_bytes());
    out[10..12].copy_from_slice(&(h as u16).to_be_bytes());
    out[16] = 4; // palette-following brush, never a coverage flag
    out[18] = 8;
    out[19] = 8;
    out[20..22].copy_from_slice(&256u16.to_be_bytes());
    for i in 0..256 {
        let word = ((palette[i * 6] as u16 >> 3) << 11)
            | ((palette[i * 6 + 2] as u16 >> 3) << 6)
            | ((palette[i * 6 + 4] as u16 >> 3) << 1)
            | 1;
        out[32 + i * 2..34 + i * 2].copy_from_slice(&word.to_be_bytes());
    }
    out[544..].copy_from_slice(indices);
    Ok(out)
}

/// VWTL contains up to eight sixteen-byte records in D5 and later. A zero member
/// selects the built-in texture; the rectangle belongs to an authored tile.
pub fn tile_reference(bytes: &[u8], pattern: i64) -> R<Option<Value>> {
    if !(57..=64).contains(&pattern)
        || bytes.is_empty()
        || bytes.len() > 128
        || bytes.len() % 16 != 0
    {
        return Err("unsupported VWTL layout".into());
    }
    let at = (pattern - 57) as usize * 16;
    if at >= bytes.len() {
        return Ok(None);
    }
    let be = |p: usize| u16::from_be_bytes([bytes[p], bytes[p + 1]]);
    let member = be(at + 6);
    if member == 0 {
        return Ok(None);
    }
    Ok(Some(json!({"cast":be(at+4),"member":member,
        "rect":[be(at+10) as i16,be(at+8) as i16,be(at+14) as i16,be(at+12) as i16]})))
}

/// Bind after all casts convert so an authored VWTL bitmap can refer to a
/// later member or external cast. Keep shape geometry separate from tile size.
pub fn bind(
    fs: &mut dyn Files,
    media: &str,
    output: &str,
    policy: &Value,
    movies: &mut [Value],
) -> R<()> {
    let mut tasks = Vec::new();
    for (mi, movie) in movies.iter().enumerate() {
        let mut requested = std::collections::BTreeMap::new();
        for member in movie["members"].as_array().into_iter().flatten() {
            let pattern = member["pattern"].as_i64().unwrap_or(0);
            if member["type"] == 8 && (57..=64).contains(&pattern) {
                requested.insert(
                    (member["cast"].as_i64().unwrap_or(1), pattern),
                    member["patternTile"].clone(),
                );
            }
        }
        // Retain every selectable texture for casts that use the library;
        // member.pattern can switch among these during playback.
        if policy["shape_pattern_library"].is_object() {
            for cast in movie["casts"].as_array().into_iter().flatten() {
                let number = cast["number"].as_i64().unwrap_or(1);
                if !movie["members"]
                    .as_array()
                    .into_iter()
                    .flatten()
                    .any(|m| m["type"] == 8 && m["cast"].as_i64() == Some(number))
                {
                    continue;
                }
                for pattern in 57..=64 {
                    requested.insert(
                        (number, pattern),
                        cast["patternTiles"][(pattern - 57) as usize].clone(),
                    );
                }
            }
        }
        for ((cast_number, pattern), reference) in requested {
            let source = if reference.is_object() {
                let cast = reference["cast"].as_i64().unwrap_or(0);
                let cast = if cast == 65535 { cast_number } else { cast };
                let (_, tile) = film::child_member(
                    movie,
                    cast,
                    reference["member"].as_i64().unwrap_or(0),
                    movies,
                )
                .ok_or("VWTL tile member missing")?;
                if tile["type"] != 1 {
                    return Err("VWTL tile is not a bitmap".into());
                }
                Some((
                    tile["asset"]
                        .as_str()
                        .ok_or("VWTL bitmap has no converted pixels")?
                        .to_string(),
                    reference["rect"].clone(),
                    tile["patternSource"].clone(),
                ))
            } else {
                None
            };
            tasks.push((mi, cast_number, pattern, source));
        }
    }
    let mut library = None;
    for (mi, cast_number, pattern, source) in tasks {
        let bytes = if let Some((asset, rect, indices)) = source {
            let data = fs.read(&files::join(output, &format!("images/{asset}")))?;
            if data.len() < 32 {
                return Err("VWTL bitmap header".into());
            }
            let w = u16::from_be_bytes([data[8], data[9]]) as i64;
            let h = u16::from_be_bytes([data[10], data[11]]) as i64;
            let r: Vec<i64> = rect
                .as_array()
                .ok_or("VWTL tile rectangle")?
                .iter()
                .map(|v| v.as_i64().unwrap_or(-1))
                .collect();
            if r.len() != 4 || r[2] <= r[0] || r[3] <= r[1] {
                return Err("invalid VWTL tile period".into());
            }
            // Original Director7 uses the rectangle's extent as the repeat
            // period, ignoring its left/top. It preserves BITD palette slots.
            let (tw, th) = ((r[2] - r[0]) as usize, (r[3] - r[1]) as usize);
            if tw > w as usize || th > h as usize {
                // Some unused authored slots read outside the bitmap in the
                // original. Preserve the invalid source as an unavailable slot;
                // selecting it must fail rather than reproduce heap contents.
                if movies[mi]["members"]
                    .as_array()
                    .into_iter()
                    .flatten()
                    .any(|m| {
                        m["type"] == 8
                            && m["cast"].as_i64().unwrap_or(1) == cast_number
                            && m["pattern"].as_i64() == Some(pattern)
                    })
                {
                    return Err("selected VWTL tile period exceeds bitmap".into());
                }
                for cast in movies[mi]["casts"].as_array_mut().into_iter().flatten() {
                    if cast["number"].as_i64().unwrap_or(1) == cast_number {
                        if !cast["patternProblems"].is_array() {
                            cast["patternProblems"] = json!([]);
                        }
                        cast["patternProblems"].as_array_mut().unwrap().push(json!({"pattern":pattern,"kind":"source-texture-period-outside-bitmap","rect":rect,"width":w,"height":h}));
                    }
                }
                continue;
            }
            if let Some(path) = indices["asset"].as_str() {
                if indices["width"].as_i64() != Some(w) || indices["height"].as_i64() != Some(h) {
                    return Err("VWTL index dimensions changed".into());
                }
                let plane = fs.read(&files::join(output, path))?;
                if plane.len() != w as usize * h as usize {
                    return Err("VWTL index plane extent".into());
                }
                let mut tile = Vec::with_capacity(tw * th);
                for y in 0..th {
                    tile.extend_from_slice(&plane[y * w as usize..y * w as usize + tw]);
                }
                indexed_brush_asset(tw, th, &tile)?
            } else {
                let mut rgba = Vec::with_capacity(tw * th * 4);
                for y in 0..th {
                    for x in 0..tw {
                        let mut pixel = fdi::fdi_pixel(&data, y * w as usize + x, 0);
                        pixel[3] = 255;
                        rgba.extend_from_slice(&pixel);
                    }
                }
                brush_asset(tw, th, &rgba)?
            }
        } else {
            if library.is_none() {
                let spec = &policy["shape_pattern_library"];
                let path = spec["path"]
                    .as_str()
                    .ok_or("texture pattern requires selected engine DLL")?;
                let bytes = fs.read(&files::join(media, path))?;
                if spec["sha256"].as_str() != Some(sha256(&bytes).as_str()) {
                    return Err("shape pattern engine DLL hash changed".into());
                }
                let bytes = if let Some(name) = spec["embedded"].as_str() {
                    embedded_library(&bytes, name)?.to_vec()
                } else {
                    bytes
                };
                library = Some(bytes);
            }
            let (w, h, rgba) = dib(pe_bitmap(library.as_ref().unwrap(), (pattern - 37) as u16)?)?;
            brush_asset(w, h, &rgba)?
        };
        let asset = movie::image_asset_name(&bytes);
        movie::write_asset(fs, &files::join(output, &format!("images/{asset}")), &bytes)?;
        for member in movies[mi]["members"].as_array_mut().into_iter().flatten() {
            if member["type"] == 8
                && member["cast"].as_i64().unwrap_or(1) == cast_number
                && member["pattern"].as_i64() == Some(pattern)
            {
                member["asset"] = json!(asset);
            }
        }
        for cast in movies[mi]["casts"].as_array_mut().into_iter().flatten() {
            if cast["number"].as_i64().unwrap_or(1) == cast_number {
                if !cast["patternAssets"].is_array() {
                    cast["patternAssets"] = json!([null, null, null, null, null, null, null, null]);
                }
                cast["patternAssets"][(pattern - 57) as usize] = json!(asset);
            }
        }
    }
    for movie in movies {
        for member in movie["members"].as_array_mut().into_iter().flatten() {
            if let Some(obj) = member.as_object_mut() {
                obj.remove("patternSource");
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn brushes_preserve_palette_slots_instead_of_baking_windows_rgb() {
        let bytes = brush_asset(2, 1, &[0, 0, 176, 255, 255, 255, 255, 255]).unwrap();
        assert_eq!(&bytes[..4], b"FDIC");
        assert_eq!(bytes[16], 4);
        assert_eq!(bytes[18], 8);
        assert_eq!(u16::from_be_bytes([bytes[20], bytes[21]]), 256);
        // The first slot is legacy blue191, displayed as128 under modern
        // SystemWin. Mac displays the same retained slot differently.
        assert_eq!(bytes[544], 251);
        assert_eq!(bytes[545], 0);
        assert_eq!(fdi::fdi_pixel(&bytes, 0, 0), [0, 0, 132, 255]);
    }
    fn dib_fixture() -> Vec<u8> {
        let mut b = vec![0; 40 + 8 + 8];
        b[..4].copy_from_slice(&40u32.to_le_bytes());
        b[4..8].copy_from_slice(&2u32.to_le_bytes());
        b[8..12].copy_from_slice(&2u32.to_le_bytes());
        b[12..14].copy_from_slice(&1u16.to_le_bytes());
        b[14..16].copy_from_slice(&8u16.to_le_bytes());
        b[32..36].copy_from_slice(&2u32.to_le_bytes());
        b[40..48].copy_from_slice(&[3, 2, 1, 0, 30, 20, 10, 0]);
        b[48..56].copy_from_slice(&[1, 0, 0, 0, 0, 1, 0, 0]);
        b
    }
    fn pe_fixture(dib: &[u8]) -> Vec<u8> {
        let mut b = vec![0; 0x400];
        b[..2].copy_from_slice(b"MZ");
        b[60..64].copy_from_slice(&0x80u32.to_le_bytes());
        b[0x80..0x84].copy_from_slice(b"PE\0\0");
        b[0x86..0x88].copy_from_slice(&1u16.to_le_bytes());
        b[0x94..0x96].copy_from_slice(&224u16.to_le_bytes());
        b[0x98..0x9a].copy_from_slice(&0x10bu16.to_le_bytes());
        b[0xf4..0xf8].copy_from_slice(&16u32.to_le_bytes());
        b[0x108..0x10c].copy_from_slice(&0x1000u32.to_le_bytes());
        b[0x10c..0x110].copy_from_slice(&0x200u32.to_le_bytes());
        let section = 0x178;
        b[section + 12..section + 16].copy_from_slice(&0x1000u32.to_le_bytes());
        b[section + 16..section + 20].copy_from_slice(&0x200u32.to_le_bytes());
        b[section + 20..section + 24].copy_from_slice(&0x200u32.to_le_bytes());
        for (dir, id, next) in [(0, 2, 0x80000018u32), (24, 21, 0x80000030), (48, 1033, 72)] {
            let p = 0x200 + dir;
            b[p + 14..p + 16].copy_from_slice(&1u16.to_le_bytes());
            b[p + 16..p + 20].copy_from_slice(&(id as u32).to_le_bytes());
            b[p + 20..p + 24].copy_from_slice(&next.to_le_bytes());
        }
        b[0x248..0x24c].copy_from_slice(&0x1080u32.to_le_bytes());
        b[0x24c..0x250].copy_from_slice(&(dib.len() as u32).to_le_bytes());
        b[0x280..0x280 + dib.len()].copy_from_slice(dib);
        b
    }
    #[test]
    fn bounded_pe_bitmap_and_padded_bottom_up_dib() {
        let d = dib_fixture();
        let p = pe_fixture(&d);
        let (w, h, rgba) = dib(pe_bitmap(&p, 21).unwrap()).unwrap();
        assert_eq!((w, h), (2, 2));
        assert_eq!(
            rgba,
            [1, 2, 3, 255, 10, 20, 30, 255, 10, 20, 30, 255, 1, 2, 3, 255]
        );
        assert!(pe_bitmap(&p, 22).is_err());
        for n in 0..p.len() {
            assert!(pe_bitmap(&p[..n], 21).is_err());
        }
        let mut bad = p.clone();
        bad[0x214..0x218].copy_from_slice(&u32::MAX.to_le_bytes());
        assert!(pe_bitmap(&bad, 21).is_err());
        let mut bad = d.clone();
        bad[48] = 2;
        assert!(dib(&bad).is_err());
        let mut top = d;
        top[8..12].copy_from_slice(&(-2i32).to_le_bytes());
        assert_eq!(&dib(&top).unwrap().2[..4], &[10, 20, 30, 255]);
    }
    #[test]
    fn projector_component_table_is_bounded_and_named() {
        let pe = pe_fixture(&dib_fixture());
        let mut bytes = vec![0u8; 24 + 52];
        bytes[..4].copy_from_slice(b"00JP");
        bytes[20..24].copy_from_slice(&1u32.to_le_bytes());
        bytes[24..28].copy_from_slice(&76u32.to_le_bytes());
        bytes[28..32].copy_from_slice(&(pe.len() as u32).to_le_bytes());
        bytes[32..38].copy_from_slice(b"dirapi");
        bytes.extend_from_slice(&pe);
        bytes.extend_from_slice(&0u32.to_le_bytes());
        assert_eq!(embedded_library(&bytes, "DIRAPI").unwrap(), pe);
        assert!(embedded_library(&bytes, "iml32").is_err());
        for n in 0..bytes.len() {
            assert!(embedded_library(&bytes[..n], "dirapi").is_err());
        }
        bytes[24..28].copy_from_slice(&0u32.to_le_bytes());
        assert!(embedded_library(&bytes, "dirapi").is_err());
    }
    #[test]
    fn vwtl_short_table_retains_present_entry_and_builtin_tail() {
        let mut bytes = vec![0u8; 16];
        bytes[5] = 2;
        bytes[7] = 3;
        bytes[13] = 64;
        bytes[15] = 64;
        assert_eq!(
            tile_reference(&bytes, 57).unwrap(),
            Some(json!({"cast":2,"member":3,"rect":[0,0,64,64]}))
        );
        for pattern in 58..=64 {
            assert_eq!(tile_reference(&bytes, pattern).unwrap(), None);
        }
        for length in [0, 1, 15, 17, 127, 129] {
            assert!(tile_reference(&vec![0; length], 57).is_err());
        }
    }
    #[test]
    fn vwtl_overrides_keep_top_left_indices_and_ignore_rectangle_origin() {
        let mut table = vec![0u8; 128];
        for (at, value) in [(20, 2u16), (22, 7), (24, 65535), (26, 1), (28, 1), (30, 2)] {
            table[at..at + 2].copy_from_slice(&value.to_be_bytes());
        }
        assert_eq!(tile_reference(&table, 57).unwrap(), None);
        let reference = tile_reference(&table, 58).unwrap().unwrap();
        let mut movies = vec![
            json!({"name":"host","casts":[{"number":2,"file":"cast"}],
            "members":[{"type":8,"cast":1,"number":1,"pattern":58,"patternTile":reference}]}),
            json!({"name":"cast","members":[{"type":1,"cast":1,"number":7,"asset":"tile.fdi", "patternSource":{"asset":"tile.idx","width":2,"height":2}}]}),
        ];
        let (_, _, rgba) = dib(&dib_fixture()).unwrap();
        let mut fs = super::files::Memory::new();
        fs.mkdir_all("out/images").unwrap();
        fs.write("out/images/tile.fdi", &rgba_asset(2, 2, &rgba).unwrap())
            .unwrap();
        fs.write("out/tile.idx", &[1, 2, 3, 4]).unwrap();
        bind(&mut fs, "media", "out", &json!({}), &mut movies).unwrap();
        let asset = movies[0]["members"][0]["asset"].as_str().unwrap();
        let tile = fs.read(&format!("out/images/{asset}")).unwrap();
        assert_eq!(u16::from_be_bytes([tile[8], tile[9]]), 1);
        assert_eq!(&tile[..4], b"FDIC");
        assert_eq!(tile[16], 4);
        assert_eq!(&tile[544..], &[1, 3]);
        assert!(movies[1]["members"][0]["patternSource"].is_null());
    }
    #[test]
    fn unavailable_unused_texture_stays_explicit_and_selected_invalid_source_fails() {
        let reference = json!({"cast":2,"member":7,"rect":[0,0,2,2]});
        let mut references = vec![reference; 8];
        references[4]["rect"] = json!([0, -1, 2, 2]); // period3 exceeds bitmap2
        let original = vec![
            json!({"name":"host", "casts":[
            {"number":1,"file":"host","patternTiles":references},
            {"number":2,"file":"cast"}],
            "members":[{"type":8,"cast":1,"number":1,"pattern":57}]}),
            json!({"name":"cast","members":[{"type":1,"cast":1,"number":7,"asset":"tile.fdi", "patternSource":{"asset":"tile.idx","width":2,"height":2}}]}),
        ];
        let mut fs = super::files::Memory::new();
        fs.mkdir_all("out/images").unwrap();
        fs.write(
            "out/images/tile.fdi",
            &rgba_asset(2, 2, &[255; 16]).unwrap(),
        )
        .unwrap();
        fs.write("out/tile.idx", &[0, 1, 2, 3]).unwrap();
        let mut movies = original.clone();
        bind(
            &mut fs,
            "media",
            "out",
            &json!({"shape_pattern_library":{}}),
            &mut movies,
        )
        .unwrap();
        assert!(movies[0]["casts"][0]["patternAssets"][4].is_null());
        assert_eq!(movies[0]["casts"][0]["patternProblems"][0]["pattern"], 61);
        let mut selected = original;
        selected[0]["members"][0]["pattern"] = json!(61);
        assert!(
            bind(
                &mut fs,
                "media",
                "out",
                &json!({"shape_pattern_library":{}}),
                &mut selected
            )
            .unwrap_err()
            .contains("selected VWTL tile period exceeds bitmap")
        );
    }
}

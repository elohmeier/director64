//! Source-owned Windows monochrome cursor resources. PE/COFF and RT_CURSOR
//! layout: Microsoft Resource File Formats; no retail artwork lives here.
use super::{
    files::{self, Files},
    source::sha256,
};
use serde_json::{Value, json};

type R<T> = Result<T, String>;
fn word(b: &[u8], p: usize) -> R<u16> {
    Ok(u16::from_le_bytes(
        b.get(p..p.checked_add(2).ok_or("cursor offset overflow")?)
            .ok_or("truncated cursor word")?
            .try_into()
            .unwrap(),
    ))
}
fn long(b: &[u8], p: usize) -> R<u32> {
    Ok(u32::from_le_bytes(
        b.get(p..p.checked_add(4).ok_or("cursor offset overflow")?)
            .ok_or("truncated cursor long")?
            .try_into()
            .unwrap(),
    ))
}

/// A numeric or UTF-16-named PE resource. Differing language variants are
/// rejected rather than letting the build machine choose a language.
pub fn resource(bytes: &[u8], kind: u32, id: &Value) -> R<Vec<u8>> {
    if bytes.get(..2) != Some(b"MZ") {
        return Err("cursor owner is not PE".into());
    }
    let pe = long(bytes, 60)? as usize;
    if bytes.get(pe..pe.checked_add(4).ok_or("cursor PE offset")?) != Some(b"PE\0\0") {
        return Err("cursor owner PE signature".into());
    }
    let count = word(bytes, pe + 6)? as usize;
    let optional = word(bytes, pe + 20)? as usize;
    let opt = pe.checked_add(24).ok_or("cursor optional header")?;
    let directory = match word(bytes, opt)? {
        0x10b => 96,
        0x20b => 112,
        _ => return Err("cursor PE optional magic".into()),
    };
    if optional < directory + 24 || long(bytes, opt + directory - 4)? < 3 || count > 1024 {
        return Err("cursor PE resource directory absent".into());
    }
    let sections = opt.checked_add(optional).ok_or("cursor section offset")?;
    let map = |rva: u32, size: u32| -> R<&[u8]> {
        for n in 0..count {
            let at = sections
                .checked_add(n * 40)
                .ok_or("cursor section extent")?;
            let address = long(bytes, at + 12)?;
            let raw_size = long(bytes, at + 16)?;
            if let Some(delta) = rva.checked_sub(address) {
                if delta <= raw_size && size <= raw_size - delta {
                    let offset = long(bytes, at + 20)?
                        .checked_add(delta)
                        .ok_or("cursor raw offset")? as usize;
                    return bytes
                        .get(
                            offset
                                ..offset
                                    .checked_add(size as usize)
                                    .ok_or("cursor resource extent")?,
                        )
                        .ok_or_else(|| "cursor resource outside file".into());
                }
            }
        }
        Err("cursor resource RVA outside sections".into())
    };
    let root = map(
        long(bytes, opt + directory + 16)?,
        long(bytes, opt + directory + 20)?,
    )?;
    let entries = |at: usize| -> R<Vec<(u32, u32)>> {
        let count = word(root, at + 12)? as usize + word(root, at + 14)? as usize;
        if count > 4096 {
            return Err("cursor resource directory count".into());
        }
        (0..count)
            .map(|i| Ok((long(root, at + 16 + i * 8)?, long(root, at + 20 + i * 8)?)))
            .collect()
    };
    let key = |raw: u32| -> R<Value> {
        if raw & 0x80000000 == 0 {
            return Ok(json!(raw));
        }
        let at = (raw & 0x7fffffff) as usize;
        let size = word(root, at)? as usize;
        if size > 1024 {
            return Err("cursor resource name length".into());
        }
        let units = (0..size)
            .map(|i| word(root, at + 2 + i * 2))
            .collect::<R<Vec<_>>>()?;
        Ok(json!(
            String::from_utf16(&units).map_err(|_| "cursor resource name UTF-16")?
        ))
    };
    let child = |at: usize, wanted: &Value| -> R<usize> {
        let mut matches = Vec::new();
        for (name, offset) in entries(at)? {
            if key(name)? == *wanted {
                matches.push(offset);
            }
        }
        if matches.len() != 1 || matches[0] & 0x80000000 == 0 {
            return Err(format!("cursor resource missing or ambiguous: {wanted}"));
        }
        Ok((matches[0] & 0x7fffffff) as usize)
    };
    let languages = entries(child(child(0, &json!(kind))?, id)?)?;
    let mut result: Option<Vec<u8>> = None;
    for (_, offset) in languages {
        if offset & 0x80000000 != 0 {
            return Err("cursor resource depth".into());
        }
        let offset = offset as usize;
        let data = map(long(root, offset)?, long(root, offset + 4)?)?;
        if result.as_ref().is_some_and(|old| old.as_slice() != data) {
            return Err("cursor resource languages differ".into());
        }
        result = Some(data.to_vec());
    }
    result.ok_or_else(|| "cursor resource has no language".into())
}

/// Preserve all four Windows AND/XOR outcomes, the full canvas and hotspot.
/// D64C: magic, big-endian width/height/hotX/hotY, version1, reserved0,
/// then one byte per top-down pixel: XOR bit0, AND bit1.
pub fn decode(data: &[u8]) -> R<Vec<u8>> {
    let hot_x = word(data, 0)?;
    let hot_y = word(data, 2)?;
    let width = long(data, 8)? as i32;
    let combined = long(data, 12)? as i32;
    if long(data, 4)? != 40
        || word(data, 16)? != 1
        || word(data, 18)? != 1
        || long(data, 20)? != 0
        || width <= 0
        || width > 64
        || combined <= 0
        || combined > 128
        || combined % 2 != 0
        || hot_x as i32 >= width
        || hot_y as i32 >= combined / 2
        || !matches!(long(data, 36)?, 0 | 2)
        || data.get(44..52) != Some(&[0, 0, 0, 0, 255, 255, 255, 0])
    {
        return Err("unsupported monochrome executable cursor DIB".into());
    }
    let (width, height) = (width as usize, (combined / 2) as usize);
    let stride = width.div_ceil(32) * 4;
    let plane = stride * height;
    let end = 52 + plane * 2;
    if data.len() < end || data[end..].iter().any(|b| *b != 0) {
        return Err("executable cursor mask extent".into());
    }
    let mut out = Vec::with_capacity(16 + width * height);
    out.extend_from_slice(b"D64C");
    for value in [width as u16, height as u16, hot_x, hot_y, 1, 0] {
        out.extend_from_slice(&value.to_be_bytes());
    }
    for y in 0..height {
        for x in 0..width {
            let at = (height - 1 - y) * stride + x / 8;
            let bit = 7 - x % 8;
            out.push(((data[52 + at] >> bit) & 1) | (((data[52 + plane + at] >> bit) & 1) << 1));
        }
    }
    Ok(out)
}

pub fn bind(
    fs: &mut dyn Files,
    media: &str,
    output: &str,
    policy: &Value,
    movies: &mut [Value],
) -> R<()> {
    let Some(spec) = policy.get("executable_cursors") else {
        return Ok(());
    };
    let file = spec["file"].as_str().ok_or("executable cursor file")?;
    let bytes = fs.read(&files::join(media, file))?;
    if spec["sha256"] != sha256(&bytes).as_str() {
        return Err("executable cursor owner identity changed".into());
    }
    let entries = spec["entries"]
        .as_array()
        .ok_or("executable cursor entries")?;
    if entries.is_empty() || entries.len() > 32 {
        return Err("executable cursor entry count".into());
    }
    let mut assets = Vec::new();
    let mut seen = std::collections::HashSet::new();
    for entry in entries {
        let id = entry["id"]
            .as_u64()
            .filter(|n| *n < 32)
            .ok_or("executable cursor runtime id")?;
        if !seen.insert(id) {
            return Err("duplicate executable cursor runtime id".into());
        }
        let group = resource(&bytes, 12, &entry["group"])?;
        if group.len() != 20
            || word(&group, 0)? != 0
            || word(&group, 2)? != 2
            || word(&group, 4)? != 1
            || word(&group, 10)? != 1
            || word(&group, 12)? != 1
        {
            return Err("executable cursor group must contain one monochrome frame".into());
        }
        let resource_id = word(&group, 18)?;
        if entry["resource"] != json!(resource_id) {
            return Err("executable cursor group binding changed".into());
        }
        let raw = resource(&bytes, 1, &json!(resource_id))?;
        if raw.len() != long(&group, 14)? as usize || entry["sha256"] != sha256(&raw).as_str() {
            return Err("executable cursor frame identity changed".into());
        }
        let decoded = decode(&raw)?;
        if word(&group, 6)? != u16::from_be_bytes([decoded[4], decoded[5]])
            || word(&group, 8)? != 2 * u16::from_be_bytes([decoded[6], decoded[7]])
        {
            return Err("executable cursor group dimensions differ".into());
        }
        let name = format!("d64-cursor-{id}.dcur");
        fs.write(&files::join(output, &format!("images/{name}")), &decoded)?;
        assets.push(json!(name));
    }
    for movie in movies {
        movie["cursorAssets"] = json!(assets);
        movie["cursorResourceProvenance"] = spec.clone();
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn put16(b: &mut [u8], p: usize, n: u16) {
        b[p..p + 2].copy_from_slice(&n.to_le_bytes());
    }
    fn put32(b: &mut [u8], p: usize, n: u32) {
        b[p..p + 4].copy_from_slice(&n.to_le_bytes());
    }
    fn dib() -> Vec<u8> {
        let mut b = vec![0; 68];
        put16(&mut b, 0, 1);
        put32(&mut b, 4, 40);
        put32(&mut b, 8, 4);
        put32(&mut b, 12, 4);
        put16(&mut b, 16, 1);
        put16(&mut b, 18, 1);
        b[48..52].copy_from_slice(&[255, 255, 255, 0]);
        b[56] = 0x50;
        b[60] = 0xf0;
        b[64] = 0x30;
        b
    }
    fn pe(raw: &[u8]) -> Vec<u8> {
        let mut b = vec![0; 0x400];
        b[..2].copy_from_slice(b"MZ");
        put32(&mut b, 60, 0x80);
        b[0x80..0x84].copy_from_slice(b"PE\0\0");
        put16(&mut b, 0x86, 1);
        put16(&mut b, 0x94, 224);
        put16(&mut b, 0x98, 0x10b);
        put32(&mut b, 0xf4, 16);
        put32(&mut b, 0x108, 0x1000);
        put32(&mut b, 0x10c, 0x200);
        put32(&mut b, 0x184, 0x1000);
        put32(&mut b, 0x188, 0x200);
        put32(&mut b, 0x18c, 0x200);
        put16(&mut b, 0x20e, 2);
        put32(&mut b, 0x210, 1);
        put32(&mut b, 0x214, 0x80000030);
        put32(&mut b, 0x218, 12);
        put32(&mut b, 0x21c, 0x80000050);
        for (dir, id, next, named) in [
            (0x30, 3, 0x80000070, false),
            (0x50, 0x80000120, 0x80000090, true),
            (0x70, 1033, 0xb0, false),
            (0x90, 1033, 0xc0, false),
        ] {
            put16(&mut b, 0x200 + dir + if named { 12 } else { 14 }, 1);
            put32(&mut b, 0x210 + dir, id);
            put32(&mut b, 0x214 + dir, next);
        }
        put32(&mut b, 0x2b0, 0x1140);
        put32(&mut b, 0x2b4, raw.len() as u32);
        put32(&mut b, 0x2c0, 0x11b0);
        put32(&mut b, 0x2c4, 20);
        let units: Vec<_> = "CROSSCURSOR".encode_utf16().collect();
        put16(&mut b, 0x320, units.len() as u16);
        for (i, v) in units.iter().enumerate() {
            put16(&mut b, 0x322 + i * 2, *v);
        }
        b[0x340..0x340 + raw.len()].copy_from_slice(raw);
        put16(&mut b, 0x3b2, 2);
        put16(&mut b, 0x3b4, 1);
        put16(&mut b, 0x3b6, 4);
        put16(&mut b, 0x3b8, 4);
        put16(&mut b, 0x3ba, 1);
        put16(&mut b, 0x3bc, 1);
        put32(&mut b, 0x3be, raw.len() as u32);
        put16(&mut b, 0x3c2, 3);
        b
    }
    #[test]
    fn windows_masks_preserve_canvas_hotspot_and_inverted_background_pixels() {
        let raw = dib();
        let decoded = decode(&raw).unwrap();
        assert_eq!(&decoded[..16], b"D64C\0\x04\0\x02\0\x01\0\0\0\x01\0\0");
        assert_eq!(&decoded[16..], &[0, 1, 2, 3, 2, 2, 2, 2]);
        for n in 0..raw.len() {
            assert!(decode(&raw[..n]).is_err());
        }
        for (at, value) in [(0, 4), (18, 8), (20, 1), (48, 254)] {
            let mut bad = raw.clone();
            bad[at] = value;
            assert!(decode(&bad).is_err());
        }
    }
    #[test]
    fn named_group_and_numeric_frame_are_bounded_and_pinned_through_conversion() {
        let raw = dib();
        let owner = pe(&raw);
        assert_eq!(resource(&owner, 1, &json!(3)).unwrap(), raw);
        assert_eq!(
            resource(&owner, 12, &json!("CROSSCURSOR")).unwrap().len(),
            20
        );
        assert!(resource(&owner, 12, &json!("OTHER")).is_err());
        for n in 0..owner.len() {
            assert!(resource(&owner[..n], 1, &json!(3)).is_err());
        }
        let mut bad = owner.clone();
        put32(&mut bad, 0x218, 1);
        assert!(resource(&bad, 1, &json!(3)).is_err());
        let mut fs = super::files::Memory::new();
        fs.mkdir_all("media").unwrap();
        fs.mkdir_all("out/images").unwrap();
        fs.write("media/player.exe", &owner).unwrap();
        let policy = json!({"executable_cursors":{"file":"player.exe","sha256":sha256(&owner),
            "entries":[{"id":2,"group":"CROSSCURSOR","resource":3,"sha256":sha256(&raw)}]}});
        let mut movies = vec![json!({})];
        bind(&mut fs, "media", "out", &policy, &mut movies).unwrap();
        assert_eq!(
            fs.read("out/images/d64-cursor-2.dcur").unwrap(),
            decode(&raw).unwrap()
        );
        assert_eq!(movies[0]["cursorAssets"], json!(["d64-cursor-2.dcur"]));
        let mut wrong = policy.clone();
        wrong["executable_cursors"]["entries"][0]["resource"] = json!(4);
        assert!(bind(&mut fs, "media", "out", &wrong, &mut movies).is_err());
        let mut duplicate = policy.clone();
        duplicate["executable_cursors"]["entries"]
            .as_array_mut()
            .unwrap()
            .push(policy["executable_cursors"]["entries"][0].clone());
        assert!(bind(&mut fs, "media", "out", &duplicate, &mut movies).is_err());
        let mut changed = policy.clone();
        changed["executable_cursors"]["sha256"] = json!("different");
        assert!(bind(&mut fs, "media", "out", &changed, &mut movies).is_err());
    }
}

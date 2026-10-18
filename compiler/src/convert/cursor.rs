//! Cursor Xtra's persistent frame GUIDs, verified with the original 8.5.1r104
//! Xtra: castMemberList resolves slot-15 bitmap GUIDs; payload words 28/32
//! are hotspot coordinates (not member IDs). No cursor artwork is embedded.
use super::{
    d5media, fdi,
    files::{self, Files},
    js, movie,
};
use serde_json::{Value, json};
use std::collections::{HashMap, HashSet};
type R<T> = Result<T, String>;

fn word(data: &[u8], at: usize) -> R<u32> {
    Ok(u32::from_be_bytes(
        data.get(at..at + 4)
            .ok_or("truncated cursor descriptor")?
            .try_into()
            .unwrap(),
    ))
}
pub fn bitmap_guid(cast: &[u8]) -> R<Option<String>> {
    if word(cast, 4)? == 0 {
        return Ok(None);
    }
    let fields = d5media::cast_info_entries(cast)?;
    let guid = fields.get(15).copied().unwrap_or_default();
    if guid.is_empty() {
        return Ok(None);
    }
    if guid.len() != 16 {
        return Err("bitmap persistent GUID extent".into());
    }
    Ok(Some(js::hex(guid)))
}
pub fn descriptor(payload: &[u8], media: &[u8]) -> R<Value> {
    if word(payload, 0)? as usize != payload.len()
        || word(payload, 4)? != 1
        || word(payload, 8)? != 0
        || word(payload, 12)? != 0
        || media != 1u32.to_be_bytes()
    {
        return Err("unsupported Cursor Xtra descriptor".into());
    }
    let size = word(payload, 16)?;
    let interval = word(payload, 20)?;
    let auto_mask = word(payload, 24)?;
    let (x, y, frames) = (word(payload, 28)?, word(payload, 32)?, word(payload, 36)?);
    if size != 16 || frames != 1 || payload.len() != 56 || auto_mask > 1 || x >= size || y >= size {
        return Err("Cursor Xtra requires a single 16-pixel frame and valid hotspot".into());
    }
    Ok(
        json!({"size":size,"interval":interval,"autoMask":auto_mask != 0,
        "hotX":x,"hotY":y,"frameGuids":[js::hex(&payload[40..56])]}),
    )
}

/// Resolve UUIDs after every cast has converted. Repeated external-cast
/// copies may share a GUID only when they describe identical pixels.
pub fn bind(fs: &mut dyn Files, output: &str, movies: &mut [Value]) -> R<()> {
    let needed: HashSet<String> = movies
        .iter()
        .flat_map(|movie| movie["members"].as_array().into_iter().flatten())
        .filter_map(|member| {
            member["cursor"]["frameGuids"][0]
                .as_str()
                .map(str::to_owned)
        })
        .collect();
    let mut images: HashMap<String, String> = HashMap::new();
    for movie in movies.iter() {
        for member in movie["members"].as_array().into_iter().flatten() {
            let Some(guid) = member["bitmapGuid"].as_str() else {
                continue;
            };
            if !needed.contains(guid) {
                continue;
            }
            let Some(asset) = member["asset"].as_str() else {
                continue;
            };
            if let Some(previous) = images.insert(guid.to_owned(), asset.to_owned()) {
                if previous != asset {
                    return Err(format!("cursor GUID {guid} has conflicting bitmap assets"));
                }
            }
        }
    }
    for movie in movies.iter_mut() {
        for member in movie["members"].as_array_mut().into_iter().flatten() {
            let cursor = &member["cursor"];
            if !cursor.is_object() {
                continue;
            }
            let guid = cursor["frameGuids"][0]
                .as_str()
                .ok_or("cursor frame GUID missing")?;
            let asset = images
                .get(guid)
                .ok_or_else(|| format!("cursor bitmap GUID {guid} unresolved"))?;
            let source = fs.read(&files::join(output, &format!("images/{asset}")))?;
            if source.len() < 32
                || ![b"FDI1", b"FDIA", b"FDI2"].contains(&(&source[..4]).try_into().unwrap())
            {
                return Err("cursor source has no converted bitmap pixels".into());
            }
            let (width, height) = (
                u16::from_be_bytes([source[8], source[9]]) as usize,
                u16::from_be_bytes([source[10], source[11]]) as usize,
            );
            if width == 0 || height == 0 || width > 16 || height > 16 {
                return Err("Cursor Xtra bitmap resampling requires projector evidence".into());
            }
            let mut rgba = vec![0; 16 * 16 * 4];
            for y in 0..height {
                for x in 0..width {
                    let mut pixel = fdi::fdi_pixel(&source, y * width + x, 0);
                    // Cursor Xtra Auto Mask keys every white sample, including
                    // enclosed white areas. Ordinary bitmap sprite alpha differs.
                    pixel[3] = if cursor["autoMask"] == true && pixel[..3] == [255, 255, 255] {
                        0
                    } else {
                        255
                    };
                    rgba[(y * 16 + x) * 4..(y * 16 + x + 1) * 4].copy_from_slice(&pixel);
                }
            }
            let meta = fdi::FdiMeta {
                width: 16,
                height: 16,
                use_alpha: true,
                reg_x: cursor["hotX"].as_i64().unwrap(),
                reg_y: cursor["hotY"].as_i64().unwrap(),
                source_depth: 8,
                ..Default::default()
            };
            let pixels = fdi::fdi_image(&meta, &rgba)?;
            let asset = movie::image_asset_name(&pixels);
            movie::write_asset(
                fs,
                &files::join(output, &format!("images/{asset}")),
                &pixels,
            )?;
            member["sourceType"] = json!(15);
            member["type"] = json!(1);
            member["width"] = json!(16);
            member["height"] = json!(16);
            member["regX"] = json!(meta.reg_x);
            member["regY"] = json!(meta.reg_y);
            member["useAlpha"] = json!(true);
            member["asset"] = json!(asset);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn guid_frames_are_not_member_numbers() {
        let words = [56u32, 1, 0, 0, 16, 100, 1, 7, 0, 1];
        let mut payload: Vec<u8> = words.into_iter().flat_map(u32::to_be_bytes).collect();
        payload.extend(0u8..16);
        let desc = descriptor(&payload, &1u32.to_be_bytes()).unwrap();
        assert_eq!(desc["hotX"], 7);
        assert_eq!(desc["hotY"], 0);
        assert_eq!(desc["frameGuids"][0], "000102030405060708090a0b0c0d0e0f");
        payload[39] = 2;
        assert!(
            descriptor(&payload, &1u32.to_be_bytes())
                .unwrap_err()
                .contains("single")
        );
    }
    #[test]
    fn persistent_guid_is_cast_info_slot_fifteen() {
        let count = 22usize;
        let base = 20 + 2 + (count + 1) * 4;
        let mut info = vec![0u8; base + 16];
        info[..4].copy_from_slice(&20u32.to_be_bytes());
        info[20..22].copy_from_slice(&(count as u16).to_be_bytes());
        for i in 0..=count {
            let offset = if i <= 15 { 0u32 } else { 16u32 };
            info[22 + i * 4..26 + i * 4].copy_from_slice(&offset.to_be_bytes());
        }
        info[base..].copy_from_slice(&(0u8..16).collect::<Vec<_>>());
        let mut cast = vec![0; 12];
        cast[..4].copy_from_slice(&1u32.to_be_bytes());
        cast[4..8].copy_from_slice(&(info.len() as u32).to_be_bytes());
        cast.extend_from_slice(&info);
        assert_eq!(
            bitmap_guid(&cast).unwrap().unwrap(),
            "000102030405060708090a0b0c0d0e0f"
        );
        cast.pop();
        assert!(bitmap_guid(&cast).is_err());
    }
    #[test]
    fn color_and_white_key_resolve_by_guid() {
        let mut fs = files::Memory::default();
        fs.mkdir_all("out/images").unwrap();
        let source = fdi::fdi_image(
            &fdi::FdiMeta {
                width: 3,
                height: 1,
                ..Default::default()
            },
            &[0, 255, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255],
        )
        .unwrap();
        fs.write("out/images/source.fdi", &source).unwrap();
        let mut movies = vec![json!({"members":[
            {"type":15,"cursor":{"hotX":7,"hotY":0,"autoMask":true,"frameGuids":["example"]}},
            {"type":1,"bitmapGuid":"example","asset":"source.fdi"} ]})];
        bind(&mut fs, "out", &mut movies).unwrap();
        let member = &movies[0]["members"][0];
        assert_eq!(member["type"], 1);
        assert_eq!(member["regX"], 7);
        let data = fs
            .read(&format!("out/images/{}", member["asset"].as_str().unwrap()))
            .unwrap();
        assert_eq!(fdi::fdi_pixel(&data, 0, 0), [0, 255, 0, 255]);
        assert_eq!(fdi::fdi_pixel(&data, 1, 0)[3], 0);
        assert_eq!(fdi::fdi_pixel(&data, 2, 0), [0, 0, 0, 255]);
        assert_eq!(fdi::fdi_pixel(&data, 16, 0)[3], 0);
        movies[0]["members"][0]["cursor"]["frameGuids"][0] = json!("missing");
        assert!(
            bind(&mut fs, "out", &mut movies)
                .unwrap_err()
                .contains("unresolved")
        );
    }
}

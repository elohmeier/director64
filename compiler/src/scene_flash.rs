//! Bounded retained Flash metadata shared by package and native emitters.
use super::*;
#[derive(Clone, Default, Debug)]
pub struct Action {
    pub frame: u16,
    pub event: u32,
    pub key: u8,
    pub code: Vec<u8>,
}
pub struct SoundEvent {
    pub frame: u16,
    pub sound: u16,
    pub loops: u16,
    pub flags: u8,
    pub in_point: u32,
    pub out_point: u32,
    pub envelopes: Vec<(u32, u16, u16)>,
}
pub struct Clip {
    pub id: u16,
    pub frame_count: u16,
    pub local_bounds: [i32; 4],
    pub frames: Vec<Vec<FilmLayer>>,
    pub actions: Vec<Action>,
    pub labels: Vec<(String, u16)>,
    pub sounds: Vec<SoundEvent>,
}
pub struct Button {
    pub id: u16,
    pub track_as_menu: bool,
    pub local_bounds: [i32; 4],
    pub states: Vec<Vec<FilmLayer>>,
    pub actions: Vec<Action>,
}
pub struct Sound {
    pub id: u16,
    pub asset: String,
    pub samples: u32,
    pub rate: u32,
}
#[derive(Default)]
pub struct Runtime {
    pub clips: Vec<Clip>,
    pub buttons: Vec<Button>,
    pub sounds: Vec<Sound>,
    pub exports: Vec<(u16, String)>,
    pub initializers: Vec<(u16, Action)>,
}
fn number<T: TryFrom<i64>>(v: &Value, key: &str) -> Result<T, String> {
    T::try_from(int(v.get(key))).map_err(|_| format!("Flash {key} exceeds integer range"))
}
fn list<'a>(v: &'a Value, key: &str, max: usize) -> Result<&'a [Value], String> {
    let values = match v.get(key) {
        Some(Value::Array(a)) => a.as_slice(),
        None => &[],
        _ => return Err(format!("Flash {key} is not an array")),
    };
    if values.len() > max {
        return Err(format!("Flash {key} exceeds {max}"));
    }
    Ok(values)
}
fn rectangle(v: &Value) -> Result<[i32; 4], String> {
    let a = v["localBounds"]
        .as_array()
        .filter(|a| a.len() == 4)
        .ok_or("Flash definition has no local bounds")?;
    let mut out = [0; 4];
    for (o, v) in out.iter_mut().zip(a) {
        *o = fixed16(Some(v))?;
    }
    if out[2] < 0 || out[3] < 0 {
        return Err("negative Flash definition bounds".into());
    }
    Ok(out)
}
/// Validate action records and branch destinations before storing executable
/// bytes. A required opcode without an implementation fails conversion, so a
/// later activity cannot become the first place an incomplete VM is noticed.
pub fn action_bytes(code: &[u8]) -> Result<(), String> {
    if code.is_empty() || code.len() > 1024 * 1024 || code.last() != Some(&0) {
        return Err("invalid Flash action bytes".into());
    }
    action_block(code, 0)
}
fn action_block(code: &[u8], depth: usize) -> Result<(), String> {
    if depth > 32 {
        return Err("Flash function nesting exceeds profile".into());
    }
    let mut starts = HashSet::new();
    let mut jumps = Vec::new();
    let mut p = 0;
    while p < code.len() {
        let offset = p;
        starts.insert(p);
        let op = code[p];
        p += 1;
        if !matches!(op,0|4..=7|0x0a..=0x0d|0x10..=0x12|0x17|0x1c..=0x1d|
            0x20|0x22..=0x23|0x26..=0x28|0x30|0x34|0x3a..=0x44|0x46..=0x55|
            0x60..=0x67|0x69|0x81|0x87..=0x88|0x8b|0x8e|0x96|0x99..=0x9b|0x9d|0x9f)
        {
            return Err(format!(
                "unsupported AVM1 opcode 0x{op:02x} at byte {offset}"
            ));
        }
        if op == 0 {
            if p != code.len() {
                return Err("bytes after Flash ActionEnd".into());
            }
            break;
        }
        let size = if op >= 128 {
            let bytes = code.get(p..p + 2).ok_or("truncated Flash action length")?;
            p += 2;
            u16::from_le_bytes(bytes.try_into().unwrap()) as usize
        } else {
            0
        };
        let end = p
            .checked_add(size)
            .filter(|&n| n <= code.len())
            .ok_or("truncated Flash action payload")?;
        let data = &code[p..end];
        let string_end = |at: usize| {
            data.get(at..)
                .and_then(|b| b.iter().position(|&b| b == 0))
                .map(|n| at + n + 1)
                .ok_or("unterminated Flash action string")
        };
        match op {
            0x81 if size != 2 => return Err("invalid Flash GotoFrame length".into()),
            0x87 if size != 1 => return Err("invalid Flash StoreRegister length".into()),
            0x8b if string_end(0)? != size => return Err("trailing Flash SetTarget bytes".into()),
            0x9a if size != 1 || data[0] & 0x3c != 0 => {
                return Err("invalid Flash GetURL2 flags/length".into());
            }
            0x9f if size == 0
                || data[0] & !3 != 0
                || size != if data[0] & 2 != 0 { 3 } else { 1 } =>
            {
                return Err("invalid Flash GotoFrame2 flags/length".into());
            }
            0x88 => {
                let count = data.get(..2).ok_or("truncated Flash constant pool")?;
                let count = u16::from_le_bytes(count.try_into().unwrap());
                if count > 4096 {
                    return Err("Flash constant pool exceeds runtime capacity".into());
                }
                let mut q = 2;
                for _ in 0..count {
                    q = string_end(q)?;
                }
                if q != size {
                    return Err("trailing Flash constant pool bytes".into());
                }
            }
            0x96 => {
                let mut q = 0;
                while q < size {
                    let kind = data[q];
                    q += 1;
                    q = match kind {
                        0 => string_end(q)?,
                        1 | 7 => q + 4,
                        2 | 3 => q,
                        4 | 5 | 8 => q + 1,
                        6 => q + 8,
                        9 => q + 2,
                        _ => {
                            return Err(format!(
                                "unsupported AVM1 Push type {kind} at byte {offset}"
                            ));
                        }
                    };
                    if q > size {
                        return Err("truncated Flash Push operand".into());
                    }
                }
            }
            _ => {}
        }
        if matches!(op, 0x99 | 0x9d) {
            if size != 2 {
                return Err("invalid Flash branch length".into());
            }
            jumps.push(end as i64 + i16::from_le_bytes(code[p..end].try_into().unwrap()) as i64);
        }
        if matches!(op, 0x9b | 0x8e) {
            let payload = &code[p..end];
            let zero = |at: usize| {
                payload
                    .get(at..)
                    .and_then(|b| b.iter().position(|&b| b == 0))
                    .map(|n| at + n + 1)
                    .ok_or("truncated Flash function name")
            };
            let mut q = zero(0)?;
            let count = payload
                .get(q..q + 2)
                .ok_or("truncated Flash function parameter count")?;
            let count = u16::from_le_bytes(count.try_into().unwrap());
            q += 2;
            if count > 64 {
                return Err("Flash function parameters exceed runtime capacity".into());
            }
            let mut registers = 0;
            if op == 0x8e {
                let header = payload
                    .get(q..q + 3)
                    .ok_or("truncated Flash function register flags")?;
                registers = header[0];
                let flags = u16::from_le_bytes(header[1..3].try_into().unwrap());
                if flags & !511 != 0 || flags & 3 == 3 || flags & 12 == 12 || flags & 48 == 48 {
                    return Err("invalid Flash function preload flags".into());
                }
                let preloads = (flags & 0x1d5).count_ones();
                if preloads > 0 && preloads >= u32::from(registers) {
                    return Err("Flash function preload register overflow".into());
                }
                q += 3;
            }
            for _ in 0..count {
                if op == 0x8e {
                    let register = *payload.get(q).ok_or("truncated Flash parameter register")?;
                    if register > 0 && register >= registers {
                        return Err("Flash function parameter register overflow".into());
                    }
                    q += 1;
                }
                q = zero(q)?;
            }
            let length = payload
                .get(q..q + 2)
                .ok_or("truncated Flash function body size")?;
            if q + 2 != payload.len() {
                return Err("trailing Flash function header bytes".into());
            }
            let size = u16::from_le_bytes(length.try_into().unwrap()) as usize;
            let body_end = end
                .checked_add(size)
                .filter(|&n| n <= code.len())
                .ok_or("truncated Flash function body")?;
            action_block(&code[end..body_end], depth + 1)?;
            p = body_end;
        } else {
            p = end;
        }
    }
    starts.insert(code.len()); // A bounded function may branch to its implicit return.
    if jumps
        .iter()
        .any(|&n| n < 0 || !starts.contains(&(n as usize)))
    {
        return Err("Flash branch is not an instruction boundary".into());
    }
    Ok(())
}

pub fn actions(v: &Value, frames: usize) -> Result<Vec<Action>, String> {
    list(v, "actions", 65535)?
        .iter()
        .map(|v| {
            let frame: u16 = number(v, "frame")?;
            if frame as usize > frames {
                return Err("Flash action frame outside timeline".into());
            }
            let code = list(v, "code", 1024 * 1024)?
                .iter()
                .map(|v| {
                    u8::try_from(int(Some(v))).map_err(|_| "Flash action byte outside range".into())
                })
                .collect::<Result<Vec<u8>, String>>()?;
            action_bytes(&code)?;
            Ok(Action {
                frame,
                event: number(v, "event")?,
                key: number(v, "key")?,
                code,
            })
        })
        .collect()
}
pub fn layers(frame: &Value) -> Result<Vec<FilmLayer>, String> {
    let mut previous = 0;
    let values = frame
        .as_array()
        .filter(|a| a.len() < 1000)
        .ok_or("Flash layer frame exceeds profile")?;
    values
        .iter()
        .map(|v| {
            let channel: u16 = number(v, "channel")?;
            if channel <= previous || channel >= 1000 {
                return Err("Flash layer depth outside runtime profile or unordered".into());
            }
            previous = channel;
            let asset = text(v.get("asset"));
            let name = text(v.get("name"));
            let mask_asset = text(v.get("maskAsset"));
            if !mask_asset.is_empty() && asset.is_empty() {
                return Err("Flash geometric mask has no raster character".into());
            }
            let clip_id = number(v, "clipId")?;
            let button_id = number(v, "buttonId")?;
            let field_id = number(v, "fieldId")?;
            if asset.is_empty()
                && name.is_empty()
                && clip_id == 0
                && button_id == 0
                && field_id == 0
            {
                return Err("Flash layer has no raster or live character".into());
            }
            if [
                clip_id != 0,
                button_id != 0,
                field_id != 0,
                !asset.is_empty(),
            ]
            .iter()
            .filter(|&&x| x)
            .count()
                > 1
            {
                return Err("Flash layer has multiple character identities".into());
            }
            let value = Spec {
                x: number(v, "x")?,
                y: number(v, "y")?,
                width: number(v, "width")?,
                height: number(v, "height")?,
                ink: 8,
                blend: 100,
                kind: 16,
                stretch: 1,
                loc_z: channel as i16,
                flags: 48,
                fore: 1,
                back_rgb: 0xffffff,
                ..Spec::default()
            };
            if value.width <= 0 || value.height <= 0 {
                return Err("empty Flash layer bounds".into());
            }
            let affine = v["affine"].as_bool() == Some(true);
            let mut matrix = [0; 6];
            if affine {
                let a = v["matrix"]
                    .as_array()
                    .filter(|a| a.len() == 6)
                    .ok_or("Flash affine layer without matrix")?;
                for (o, v) in matrix.iter_mut().zip(a) {
                    *o = fixed16(Some(v))?;
                }
            }
            let cxform = if let Some(a) = v.get("cxform") {
                let a = a
                    .as_array()
                    .filter(|a| a.len() == 8)
                    .ok_or("invalid Flash color transform")?;
                let mut out = [0; 8];
                for (o, v) in out.iter_mut().zip(a) {
                    *o = i16::try_from(int(Some(v)))
                        .map_err(|_| "Flash color transform outside range")?;
                }
                Some(out)
            } else {
                None
            };
            let clip_depth: u16 = number(v, "clipDepth")?;
            if clip_depth != 0 && (clip_depth <= channel || clip_depth >= 1000) {
                return Err("invalid Flash mask depth".into());
            }
            Ok(FilmLayer {
                channel,
                value,
                asset,
                mask_asset,
                name,
                origin_x: number(v, "originX")?,
                origin_y: number(v, "originY")?,
                affine,
                matrix,
                clip_id,
                button_id,
                field_id,
                clip_depth,
                cxform,
                placement_generation: number(v, "placementGeneration")?,
                actions: actions(v, 0)?,
                ..FilmLayer::default()
            })
        })
        .collect()
}
pub fn read(v: &Value, root_frames: usize, fields: &[FlashField]) -> Result<Runtime, String> {
    if v.is_null() {
        return Ok(Runtime::default());
    }
    if !v.is_object() {
        return Err("Flash runtime metadata is not an object".into());
    }
    let mut out = Runtime::default();
    let mut ids = HashSet::new();
    for f in fields {
        if f.id != 0 && !ids.insert(f.id) {
            return Err("duplicate Flash field identity".into());
        }
    }
    for c in list(v, "clips", 4096)? {
        let id = number(c, "id")?;
        if !ids.insert(id) {
            return Err("duplicate Flash character identity".into());
        }
        let count: u16 = number(c, "frameCount")?;
        if count == 0 || count > 4096 || (id == 0 && count as usize != root_frames) {
            return Err("invalid Flash clip frame count".into());
        }
        let frames = list(c, "frames", 4096)?
            .iter()
            .map(layers)
            .collect::<Result<Vec<_>, _>>()?;
        if frames.len() != if id == 0 { 0 } else { count as usize } {
            return Err("Flash clip frame count disagrees".into());
        }
        let mut labels = Vec::new();
        for l in list(c, "labels", 65535)? {
            let frame: u16 = number(l, "frame")?;
            if frame == 0 || frame > count {
                return Err("Flash label outside clip".into());
            }
            labels.push((text(l.get("name")), frame));
        }
        let mut sounds = Vec::new();
        for s in list(c, "soundEvents", 65535)? {
            let frame: u16 = number(s, "frame")?;
            let flags: u8 = number(s, "flags")?;
            if frame == 0 || frame > count || flags & 0xc0 != 0 {
                return Err("invalid Flash sound event".into());
            }
            let envelopes = list(s, "envelopes", 255)?
                .iter()
                .map(|e| {
                    let a = e
                        .as_array()
                        .filter(|a| a.len() == 3)
                        .ok_or("invalid Flash sound envelope")?;
                    let position = narrow(int(a.first()), "Flash sound position")?;
                    let left: u16 = narrow(int(a.get(1)), "Flash sound left")?;
                    let right: u16 = narrow(int(a.get(2)), "Flash sound right")?;
                    if left > 32768 || right > 32768 {
                        return Err("Flash envelope exceeds unity".into());
                    }
                    Ok((position, left, right))
                })
                .collect::<Result<Vec<_>, String>>()?;
            if envelopes.windows(2).any(|p| p[0].0 >= p[1].0) {
                return Err("Flash envelope positions are not increasing".into());
            }
            sounds.push(SoundEvent {
                frame,
                flags,
                sound: number(s, "sound")?,
                loops: number(s, "loops")?,
                in_point: number(s, "inPoint")?,
                out_point: number(s, "outPoint")?,
                envelopes,
            });
        }
        out.clips.push(Clip {
            id,
            frame_count: count,
            local_bounds: rectangle(c)?,
            frames,
            actions: actions(c, count as usize)?,
            labels,
            sounds,
        });
    }
    if !out.clips.iter().any(|c| c.id == 0) {
        return Err("Flash runtime has no root clip".into());
    }
    for b in list(v, "buttons", 4096)? {
        let id = number(b, "id")?;
        if id == 0 || !ids.insert(id) {
            return Err("duplicate Flash character identity".into());
        }
        let states = list(b, "states", 4)?
            .iter()
            .map(layers)
            .collect::<Result<Vec<_>, _>>()?;
        if states.len() != 4 {
            return Err("Flash button requires four states".into());
        }
        out.buttons.push(Button {
            id,
            track_as_menu: b["trackAsMenu"].as_bool().unwrap_or(false),
            local_bounds: rectangle(b)?,
            states,
            actions: actions(b, 0)?,
        });
    }
    for s in list(v, "sounds", 4096)? {
        let id = number(s, "id")?;
        if id == 0 || !ids.insert(id) {
            return Err("duplicate Flash character identity".into());
        }
        let sound = Sound {
            id,
            asset: text(s.get("asset")),
            samples: number(s, "samples")?,
            rate: number(s, "rate")?,
        };
        if sound.asset.is_empty() || sound.samples == 0 || sound.rate == 0 {
            return Err("Flash sound has no decoded samples".into());
        }
        out.sounds.push(sound);
    }
    for e in list(v, "exports", 4096)? {
        let id = number(e, "id")?;
        let name = text(e.get("name"));
        if !ids.contains(&id) || name.is_empty() {
            return Err("Flash export references an absent character".into());
        }
        out.exports.push((id, name));
    }
    for init in list(v, "initializers", 4096)? {
        let id = number(init, "clipId")?;
        if id == 0 || !out.clips.iter().any(|c| c.id == id) {
            return Err("Flash initializer references absent clip".into());
        }
        let a = actions(&serde_json::json!({"actions":[init]}), root_frames)?.remove(0);
        if a.frame == 0 || a.event != 0 || a.key != 0 {
            return Err("invalid Flash initializer frame/event".into());
        }
        out.initializers.push((id, a));
    }
    for c in &out.clips {
        for sound in &c.sounds {
            if !out.sounds.iter().any(|s| s.id == sound.sound) {
                return Err("Flash sound event references absent sound".into());
            }
        }
    }
    for layer in out
        .clips
        .iter()
        .flat_map(|c| &c.frames)
        .chain(out.buttons.iter().flat_map(|b| &b.states))
        .flatten()
    {
        validate_layer(layer, &out, fields)?;
    }
    Ok(out)
}
pub fn validate_layer(l: &FilmLayer, r: &Runtime, fields: &[FlashField]) -> Result<(), String> {
    if !r.clips.is_empty() && !l.asset.is_empty() && l.mask_asset.is_empty() {
        return Err("retained Flash raster has no geometric mask asset".into());
    }
    if (l.clip_id != 0 && !r.clips.iter().any(|c| c.id == l.clip_id))
        || (l.button_id != 0 && !r.buttons.iter().any(|b| b.id == l.button_id))
        || (l.field_id != 0 && !fields.iter().any(|f| f.id == l.field_id))
    {
        return Err("Flash layer references absent definition".into());
    }
    Ok(())
}

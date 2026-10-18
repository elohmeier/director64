//! Retained Flash character timelines for the shared renderer and AVM1 VM.
//! Complex movies keep definition identity, placement lifetimes, masks and
//! action blocks instead of baking interactive child timelines into pixels.
use super::*;

#[derive(Default)]
struct Clip {
    frames: Vec<Depths>,
    actions: Vec<Value>,
    labels: Vec<Value>,
    sounds: Vec<Value>,
}
struct Button {
    states: [Depths; 4],
    actions: Vec<Value>,
    menu: bool,
}
pub(super) struct Converted {
    pub clips: Vec<Value>,
    pub buttons: Vec<Value>,
    pub exports: Vec<Value>,
    pub initializers: Vec<Value>,
    pub sounds: Vec<(usize, crate::convert::d5media::Sound)>,
    pub fields: Vec<Value>,
    pub layers: Vec<Value>,
    pub width: usize,
    pub height: usize,
    pub rate: f64,
    pub frames: usize,
    pub audio: Option<audio::Audio>,
}

pub(super) fn required(b: &[u8]) -> R<bool> {
    if b.len() < 8 || &b[..3] != b"FWS" {
        return Ok(false);
    }
    let mut bits = Bits::new(b, 8);
    bits.rect()?;
    let (tags, _) = tag_stream(b, bits.align() + 4)?;
    fn retained(tags: &[Tag<'_>], depth: usize) -> R<bool> {
        if depth > 16 {
            return Err("excessive SWF definition nesting".into());
        }
        for t in tags {
            if matches!(t.tag, 14 | 34 | 56 | 59)
                || (matches!(t.tag, 26 | 70) && byte(t.payload, 0)? & 128 != 0)
            {
                return Ok(true);
            }
            if t.tag == 39 {
                let (children, _) = tag_stream(t.payload, 4)?;
                if retained(&children, depth + 1)? {
                    return Ok(true);
                }
            }
        }
        Ok(false)
    }
    retained(&tags, 0)
}

fn action(frame: usize, event: u32, bytes: &[u8]) -> R<Value> {
    if bytes.is_empty() || bytes.len() > 1024 * 1024 || bytes.last() != Some(&0) {
        return Err("invalid retained SWF action block".into());
    }
    crate::scene::flash::action_bytes(bytes)?;
    Ok(json!({"frame":frame,"event":event,"code":bytes}))
}
fn sound_event(frame: usize, b: &[u8]) -> R<Value> {
    let id = u16le(b, 0)?;
    let flags = byte(b, 2)?;
    if flags & 0xc0 != 0 {
        return Err("invalid SWF sound-info reserved flags".into());
    }
    let mut p = 3;
    let mut point = |flag: u8| -> R<u32> {
        if flags & flag != 0 {
            let n = u32le(b, p)? as u32;
            p += 4;
            Ok(n)
        } else {
            Ok(0)
        }
    };
    let start = point(1)?;
    let end = point(2)?;
    let loops = if flags & 4 != 0 {
        let n = u16le(b, p)?;
        p += 2;
        n
    } else {
        1
    };
    let mut envelopes = Vec::new();
    if flags & 8 != 0 {
        let count = byte(b, p)?;
        p += 1;
        for _ in 0..count {
            let position = u32le(b, p)?;
            let left = u16le(b, p + 4)?;
            let right = u16le(b, p + 6)?;
            p += 8;
            if left > 32768 || right > 32768 {
                return Err("SWF sound envelope level exceeds unity".into());
            }
            if envelopes
                .last()
                .is_some_and(|v: &Value| v[0].as_u64().unwrap_or(0) >= position as u64)
            {
                return Err("SWF sound envelope positions are not increasing".into());
            }
            envelopes.push(json!([position, left, right]));
        }
    }
    if p != b.len() {
        return Err("trailing SWF sound-info bytes".into());
    }
    Ok(
        json!({"frame":frame,"sound":id,"flags":flags,"inPoint":start,"outPoint":end,"loops":loops,"envelopes":envelopes}),
    )
}
fn placement_actions(b: &[u8], version: u8) -> R<Vec<Value>> {
    if b.is_empty() {
        return Ok(Vec::new());
    }
    if u16le(b, 0)? != 0 {
        return Err("invalid SWF clip-action reserved flags".into());
    }
    let width = if version >= 6 { 4 } else { 2 };
    let flags = |p: usize| -> R<u32> {
        Ok(if width == 4 {
            u32le(b, p)? as u32
        } else {
            u16le(b, p)? as u32
        })
    };
    let all = flags(2)?;
    let mut p = 2 + width;
    let mut seen = 0u32;
    let mut out = Vec::new();
    loop {
        let events = flags(p)?;
        p += width;
        if events == 0 {
            break;
        }
        let length = u32le(b, p)?;
        p += 4;
        let end = p
            .checked_add(length)
            .filter(|&n| n <= b.len())
            .ok_or("truncated SWF clip action")?;
        let key = if events & 0x20000 != 0 {
            let key = byte(b, p)?;
            p += 1;
            key
        } else {
            0
        };
        let mut record = action(0, events, &b[p..end])?;
        record["key"] = json!(key);
        out.push(record);
        seen |= events;
        p = end;
    }
    if p != b.len() || seen != all {
        return Err("SWF clip action mask or length disagrees".into());
    }
    Ok(out)
}
fn button(b: &[u8], effects: &mut Effects) -> R<(usize, Button)> {
    let id = u16le(b, 0)?;
    let flags = byte(b, 2)?;
    let offset = u16le(b, 3)?;
    if flags > 1 {
        return Err("invalid SWF button flags".into());
    }
    let mut states: [Depths; 4] = Default::default();
    let mut p = 5;
    loop {
        let state = byte(b, p)?;
        p += 1;
        if state == 0 {
            break;
        }
        if state & 0xc0 != 0 {
            return Err("invalid SWF button state flags".into());
        }
        let character = u16le(b, p)?;
        let depth = u16le(b, p + 2)?;
        p += 4;
        let mut bits = Bits::new(b, p);
        let matrix = bits.matrix()?;
        let cxform = Some(read_color_transform(&mut bits)?);
        p = bits.align();
        let filters = if state & 16 != 0 {
            let values = filters::read_list(b, &mut p)?;
            effects.applied_filters += values.len() as u32;
            values
        } else {
            Vec::new()
        };
        let blend_mode = if state & 32 != 0 {
            let mode = byte(b, p)?;
            p += 1;
            mode
        } else {
            0
        };
        for (n, list) in states.iter_mut().enumerate() {
            if state & (1 << n) != 0 {
                if list.get(depth).is_some() {
                    return Err("duplicate SWF button-state depth".into());
                }
                list.insert(
                    depth,
                    Placement {
                        id: character,
                        ratio: 0,
                        filters: filters.clone(),
                        matrix,
                        cxform: cxform.clone(),
                        clip: None,
                        name: None,
                        placed: Some(0),
                        generation: 0,
                        clip_actions: Vec::new(),
                        blend_mode,
                    },
                );
            }
        }
    }
    let mut actions = Vec::new();
    if offset != 0 {
        if p != 3 + offset {
            return Err("SWF button action offset disagrees".into());
        }
        loop {
            let size = u16le(b, p)?;
            let end = if size == 0 {
                b.len()
            } else {
                p.checked_add(size)
                    .filter(|&n| n <= b.len())
                    .ok_or("truncated SWF button action")?
            };
            let events = u16le(b, p + 2)? as u32;
            p += 4;
            if events == 0 || p >= end {
                return Err("empty SWF button condition".into());
            }
            actions.push(action(0, events, &b[p..end])?);
            p = end;
            if size == 0 {
                break;
            }
        }
    }
    if p != b.len() {
        return Err("trailing SWF button bytes".into());
    }
    Ok((
        id,
        Button {
            states,
            actions,
            menu: flags != 0,
        },
    ))
}
fn clip(tags: &[Tag<'_>], declared: usize, effects: &mut Effects) -> R<Clip> {
    let mut result = Clip::default();
    let mut depths = Depths::default();
    for &Tag { tag, payload } in tags {
        match tag {
            4|26|70=>read_placement(tag,payload,&mut depths,effects)?,
            5|28=>{let depth=u16le(payload,if tag==5 {2} else {0})?;
                if !depths.remove(depth) {return Err("SWF removal references empty depth".into());}},
            1=>{
                if !payload.is_empty() || result.frames.len()>=declared {return Err("SWF runtime frame count disagrees".into());}
                for (_,entry) in &mut depths.0 {if entry.placed.is_none() {entry.placed=Some(result.frames.len());}}
                result.frames.push(depths.clone());
            }
            12=>result.actions.push(action(result.frames.len()+1,0,payload)?),
            15=>result.sounds.push(sound_event(result.frames.len()+1,payload)?),
            43=>result.labels.push(json!({"frame":result.frames.len()+1,"name":js::latin1(payload).trim_end_matches('\0')})),
            0=>{},
            // Definitions, exports and initialization programs belong to the
            // outer dictionary and are handled before constructing frames.
            2|6|8|9|10|11|13|14|18|19|20|21|22|24|32|33|34|35|36|37|39|45|46|48|56|59|60|61|69|73|74|75|77|83|88=>{},
            _=>return Err(format!("unsupported retained SWF timeline tag {tag}")),
        }
    }
    if result.frames.len() != declared || declared == 0 || declared > 4096 {
        return Err("SWF runtime frame count disagrees".into());
    }
    Ok(result)
}

fn transformed(bounds: [f64; 4], m: Matrix) -> [f64; 4] {
    let mut out = [
        f64::INFINITY,
        f64::INFINITY,
        f64::NEG_INFINITY,
        f64::NEG_INFINITY,
    ];
    for (x, y) in [
        (bounds[0], bounds[1]),
        (bounds[2], bounds[1]),
        (bounds[2], bounds[3]),
        (bounds[0], bounds[3]),
    ] {
        let (x, y) = (m.a * x + m.c * y + m.tx, m.b * x + m.d * y + m.ty);
        out[0] = out[0].min(x);
        out[1] = out[1].min(y);
        out[2] = out[2].max(x);
        out[3] = out[3].max(y);
    }
    out
}
fn union(out: &mut [f64; 4], b: [f64; 4]) {
    out[0] = out[0].min(b[0]);
    out[1] = out[1].min(b[1]);
    out[2] = out[2].max(b[2]);
    out[3] = out[3].max(b[3]);
}
fn rect_json(b: [f64; 4]) -> Value {
    json!([
        b[0] / 20.0,
        b[1] / 20.0,
        (b[2] - b[0]) / 20.0,
        (b[3] - b[1]) / 20.0
    ])
}
fn bounds(
    id: usize,
    base: &HashMap<usize, [f64; 4]>,
    clips: &HashMap<usize, Clip>,
    buttons: &HashMap<usize, Button>,
    path: &mut Vec<usize>,
) -> R<[f64; 4]> {
    if let Some(b) = base.get(&id) {
        return Ok(*b);
    }
    if path.len() >= 16 || path.contains(&id) {
        return Err("recursive or excessive SWF character nesting".into());
    }
    path.push(id);
    let mut out = [
        f64::INFINITY,
        f64::INFINITY,
        f64::NEG_INFINITY,
        f64::NEG_INFINITY,
    ];
    let frames: Vec<&Depths> = if let Some(c) = clips.get(&id) {
        c.frames.iter().collect()
    } else if let Some(b) = buttons.get(&id) {
        b.states.iter().collect()
    } else {
        return Err(format!("SWF placement references absent character {id}"));
    };
    for frame in frames {
        for (_, p) in &frame.0 {
            union(
                &mut out,
                transformed(bounds(p.id, base, clips, buttons, path)?, p.matrix),
            );
        }
    }
    path.pop();
    if !out[0].is_finite() {
        out = [0.0; 4];
    }
    if out
        .iter()
        .any(|n| !n.is_finite() || n.abs() > 32767.0 * 20.0)
    {
        return Err("SWF character geometry exceeds fixed-point range".into());
    }
    Ok(out)
}

struct Layers<'a, 'b> {
    dictionary: &'a Dictionary,
    clips: &'a HashMap<usize, Clip>,
    buttons: &'a HashMap<usize, Button>,
    bounds: &'a HashMap<usize, [f64; 4]>,
    version: u8,
    sink: &'b mut LayerSink<'a>,
}
impl Layers<'_, '_> {
    fn frame(&mut self, frame: &Depths) -> R<Value> {
        let mut layers = Vec::new();
        for (depth, entry) in frame.sorted() {
            if depth == 0
                || depth >= 1000
                || entry.clip.is_some_and(|end| end < depth || end >= 1000)
            {
                return Err("SWF runtime layer depth exceeds profile".into());
            }
            if entry.blend_mode > 1 {
                return Err(format!(
                    "unsupported live SWF blend mode {}",
                    entry.blend_mode
                ));
            }
            let character = self
                .bounds
                .get(&entry.id)
                .copied()
                .ok_or("SWF runtime character has no bounds")?;
            let is_clip = self.clips.contains_key(&entry.id);
            let is_button = self.buttons.contains_key(&entry.id);
            let field = self.dictionary.fields.get(&entry.id);
            let dynamic = is_clip || is_button || field.is_some();
            if entry.filters.iter().any(|f| !f.identity()) {
                return Err("SWF live character filtering requires runtime filters".into());
            }
            let transformed = character;
            let left = (transformed[0] / 20.0).floor();
            let top = (transformed[1] / 20.0).floor();
            let width = ((transformed[2] / 20.0).ceil() - left).max(1.0) as usize;
            let height = ((transformed[3] / 20.0).ceil() - top).max(1.0) as usize;
            let mut layer = json!({"channel":depth,"name":entry.name.as_deref().unwrap_or(""),
                "x":left as i32,"y":top as i32,"width":width,"height":height,
                "originX":(entry.matrix.tx/20.0*65536.0).round() as i32,"originY":(entry.matrix.ty/20.0*65536.0).round() as i32,
                "placementGeneration":entry.generation,"clipDepth":entry.clip.unwrap_or(0),
                "actions":placement_actions(&entry.clip_actions,self.version)?});
            {
                layer["affine"] = json!(true);
                layer["matrix"] = json!([
                    entry.matrix.a,
                    entry.matrix.b,
                    entry.matrix.c,
                    entry.matrix.d,
                    entry.matrix.tx / 20.0,
                    entry.matrix.ty / 20.0
                ]);
                if is_clip {
                    layer["clipId"] = json!(entry.id);
                }
                if is_button {
                    layer["buttonId"] = json!(entry.id);
                }
                if field.is_some() {
                    layer["fieldId"] = json!(entry.id);
                }
                if let Some(c) = &entry.cxform {
                    layer["cxform"] = json!(
                        c.mult
                            .iter()
                            .chain(c.add.iter())
                            .map(|n| *n as i16)
                            .collect::<Vec<_>>()
                    );
                }
            }
            if !dynamic {
                if width
                    .checked_mul(height)
                    .is_none_or(|n| n > 16 * 1024 * 1024)
                {
                    return Err("SWF runtime raster character exceeds budget".into());
                }
                let mut rgba = vec![0; width * height * 4];
                let renderer = Renderer {
                    geometric_mask: false,
                    dictionary: self.dictionary,
                    canvas: Canvas {
                        width,
                        height,
                        origin_x: left * 20.0,
                        origin_y: top * 20.0,
                    },
                    variant: 0,
                };
                let mut local = entry.clone();
                local.matrix = Matrix {
                    a: 1.0,
                    b: 0.0,
                    c: 0.0,
                    d: 1.0,
                    tx: 0.0,
                    ty: 0.0,
                };
                local.cxform = None;
                local.clip = None;
                local.filters.clear();
                renderer.render_entry(&mut rgba, &mut local, 0, &None, &[], false)?;
                let mut mask = vec![0; width * height * 4];
                renderer.render_mask_entry(&mut mask, &mut local, 0, false)?;
                let same = rgba
                    .chunks_exact(4)
                    .zip(mask.chunks_exact(4))
                    .all(|(a, b)| a[3] == b[3]);
                let asset = (self.sink)(&RasterLayer {
                    width,
                    height,
                    rgba,
                })?;
                let mask_asset = if same {
                    asset.clone()
                } else {
                    (self.sink)(&RasterLayer {
                        width,
                        height,
                        rgba: mask,
                    })?
                };
                layer["asset"] = json!(asset);
                layer["maskAsset"] = json!(mask_asset);
            }
            layers.push(layer);
        }
        Ok(json!(layers))
    }
}

pub(super) fn convert<'a>(b: &[u8], sink: &mut LayerSink<'a>) -> R<Converted> {
    if b.len() < 8 || &b[..3] != b"FWS" || u32le(b, 4)? != b.len() {
        return Err("unsupported runtime SWF envelope".into());
    }
    let mut bits = Bits::new(b, 8);
    let stage = bits.rect()?;
    let p = bits.align();
    let rate = u16le(b, p)? as f64 / 256.0;
    let declared = u16le(b, p + 2)?;
    let width = (stage[1] - stage[0]) / 20.0;
    let height = (stage[3] - stage[2]) / 20.0;
    if width <= 0.0
        || height <= 0.0
        || width.fract() != 0.0
        || height.fract() != 0.0
        || width > 2048.0
        || height > 2048.0
    {
        return Err("invalid runtime SWF stage bounds".into());
    }
    let (tags, end) = tag_stream(b, p + 4)?;
    if end != b.len() || !tags.last().is_some_and(|t| t.tag == 0) {
        return Err("unterminated runtime SWF".into());
    }
    let mut dictionary = Dictionary::default();
    let mut clips = HashMap::new();
    let mut buttons = HashMap::new();
    let mut base = HashMap::new();
    let mut sounds = Vec::new();
    let mut exports = Vec::new();
    let mut initializers = Vec::new();
    let mut identities = std::collections::HashSet::new();
    let mut jpeg_tables = None;
    let mut effects = Effects::default();
    let mut stream = None;
    let mut frame = 0;
    for &Tag { tag, payload } in &tags {
        if matches!(
            tag,
            2 | 6
                | 10
                | 11
                | 14
                | 20
                | 21
                | 22
                | 32
                | 33
                | 34
                | 35
                | 36
                | 37
                | 39
                | 46
                | 48
                | 60
                | 75
                | 83
        ) {
            let id = u16le(payload, 0)?;
            if id == 0 || !identities.insert(id) {
                return Err("duplicate or zero SWF definition identity".into());
            }
        }
        if let Some(version) = shape_version(tag) {
            let (id, shape) = parse_shape(payload, version, &dictionary.bitmaps)?;
            dictionary.shapes.insert(id, shape);
            let r = Bits::new(payload, 2).rect()?;
            base.insert(id, [r[0], r[2], r[1], r[3]]);
        } else {
            match tag {
                8 => jpeg_tables = Some(payload),
                6 | 21 | 35 | 20 | 36 => {
                    let id = u16le(payload, 0)?;
                    dictionary.bitmaps.insert(
                        id,
                        std::rc::Rc::new(parse_bitmap(tag, payload, jpeg_tables)?),
                    );
                }
                46 => {
                    let (id, m) = morph::parse(payload, &dictionary.bitmaps)?;
                    dictionary.morphs.insert(id, m);
                    let mut bits = Bits::new(payload, 2);
                    let a = bits.rect()?;
                    let b = bits.rect()?;
                    base.insert(
                        id,
                        [
                            a[0].min(b[0]),
                            a[2].min(b[2]),
                            a[1].max(b[1]),
                            a[3].max(b[3]),
                        ],
                    );
                }
                10 | 48 | 75 => {
                    let (id, font) = parse_font(payload, tag)?;
                    dictionary.fonts.insert(id, font);
                }
                13 => {
                    let id = u16le(payload, 0)?;
                    let font = dictionary
                        .fonts
                        .get_mut(&id)
                        .ok_or("SWF font info without definition")?;
                    let length = byte(payload, 2)? as usize;
                    let wide = byte(payload, 3 + length)? & 1 != 0;
                    let mut p = 4 + length;
                    let mut codes = Vec::new();
                    while p < payload.len() {
                        codes.push(if wide {
                            u16le(payload, p)?
                        } else {
                            byte(payload, p)? as usize
                        });
                        p += if wide { 2 } else { 1 };
                    }
                    font.codes = Some(codes);
                }
                11 | 33 => {
                    let (id, text) = parse_define_text(payload, if tag == 33 { 2 } else { 1 })?;
                    dictionary.texts.insert(id, text);
                    let r = Bits::new(payload, 2).rect()?;
                    base.insert(id, [r[0], r[2], r[1], r[3]]);
                }
                37 => {
                    let (id, f) = parse_edit_text(payload, b[3] >= 6)?;
                    base.insert(id, [f.bounds[0], f.bounds[2], f.bounds[1], f.bounds[3]]);
                    dictionary.fields.insert(id, f);
                }
                39 => {
                    let id = u16le(payload, 0)?;
                    let (tags, end) = tag_stream(payload, 4)?;
                    if end != payload.len() {
                        return Err("trailing SWF clip bytes".into());
                    }
                    clips.insert(id, clip(&tags, u16le(payload, 2)?, &mut effects)?);
                }
                34 => {
                    let (id, button) = button(payload, &mut effects)?;
                    buttons.insert(id, button);
                }
                14 => sounds.push(audio::event(payload)?),
                59 => {
                    let mut init = action(frame + 1, 0, &payload[2..])?;
                    init["clipId"] = json!(u16le(payload, 0)?);
                    initializers.push(init);
                }
                56 => {
                    let count = u16le(payload, 0)?;
                    let mut p = 2;
                    for _ in 0..count {
                        let id = u16le(payload, p)?;
                        p += 2;
                        let end = index_of(payload, 0, p).ok_or("truncated SWF export name")?;
                        exports.push(json!({"id":id,"name":js::latin1(&payload[p..end])}));
                        p = end + 1;
                    }
                    if p != payload.len() {
                        return Err("trailing SWF export bytes".into());
                    }
                }
                60 => {
                    let (id, video) = Video::new(payload)?;
                    base.insert(
                        id,
                        [
                            0.0,
                            0.0,
                            video.width as f64 * 20.0,
                            video.height as f64 * 20.0,
                        ],
                    );
                    dictionary.videos.insert(id, video);
                }
                61 => dictionary
                    .videos
                    .get_mut(&u16le(payload, 0)?)
                    .ok_or("SWF video packet without definition")?
                    .packet(payload)?,
                18 | 45 => {
                    if stream.is_some() {
                        return Err("multiple runtime SWF streams".into());
                    }
                    stream = Some(audio::Stream::new(payload)?);
                }
                19 => stream
                    .as_mut()
                    .ok_or("SWF stream block without header")?
                    .block(frame, payload)?,
                1 => frame += 1,
                _ => {}
            }
        }
    }
    if dictionary
        .videos
        .values()
        .any(|v| v.frames.len() != v.count)
    {
        return Err("SWF video frame count disagrees".into());
    }
    let mut root = clip(&tags, declared, &mut effects)?;
    for frame in &mut root.frames {
        for (_, p) in &mut frame.0 {
            p.matrix.tx -= stage[0];
            p.matrix.ty -= stage[2];
        }
    }
    clips.insert(0, root);
    base.insert(0, [0.0, 0.0, width * 20.0, height * 20.0]);
    for init in &initializers {
        if !clips.contains_key(&(init["clipId"].as_u64().unwrap() as usize)) {
            return Err("SWF initializer references non-clip definition".into());
        }
    }
    for export in &exports {
        if !identities.contains(&(export["id"].as_u64().unwrap() as usize)) {
            return Err("SWF export references missing definition".into());
        }
    }
    let mut all = base.clone();
    for id in clips.keys().chain(buttons.keys()) {
        all.insert(*id, bounds(*id, &base, &clips, &buttons, &mut Vec::new())?);
    }
    let mut builder = Layers {
        dictionary: &dictionary,
        clips: &clips,
        buttons: &buttons,
        bounds: &all,
        version: b[3],
        sink,
    };
    let mut clip_ids: Vec<_> = clips.keys().copied().collect();
    clip_ids.sort_unstable();
    let mut button_ids: Vec<_> = buttons.keys().copied().collect();
    button_ids.sort_unstable();
    let mut clip_values = Vec::new();
    let mut root_layers = Vec::new();
    let mut button_values = Vec::new();
    for id in clip_ids {
        let c = &clips[&id];
        let mut frames = Vec::new();
        for f in &c.frames {
            frames.push(builder.frame(f)?);
        }
        if id == 0 {
            root_layers = frames;
            frames = Vec::new();
        }
        clip_values.push(
            json!({"id":id,"frameCount":c.frames.len(),"frames":frames,"actions":c.actions,
            "labels":c.labels,"soundEvents":c.sounds,"localBounds":rect_json(all[&id])}),
        );
    }
    for id in button_ids {
        let b = &buttons[&id];
        let mut states = Vec::new();
        for state in &b.states {
            states.push(builder.frame(state)?);
        }
        button_values.push(json!({"id":id,"states":states,"actions":b.actions,"trackAsMenu":b.menu,"localBounds":rect_json(all[&id])}));
    }
    let mut field_ids: Vec<_> = dictionary.fields.keys().copied().collect();
    field_ids.sort_unstable();
    let mut fields = Vec::new();
    for id in field_ids {
        let f = &dictionary.fields[&id];
        let font = f.font.and_then(|id| dictionary.fonts.get(&id));
        fields.push(json!({"id":id,"name":format!("@field_{id}"),"variable":f.variable,"text":f.text,"html":f.html,
            "fontName":font.map(|f|f.name.clone()),"fontHeight":f.font_height/20.0,"color":f.color,
            "align":f.align,"wordWrap":f.word_wrap,"multiline":f.multiline,"embedFonts":f.embed_fonts,"leading":f.leading/20.0,
            "leftMargin":f.left_margin/20.0,"rightMargin":f.right_margin/20.0,"indent":f.indent/20.0,
            "bounds":{"left":f.bounds[0]/20.0,"top":f.bounds[2]/20.0,"width":(f.bounds[1]-f.bounds[0])/20.0,"height":(f.bounds[3]-f.bounds[2])/20.0},
            "localBounds":{"left":f.bounds[0]/20.0,"top":f.bounds[2]/20.0,"width":(f.bounds[1]-f.bounds[0])/20.0,"height":(f.bounds[3]-f.bounds[2])/20.0}}));
    }
    let audio = match stream {
        Some(s) => s.finish(declared)?,
        None => None,
    };
    Ok(Converted {
        clips: clip_values,
        buttons: button_values,
        exports,
        initializers,
        sounds,
        fields,
        layers: root_layers,
        width: width as usize,
        height: height as usize,
        rate,
        frames: declared,
        audio,
    })
}

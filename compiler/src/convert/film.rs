//! Film-loop flattening (tools/director/film-model.mjs): child sprites
//! composed onto the loop's bounds, uncovered pixels left transparent.

use serde_json::{json, Map, Value};

use super::fdi::{fdi_composite, fdi_image, fdi_pixel, FdiMeta};
use super::js;

fn shape_inset(kind: i64, w: f64, h: f64, y: f64) -> f64 {
    if kind == 1 {
        return 0.0;
    }
    let (mut rx, mut ry) = (w * 0.5, h * 0.5);
    if kind == 2 {
        if rx > 12.0 {
            rx = 12.0;
        }
        if ry > 12.0 {
            ry = 12.0;
        }
        if y >= ry && y < h - ry {
            return 0.0;
        }
    }
    let dy = if y < h / 2.0 { ry - y - 0.5 } else { y + 0.5 - (h - ry) };
    let square = 1.0 - (dy / ry).powi(2);
    (rx * (1.0 - (if square > 0.0 { square } else { 0.0 }).sqrt()) - 0.5).ceil()
}

/// Row spans mirroring runtime/director/shape.c.
pub fn shape_spans(kind: i64, w: i64, h: i64, y: i64, line: i64, filled: bool, reverse: bool) -> Vec<(i64, i64)> {
    if w <= 0 || h <= 0 || y < 0 || y >= h || !(1..=4).contains(&kind) {
        return vec![];
    }
    if kind == 4 {
        if line == 0 {
            return vec![];
        }
        let mut a = (y * w / h) - line / 2;
        let mut b = ((y + 1) * w / h) + 1 + (line - 1) / 2;
        if a < 0 {
            a = 0;
        }
        if b > w {
            b = w;
        }
        return vec![if reverse { (w - b, w - a) } else { (a, b) }];
    }
    if !filled && line == 0 {
        return vec![];
    }
    let outer = shape_inset(kind, w as f64, h as f64, y as f64).max(0.0) as i64;
    if filled || line * 2 >= w || line * 2 >= h || y < line || y >= h - line {
        return vec![(outer, w - outer)];
    }
    let inner = outer.max(line + shape_inset(kind, (w - line * 2) as f64, (h - line * 2) as f64, (y - line) as f64) as i64);
    vec![(outer, inner), (w - inner, w - outer)]
}

fn int(v: &Value) -> i64 {
    v.as_i64().or_else(|| v.as_f64().map(|f| f as i64)).unwrap_or(0)
}
fn be16(b: &[u8], at: usize) -> i64 {
    u16::from_be_bytes([b[at], b[at + 1]]) as i64
}
fn bei16(b: &[u8], at: usize) -> i64 {
    i16::from_be_bytes([b[at], b[at + 1]]) as i64
}

pub struct Composed {
    pub data: Vec<u8>,
    pub sounds: Value,
}

/// One flattened frame of a film loop. `states` are the channels' latest
/// records, composed in channel order; `load` reads a converted image asset.
pub fn compose_film_frame(
    movie: &Value,
    loop_member: &Value,
    states: &[(i64, Value)],
    load: &mut dyn FnMut(&str) -> Result<Vec<u8>, String>,
    movies: &[Value],
    limits: &mut Vec<Value>,
) -> Result<Composed, String> {
    let (lw, lh) = (int(&loop_member["width"]), int(&loop_member["height"]));
    let mut composed = vec![0u8; (lw * lh * 4) as usize];
    let version = int(&loop_member["filmScore"]["version"]);
    if version == 7 {
        for px in composed.chunks_mut(4) {
            px[..3].fill(255);
        }
    }
    let mut sounds = [Value::Null, Value::Null];
    let record = |kind: &str, extra: Vec<(&str, Value)>, limits: &mut Vec<Value>| {
        let mut entry = Map::new();
        entry.insert("kind".into(), json!(kind));
        entry.insert("movie".into(), movie["name"].clone());
        entry.insert("cast".into(), loop_member["cast"].clone());
        entry.insert("member".into(), loop_member["number"].clone());
        for (k, v) in extra {
            entry.insert(k.into(), v);
        }
        entry.insert("original_projector_verified".into(), json!(false));
        let entry = Value::Object(entry);
        let key = js::stringify(&entry);
        if !limits.iter().any(|l| js::stringify(l) == key) {
            limits.push(entry);
        }
    };
    let (loop_cast, loop_left, loop_top) =
        (int(&loop_member["cast"]), int(&loop_member["left"]), int(&loop_member["top"]));
    let mut ordered: Vec<&(i64, Value)> = states.iter().collect();
    ordered.sort_by_key(|s| s.0);
    for (channel, c) in ordered {
        let channel = *channel;
        let b: Vec<u8> = c["bytes"].as_array().ok_or("channel bytes")?.iter().map(|v| int(v) as u8).collect();
        let n = be16(&b, 6);
        let cast = be16(&b, 4);
        if channel < 6 {
            if channel == 3 || channel == 4 {
                let sound_cast = bei16(&b, 0);
                let number = be16(&b, 2);
                let slot = if version == 7 { if channel == 4 { 0 } else { 1 } } else if channel == 3 { 0 } else { 1 };
                if number != 0 {
                    if sound_cast == 0 && version != 7 {
                        record("film-loop-unresolved-sound", vec![("channel", json!(channel)), ("sound", json!(number))], limits);
                    } else {
                        sounds[slot] = json!({"cast": if sound_cast == -1 { loop_cast } else { sound_cast }, "member": number});
                    }
                }
                continue;
            }
            let empty_cast = channel >= 2 && bei16(&b, 0) == -2 && !b[2..].iter().any(|&v| v != 0);
            if !empty_cast && b.iter().any(|&v| v != 0) {
                return Err("film-loop main-channel action".into());
            }
            continue;
        }
        if b[0] == 0 || n == 0 {
            continue;
        }
        if cast == 0 && version != 7 {
            record("film-loop-unresolved-child", vec![("channel", json!(channel)), ("child", json!(n))], limits);
            continue;
        }
        let child_cast = if cast == 65535 { loop_cast } else { cast };
        let mut target = movie;
        let mut target_cast = child_cast;
        let find = |m: &Value, number: i64, cast: i64| -> Option<Value> {
            m["members"].as_array()?.iter().find(|x| int(&x["number"]) == number && int(&x["cast"]) == cast).cloned()
        };
        let mut child = find(movie, n, child_cast);
        if child.is_none() {
            let library = movie["casts"].as_array().and_then(|c| c.get((child_cast - 1) as usize));
            let other = library.filter(|l| l["file"] != movie["name"]).and_then(|l| movies.iter().find(|m| m["name"] == l["file"]));
            target_cast = 1;
            match other {
                Some(m) => {
                    target = m;
                    child = find(m, n, 1);
                }
                None => child = None,
            }
        }
        let Some(child) = child else {
            record("film-loop-absent-child", vec![("channel", json!(channel)), ("cast", json!(child_cast)), ("child", json!(n))], limits);
            continue;
        };
        let opacity = if b[22] & 16 != 0 { 255 - b[21] as i64 } else { 255 };
        let ink = (b[1] & 63) as i64;
        let (cw, ch) = (int(&child["width"]), int(&child["height"]));
        let (width, height) = if b[1] & 128 != 0 { (bei16(&b, 18), bei16(&b, 16)) } else { (cw, ch) };
        if int(&child["type"]) == 8 && version == 13 {
            if opacity == 0 {
                continue;
            }
            let shape = int(&child["shape"]);
            if int(&child["pattern"]) != 1 || !(1..=4).contains(&shape) {
                return Err("film-loop shape pattern".into());
            }
            if ![0, 8].contains(&ink) {
                return Err("film-loop ink".into());
            }
            let left = bei16(&b, 14) - loop_left;
            let top = bei16(&b, 12) - loop_top;
            for y in 0..height {
                if top + y < 0 || top + y >= lh {
                    continue;
                }
                let filled = child["filled"].as_bool().unwrap_or_else(|| int(&child["filled"]) != 0);
                for (a, e) in shape_spans(shape, width, height, y, int(&child["lineWidth"]), filled, int(&child["lineDirection"]) == 6) {
                    for x in a..e {
                        if left + x < 0 || left + x >= lw {
                            continue;
                        }
                        fdi_composite(&mut composed, (((top + y) * lw + left + x) * 4) as usize, [b[2], b[24], b[26], opacity as u8]);
                    }
                }
            }
            continue;
        }
        if int(&child["type"]) != 1 {
            return Err("film-loop non-bitmap/script child".into());
        }
        let allowed = [0, 8, 36].contains(&ink) || (version == 7 && ink == 2) || (version == 13 && ink == 9);
        if !allowed {
            return Err("film-loop ink".into());
        }
        let mut mask: Option<(Vec<u8>, i64, i64)> = None;
        if ink == 9 {
            let member = find(target, n + 1, target_cast);
            let member = member.filter(|m| int(&m["type"]) == 1 && m["asset"].as_str().is_some_and(|a| !a.is_empty()))
                .ok_or("film-loop mask member")?;
            mask = Some((load(member["asset"].as_str().unwrap())?, int(&member["width"]), int(&member["height"])));
        }
        let pixels = load(child["asset"].as_str().unwrap_or(""))?;
        // Math.trunc of the scaled registration.
        let reg_x = (int(&child["regX"]) as f64 * width as f64 / cw as f64).trunc() as i64;
        let reg_y = (int(&child["regY"]) as f64 * height as f64 / ch as f64).trunc() as i64;
        let left = bei16(&b, 14) - reg_x - loop_left;
        let top = bei16(&b, 12) - reg_y - loop_top;
        for y in 0..height {
            for x in 0..width {
                if left + x < 0 || left + x >= lw || top + y < 0 || top + y >= lh {
                    continue;
                }
                let sx = (x as f64 * cw as f64 / width as f64).floor() as i64;
                let sy = (y as f64 * ch as f64 / height as f64).floor() as i64;
                let mut pixel = fdi_pixel(&pixels, (sy * cw + sx) as usize, if ink == 2 || ink == 9 { 0 } else { ink });
                if let Some((mask_pixels, mw, mh)) = &mask {
                    let mut opaque = false;
                    if sx < *mw && sy < *mh {
                        let stencil = fdi_pixel(mask_pixels, (sy * mw + sx) as usize, 8);
                        if stencil[3] != 0
                            && (stencil[0] != 0 || stencil[1] != 0 || stencil[2] != 0)
                            && !(stencil[0] == 255 && stencil[1] == 255 && stencil[2] == 255)
                        {
                            record("film-loop-mask-antialiased", vec![("channel", json!(channel)), ("child", json!(n))], limits);
                        }
                        opaque = stencil[3] > 0 && (stencil[0] as u32 + stencil[1] as u32 + stencil[2] as u32) < 384;
                    }
                    if !opaque {
                        pixel[3] = 0;
                    }
                }
                pixel[3] = js::round(pixel[3] as f64 * opacity as f64 / 255.0) as u8;
                let offset = (((top + y) * lw + left + x) * 4) as usize;
                if ink == 2 {
                    for c in 0..3 {
                        pixel[c] = composed[offset + c] ^ (255 - pixel[c]);
                    }
                }
                fdi_composite(&mut composed, offset, pixel);
            }
        }
    }
    let meta = FdiMeta {
        width: lw as usize,
        height: lh as usize,
        reg_x: int(&loop_member["regX"]),
        reg_y: int(&loop_member["regY"]),
        use_alpha: true,
        alpha_threshold: loop_member["alphaThreshold"].as_u64().map(|v| v as u8),
        rgba32: loop_member["rgba32"].as_bool().unwrap_or(false),
    };
    Ok(Composed { data: fdi_image(&meta, &composed)?, sounds: Value::Array(sounds.to_vec()) })
}

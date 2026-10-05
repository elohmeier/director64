//! Film-loop flattening (was tests/node/film-model.test.mjs).

mod common;

use common::*;
use director64_aot::convert::fdi::{fdi_image, fdi_pixel, FdiMeta};
use director64_aot::convert::film::{compose_film_frame, shape_spans, Composed};
use serde_json::{json, Value};
use std::collections::HashMap;

#[derive(Default)]
struct Sprite {
    ink: u8,
    cast: i64,
    member: i64,
    x: i64,
    y: i64,
    blend: Option<u8>,
    fore: Option<[u8; 3]>,
}
fn sprite(s: Sprite) -> Value {
    let mut b = vec![0u8; 48];
    b[0] = 1;
    b[1] = s.ink;
    be16(&mut b, 4, s.cast);
    be16(&mut b, 6, s.member);
    be16(&mut b, 12, s.y);
    be16(&mut b, 14, s.x);
    if let Some(blend) = s.blend {
        b[21] = blend;
        b[22] = 16;
    }
    if let Some(f) = s.fore {
        (b[2], b[24], b[26]) = (f[0], f[1], f[2]);
    }
    json!({"bytes": b, "behaviors": []})
}
fn bitmap(number: i64, width: usize, height: usize, rgba: &[u8], cast: i64) -> (Value, Vec<u8>) {
    let child = json!({"cast": cast, "number": number, "type": 1, "width": width, "height": height,
        "regX": 0, "regY": 0, "asset": format!("asset-{cast}-{number}")});
    let meta = FdiMeta { width, height, use_alpha: true, ..Default::default() };
    (child, fdi_image(&meta, rgba).unwrap())
}
fn solid(count: usize, color: [u8; 4]) -> Vec<u8> {
    color.repeat(count)
}
fn compose(movie: &Value, lp: &Value, states: &[(i64, Value)], assets: &HashMap<String, Vec<u8>>,
    movies: &[Value], limits: &mut Vec<Value>) -> Composed {
    let mut load = |asset: &str| assets.get(asset).cloned().ok_or_else(|| "no bitmap".to_string());
    compose_film_frame(movie, lp, states, &mut load, movies, limits).unwrap()
}

#[test]
fn d10_film_loops_flatten_shape_outlines_with_runtime_span_geometry() {
    let lp = json!({"cast": 1, "number": 9, "left": 0, "top": 0, "width": 4, "height": 4, "filmScore": {"version": 13}});
    let shape = json!({"cast": 1, "number": 2, "type": 8, "shape": 1, "pattern": 1, "filled": 0, "lineWidth": 1,
        "lineDirection": 5, "width": 4, "height": 4});
    let movie = json!({"name": "M", "members": [shape]});
    let record = sprite(Sprite { cast: 1, member: 2, fore: Some([10, 20, 30]), ..Default::default() });
    let none = HashMap::new();
    let Composed { data, .. } = compose(&movie, &lp, &[(6, record)], &none, &[], &mut vec![]);
    for y in 0..4 {
        for x in 0..4 {
            let edge = x == 0 || y == 0 || x == 3 || y == 3;
            assert_eq!(fdi_pixel(&data, y * 4 + x, 8), if edge { [8, 16, 24, 255] } else { [0; 4] }, "{x},{y}");
        }
    }
    let invisible = sprite(Sprite { cast: 1, member: 2, fore: Some([10, 20, 30]), blend: Some(255), ..Default::default() });
    let empty = compose(&movie, &lp, &[(6, invisible)], &none, &[], &mut vec![]);
    assert_eq!(fdi_pixel(&empty.data, 0, 8)[3], 0);
    assert_eq!(shape_spans(1, 4, 4, 1, 1, false, false), [(0, 1), (3, 4)]);
    assert_eq!(shape_spans(4, 4, 4, 0, 1, false, true), [(2, 4)]);
    assert_eq!(shape_spans(4, 4, 4, 0, 1, false, false), [(0, 2)]);
}

#[test]
fn mask_ink_stencils_through_the_neighbouring_bitmap_member() {
    let lp = json!({"cast": 1, "number": 9, "left": 0, "top": 0, "width": 2, "height": 1, "filmScore": {"version": 13}});
    let (child, pixels) = bitmap(5, 2, 1, &solid(2, [255, 0, 0, 255]), 1);
    let (mask, mask_pixels) = bitmap(6, 2, 1, &[0, 0, 0, 255, 255, 255, 255, 255], 1);
    let mut assets = HashMap::from([
        (child["asset"].as_str().unwrap().to_string(), pixels),
        (mask["asset"].as_str().unwrap().to_string(), mask_pixels),
    ]);
    let movie = json!({"name": "M", "members": [child, mask]});
    let states = [(6, sprite(Sprite { ink: 9, cast: 1, member: 5, ..Default::default() }))];
    let hard = compose(&movie, &lp, &states, &assets, &[], &mut vec![]);
    assert_eq!(fdi_pixel(&hard.data, 0, 8)[3], 255);
    assert_eq!(fdi_pixel(&hard.data, 1, 8)[3], 0);
    // Anti-aliased grays threshold at half luminance with a recorded limit.
    let (_, gray) = bitmap(6, 2, 1, &[60, 60, 60, 255, 200, 200, 200, 255], 1);
    assets.insert(mask["asset"].as_str().unwrap().to_string(), gray);
    let mut limits = vec![];
    let soft = compose(&movie, &lp, &states, &assets, &[], &mut limits);
    assert_eq!(fdi_pixel(&soft.data, 0, 8)[3], 255);
    assert_eq!(fdi_pixel(&soft.data, 1, 8)[3], 0);
    assert_eq!(limits.iter().map(|l| l["kind"].clone()).collect::<Vec<_>>(), [json!("film-loop-mask-antialiased")]);
}

#[test]
fn d6_sound_channels_capture_references_and_record_unresolved_casts() {
    let lp = json!({"cast": 3, "number": 9, "left": 0, "top": 0, "width": 1, "height": 1, "filmScore": {"version": 13}});
    let channel = |cast: i64, member: i64| {
        let mut b = vec![0u8; 48];
        be16(&mut b, 0, cast);
        be16(&mut b, 2, member);
        json!({"bytes": b, "behaviors": []})
    };
    let movie = json!({"name": "M", "members": []});
    let none = HashMap::new();
    let mut limits = vec![];
    let captured = compose(&movie, &lp, &[(3, channel(-1, 7)), (4, channel(2, 9))], &none, &[], &mut limits);
    assert_eq!(captured.sounds, json!([{"cast": 3, "member": 7}, {"cast": 2, "member": 9}]));
    let mut unresolved = vec![];
    let silent = compose(&movie, &lp, &[(3, channel(0, 5))], &none, &[], &mut unresolved);
    assert_eq!(silent.sounds, json!([null, null]));
    assert_eq!(unresolved[0]["kind"], "film-loop-unresolved-sound");
}

#[test]
fn unresolved_absent_and_external_children_follow_the_reference() {
    let lp = json!({"cast": 1, "number": 9, "left": 0, "top": 0, "width": 1, "height": 1, "filmScore": {"version": 13}});
    let (child, pixels) = bitmap(3, 1, 1, &solid(1, [0, 255, 0, 255]), 1);
    let external = json!({"name": "EXT.CXT", "members": [child]});
    let movie = json!({"name": "M", "members": [], "casts": [{"number": 1, "file": "M"}, {"number": 2, "file": "EXT.CXT"}]});
    let assets = HashMap::from([(child["asset"].as_str().unwrap().to_string(), pixels)]);
    let movies = [external];
    let mut limits = vec![];
    let at = |cast, member| [(6, sprite(Sprite { cast, member, ..Default::default() }))];
    let zero = compose(&movie, &lp, &at(0, 3), &assets, &movies, &mut limits);
    assert_eq!(fdi_pixel(&zero.data, 0, 8)[3], 0);
    assert_eq!(limits[0]["kind"], "film-loop-unresolved-child");
    let absent = compose(&movie, &lp, &at(1, 99), &assets, &movies, &mut limits);
    assert_eq!(fdi_pixel(&absent.data, 0, 8)[3], 0);
    assert_eq!(limits[1]["kind"], "film-loop-absent-child");
    let resolved = compose(&movie, &lp, &at(2, 3), &assets, &movies, &mut limits);
    assert_eq!(fdi_pixel(&resolved.data, 0, 8), [0, 255, 0, 255]);
    // Reference sub-channels are render-only; behaviors never reject a child.
    let mut behavior = sprite(Sprite { cast: 2, member: 3, ..Default::default() });
    behavior["behaviors"] = json!([{"cast": 1, "member": 1}]);
    let scripted = compose(&movie, &lp, &[(6, behavior)], &assets, &movies, &mut limits);
    assert_eq!(fdi_pixel(&scripted.data, 0, 8), [0, 255, 0, 255]);
    assert_eq!(limits.len(), 2);
}

#[test]
fn d5_flag_poses_preserve_uncovered_margins_under_parent_copy_ink() {
    let lp = json!({"cast": 1, "left": 117, "top": 21, "width": 54, "height": 26, "filmScore": {"version": 7}});
    let (lw, lh, left, top) = (54i64, 26i64, 117i64, 21i64);
    // Recovered flag geometry; synthetic pixels contain both opaque white and red.
    for (width, height, reg_x, reg_y) in [(47i64, 25i64, 203i64, 218i64), (47, 25, 203, 219), (53, 23, 202, 218)] {
        let child = json!({"cast": 1, "number": 1, "type": 1, "width": width, "height": height,
            "regX": reg_x, "regY": reg_y, "asset": "pose"});
        let rgba: Vec<u8> = (0..width * height).flat_map(|i| if i == 0 { [255, 255, 255, 255] } else { [255, 0, 0, 255] }).collect();
        let meta = FdiMeta { width: width as usize, height: height as usize, reg_x, reg_y, ..Default::default() };
        let assets = HashMap::from([("pose".to_string(), fdi_image(&meta, &rgba).unwrap())]);
        let mut bytes = vec![0u8; 24];
        bytes[0] = 16;
        be16(&mut bytes, 4, 65535);
        be16(&mut bytes, 6, 1);
        be16(&mut bytes, 12, 240);
        be16(&mut bytes, 14, 320);
        // Without stretch, the bitmap's dimensions override these stale score sizes.
        be16(&mut bytes, 16, 32);
        be16(&mut bytes, 18, 51);
        let movie = json!({"members": [child]});
        let composed = compose(&movie, &lp, &[(8, json!({"bytes": bytes, "behaviors": []}))], &assets, &[], &mut vec![]);
        let mut covered = 0;
        for y in 0..lh {
            for x in 0..lw {
                let inside = x >= 320 - reg_x - left && x < 320 - reg_x - left + width
                    && y >= 240 - reg_y - top && y < 240 - reg_y - top + height;
                let pixel = fdi_pixel(&composed.data, (y * lw + x) as usize, 0);
                assert_eq!(pixel[3], if inside { 255 } else { 0 }, "pose {reg_x},{reg_y}: {x},{y}");
                covered += i64::from(pixel[3] > 0);
            }
        }
        assert_eq!(covered, width * height);
        let first = (240 - reg_y - top) * lw + 320 - reg_x - left;
        assert_eq!(fdi_pixel(&composed.data, first as usize, 0), [255, 255, 255, 255]);
    }
}

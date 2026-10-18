//! Film-loop flattening (was tests/node/film-model.test.mjs).

mod common;

use common::*;
use director64_aot::convert::fdi::{FdiMeta, fdi_image, fdi_pixel};
use director64_aot::convert::film::{
    Composed, NESTED_PENDING, compose_film_frame, needs_runtime, shape_spans,
};
use serde_json::{Value, json};
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
    let meta = FdiMeta {
        width,
        height,
        use_alpha: true,
        ..Default::default()
    };
    (child, fdi_image(&meta, rgba).unwrap())
}
fn solid(count: usize, color: [u8; 4]) -> Vec<u8> {
    color.repeat(count)
}
fn compose(
    movie: &Value,
    lp: &Value,
    states: &[(i64, Value)],
    assets: &HashMap<String, Vec<u8>>,
    movies: &[Value],
    limits: &mut Vec<Value>,
) -> Composed {
    compose_at(movie, lp, 0, states, assets, movies, limits).unwrap()
}
fn compose_at(
    movie: &Value,
    lp: &Value,
    frame: usize,
    states: &[(i64, Value)],
    assets: &HashMap<String, Vec<u8>>,
    movies: &[Value],
    limits: &mut Vec<Value>,
) -> Result<Composed, String> {
    let mut load = |asset: &str| {
        assets
            .get(asset)
            .cloned()
            .ok_or_else(|| "no bitmap".to_string())
    };
    compose_film_frame(movie, lp, frame, false, states, &mut load, movies, limits)
}

#[test]
fn authored_empty_bitmap_children_preserve_the_other_layers() {
    let (child, pixels) = bitmap(1, 1, 1, &[255, 0, 0, 255], 1);
    let mut movie = json!({"name":"M","directorVersion":850,"members":[child,
        {"cast":1,"number":2,"type":1,"empty":true,"width":0,"height":0}]});
    let lp = json!({"cast":1,"number":9,"width":1,"height":1,"filmScore":{"version":13}});
    let states = [
        (
            6,
            sprite(Sprite {
                cast: 65535,
                member: 1,
                ..Default::default()
            }),
        ),
        (
            7,
            sprite(Sprite {
                cast: 65535,
                member: 2,
                ..Default::default()
            }),
        ),
    ];
    let assets = HashMap::from([("asset-1-1".into(), pixels)]);
    let mut limits = Vec::new();
    let result = compose(&movie, &lp, &states, &assets, &[], &mut limits);
    assert_eq!(fdi_pixel(&result.data, 0, 8), [255, 0, 0, 255]);
    assert!(limits.is_empty());
    movie["members"][1]["empty"] = json!(false);
    movie["members"][1]["width"] = json!(1);
    movie["members"][1]["height"] = json!(1);
    assert_eq!(
        compose_at(&movie, &lp, 0, &states, &assets, &[], &mut Vec::new())
            .err()
            .unwrap(),
        "no bitmap"
    );
}

#[test]
fn modern_film_main_channels_are_inert_but_sprite_and_sound_channels_survive() {
    let (child, pixels) = bitmap(1, 1, 1, &[255, 0, 0, 255], 1);
    let movie = json!({"name":"M","directorVersion":850,"members":[child]});
    let lp = json!({"cast":1,"number":9,"width":1,"height":1,"filmScore":{"version":13}});
    let mut states = vec![(
        6,
        sprite(Sprite {
            cast: 65535,
            member: 1,
            ..Default::default()
        }),
    )];
    for channel in [0, 1, 2, 5] {
        let mut bytes = vec![0; 48];
        bytes[0] = 255;
        bytes[1] = 255;
        bytes[3] = 15;
        states.push((channel, json!({"bytes":bytes})));
    }
    let mut sound = vec![0; 48];
    be16(&mut sound, 0, 65535);
    be16(&mut sound, 2, 7);
    states.push((4, json!({"bytes":sound})));
    let mut limits = Vec::new();
    let result = compose(
        &movie,
        &lp,
        &states,
        &HashMap::from([("asset-1-1".into(), pixels)]),
        &[],
        &mut limits,
    );
    assert_eq!(fdi_pixel(&result.data, 0, 8), [255, 0, 0, 255]);
    assert_eq!(result.sounds, json!([{"cast":1,"member":7},null]));
    assert!(limits.is_empty());
}

#[test]
fn recovered_font_resources_in_film_sprite_channels_are_nonvisual() {
    let lp = json!({"cast":1,"number":9,"width":1,"height":1,"filmScore":{"version":13}});
    let movie = json!({"name":"M","directorVersion":850,"members":[
        {"cast":1,"number":2,"type":15,"xtra":{"symbol":"font"},"embeddedFont":1}]});
    let states = [(
        6,
        sprite(Sprite {
            cast: 65535,
            member: 2,
            ..Default::default()
        }),
    )];
    let mut limits = Vec::new();
    let result = compose(&movie, &lp, &states, &HashMap::new(), &[], &mut limits);
    assert_eq!(fdi_pixel(&result.data, 0, 8)[3], 0);
    assert_eq!(limits[0]["kind"], "film-loop-nonvisual-child");
}

#[test]
fn live_text_layers_and_their_ancestors_keep_runtime_composition() {
    let channel = |member| {
        let mut record = sprite(Sprite {
            cast: 65535,
            member,
            ..Default::default()
        });
        record["channel"] = json!(6);
        record
    };
    let text = json!({"cast":1,"number":1,"type":3,"text":"Credits"});
    let child = json!({"cast":1,"number":2,"type":2,"width":1,"height":1,
        "filmScore":{"version":13,"frames":[{"channels":[channel(1)]}]}});
    let parent = json!({"cast":1,"number":3,"type":2,"width":1,"height":1,
        "filmScore":{"version":13,"frames":[{"channels":[channel(2)]}]}});
    let movie = json!({"name":"M","directorVersion":850,"members":[text,child,parent]});
    let mut lp = movie["members"][2].clone();
    assert!(needs_runtime(&movie, &lp, &[]).unwrap());
    lp["filmRuntime"] = json!(true);
    let mut notes = Vec::new();
    let output = compose(
        &movie,
        &lp,
        &[(6, channel(2))],
        &HashMap::new(),
        &[],
        &mut notes,
    );
    assert_eq!(fdi_pixel(&output.data, 0, 8)[3], 0);
    assert!(
        notes.is_empty(),
        "retained layers must not be recorded as discarded output"
    );
}

#[test]
fn wide_runtime_film_strips_keep_geometry_without_allocating_a_flat_bitmap() {
    let member = json!({"cast":1,"number":1,"type":2,"width":6316,"height":89,
        "regX":3158,"regY":44,"filmScore":{"version":13,"frames":[{"channels":[]}]}});
    let movie = json!({"name":"M","directorVersion":850,"members":[member.clone()]});
    assert!(needs_runtime(&movie, &member, &[]).unwrap());
    let mut runtime = member;
    runtime["filmRuntime"] = json!(true);
    let result = compose(&movie, &runtime, &[], &HashMap::new(), &[], &mut Vec::new());
    assert_eq!(&result.data[8..16], &[0, 1, 0, 1, 0, 0, 0, 0]);
    assert_eq!(result.data.len(), 34);
    assert_eq!(fdi_pixel(&result.data, 0, 8)[3], 0);
}

#[test]
fn d10_film_loops_flatten_shape_outlines_with_runtime_span_geometry() {
    let lp = json!({"cast": 1, "number": 9, "left": 0, "top": 0, "width": 4, "height": 4, "filmScore": {"version": 13}});
    let shape = json!({"cast": 1, "number": 2, "type": 8, "shape": 1, "pattern": 1, "filled": 0, "lineWidth": 1,
        "lineDirection": 5, "width": 4, "height": 4});
    let movie = json!({"name": "M", "members": [shape]});
    let record = sprite(Sprite {
        cast: 65535,
        member: 2,
        fore: Some([10, 20, 30]),
        ..Default::default()
    });
    let none = HashMap::new();
    let Composed { data, .. } = compose(&movie, &lp, &[(6, record)], &none, &[], &mut vec![]);
    for y in 0..4 {
        for x in 0..4 {
            let edge = x == 0 || y == 0 || x == 3 || y == 3;
            assert_eq!(
                fdi_pixel(&data, y * 4 + x, 8),
                if edge { [8, 16, 24, 255] } else { [0; 4] },
                "{x},{y}"
            );
        }
    }
    let invisible = sprite(Sprite {
        cast: 65535,
        member: 2,
        fore: Some([10, 20, 30]),
        blend: Some(255),
        ..Default::default()
    });
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
    let states = [(
        6,
        sprite(Sprite {
            ink: 9,
            cast: 65535,
            member: 5,
            ..Default::default()
        }),
    )];
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
    assert_eq!(
        limits.iter().map(|l| l["kind"].clone()).collect::<Vec<_>>(),
        [json!("film-loop-mask-antialiased")]
    );
}

#[test]
fn director_85_masks_keep_grey_levels_as_coverage() {
    let lp = json!({"cast": 1, "number": 9, "left": 0, "top": 0, "width": 2, "height": 1, "filmScore": {"version": 13}});
    let (child, pixels) = bitmap(5, 2, 1, &solid(2, [255, 0, 0, 255]), 1);
    let (mask, gray) = bitmap(6, 2, 1, &[60, 60, 60, 255, 200, 200, 200, 255], 1);
    let assets = HashMap::from([
        (child["asset"].as_str().unwrap().to_string(), pixels),
        (mask["asset"].as_str().unwrap().to_string(), gray),
    ]);
    let movie = json!({"name": "M", "directorVersion": 850, "members": [child, mask]});
    let states = [(
        6,
        sprite(Sprite {
            ink: 9,
            cast: 65535,
            member: 5,
            ..Default::default()
        }),
    )];
    let mut limits = vec![];
    let soft = compose(&movie, &lp, &states, &assets, &[], &mut limits);
    // The 5-bit stored grey reads back a little off its authored value.
    let (dark, light) = (
        fdi_pixel(&soft.data, 0, 8)[3],
        fdi_pixel(&soft.data, 1, 8)[3],
    );
    assert!(
        (190..=200).contains(&dark),
        "dark mask pixel coverage {dark}"
    );
    assert!(
        (45..=60).contains(&light),
        "light mask pixel coverage {light}"
    );
    assert!(limits.is_empty());
}

#[test]
fn modern_sound_channels_relocate_zero_and_one_and_preserve_owner_sentinel() {
    let lp = json!({"cast":3,"number":9,"width":1,"height":1,"filmScore":{"version":13}});
    let channel = |cast: i64, member: i64| {
        let mut b = vec![0u8; 48];
        be16(&mut b, 0, cast);
        be16(&mut b, 2, member);
        json!({"bytes":b})
    };
    // Source-free form of two independent original 8.5.1r104 controls:
    // encoded cast0/1 follows ccl entry0/1, while 0xffff remains the owner.
    let movie = json!({"name":"M","members":[],"casts":[{"number":3,"filmCastMap":[5,2]}]});
    let none = HashMap::new();
    let result = compose(
        &movie,
        &lp,
        &[(3, channel(0, 7)), (4, channel(1, 9))],
        &none,
        &[],
        &mut vec![],
    );
    assert_eq!(
        result.sounds,
        json!([{"cast":2,"member":9},{"cast":5,"member":7}])
    );
    let owned = compose(&movie, &lp, &[(4, channel(-1, 7))], &none, &[], &mut vec![]);
    assert_eq!(owned.sounds, json!([{"cast":3,"member":7},null]));
    let bad = compose_at(
        &movie,
        &lp,
        0,
        &[(4, channel(2, 7))],
        &none,
        &[],
        &mut vec![],
    )
    .err()
    .unwrap();
    assert!(bad.contains("relocation index 2"));
    let missing = json!({"name":"M","members":[]});
    assert!(
        compose_at(
            &missing,
            &lp,
            0,
            &[(4, channel(0, 7))],
            &none,
            &[],
            &mut vec![]
        )
        .err()
        .unwrap()
        .contains("table missing")
    );
}

#[test]
fn external_sound_relocations_keep_the_source_movie_for_packaging() {
    let film = json!({"cast":1,"number":9,"width":1,"height":1,"filmScore":{"version":13}});
    let movie = json!({"name":"M","casts":[{"number":1,
        "filmCastMap":[{"file":"SOUND.CXT","cast":1}]}]});
    let external = json!({"name":"SOUND.CXT","members":[]});
    let mut bytes = vec![0; 48];
    be16(&mut bytes, 2, 7);
    let result = compose(
        &movie,
        &film,
        &[(4, json!({"bytes":bytes}))],
        &HashMap::new(),
        &[external],
        &mut vec![],
    );
    assert_eq!(
        result.sounds,
        json!([{"file":"SOUND.CXT","cast":1,"member":7},null])
    );
}

#[test]
fn relocated_zero_external_and_owner_visual_references_share_the_same_resolver() {
    let lp = json!({"cast":1,"number":9,"width":1,"height":1,"filmScore":{"version":13}});
    let (child, pixels) = bitmap(3, 1, 1, &[0, 255, 0, 255], 1);
    let external = json!({"name":"EXT.CXT","members":[child]});
    let movie = json!({"name":"M","members":[],"casts":[
        {"number":1,"file":"M","filmCastMap":[2,{"file":"EXT.CXT","cast":1}]},
        {"number":2,"file":"EXT.CXT"}]});
    let assets = HashMap::from([("asset-1-3".into(), pixels)]);
    let movies = [external];
    let at = |cast, member| {
        [(
            6,
            sprite(Sprite {
                cast,
                member,
                ..Default::default()
            }),
        )]
    };
    for cast in [0, 1] {
        let resolved = compose(&movie, &lp, &at(cast, 3), &assets, &movies, &mut vec![]);
        assert_eq!(fdi_pixel(&resolved.data, 0, 8), [0, 255, 0, 255]);
    }
    let mut limits = vec![];
    let absent = compose(&movie, &lp, &at(65535, 99), &assets, &movies, &mut limits);
    assert_eq!(fdi_pixel(&absent.data, 0, 8)[3], 0);
    assert_eq!(limits[0]["kind"], "film-loop-absent-child");
    let mut record = at(1, 3)[0].1.clone();
    record["behaviors"] = json!([{"cast":1,"member":1}]);
    let scripted = compose(&movie, &lp, &[(6, record)], &assets, &movies, &mut vec![]);
    assert_eq!(fdi_pixel(&scripted.data, 0, 8), [0, 255, 0, 255]);
}

#[test]
fn d5_flag_poses_preserve_uncovered_margins_under_parent_copy_ink() {
    let lp = json!({"cast": 1, "left": 117, "top": 21, "width": 54, "height": 26, "filmScore": {"version": 7}});
    let (lw, lh, left, top) = (54i64, 26i64, 117i64, 21i64);
    // Recovered flag geometry; synthetic pixels contain both opaque white and red.
    for (width, height, reg_x, reg_y) in [
        (47i64, 25i64, 203i64, 218i64),
        (47, 25, 203, 219),
        (53, 23, 202, 218),
    ] {
        let child = json!({"cast": 1, "number": 1, "type": 1, "width": width, "height": height,
            "regX": reg_x, "regY": reg_y, "asset": "pose"});
        let rgba: Vec<u8> = (0..width * height)
            .flat_map(|i| {
                if i == 0 {
                    [255, 255, 255, 255]
                } else {
                    [255, 0, 0, 255]
                }
            })
            .collect();
        let meta = FdiMeta {
            width: width as usize,
            height: height as usize,
            reg_x,
            reg_y,
            ..Default::default()
        };
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
        let composed = compose(
            &movie,
            &lp,
            &[(8, json!({"bytes": bytes, "behaviors": []}))],
            &assets,
            &[],
            &mut vec![],
        );
        let mut covered = 0;
        for y in 0..lh {
            for x in 0..lw {
                let inside = x >= 320 - reg_x - left
                    && x < 320 - reg_x - left + width
                    && y >= 240 - reg_y - top
                    && y < 240 - reg_y - top + height;
                let pixel = fdi_pixel(&composed.data, (y * lw + x) as usize, 0);
                assert_eq!(
                    pixel[3],
                    if inside { 255 } else { 0 },
                    "pose {reg_x},{reg_y}: {x},{y}"
                );
                covered += i64::from(pixel[3] > 0);
            }
        }
        assert_eq!(covered, width * height);
        let first = (240 - reg_y - top) * lw + 320 - reg_x - left;
        assert_eq!(
            fdi_pixel(&composed.data, first as usize, 0),
            [255, 255, 255, 255]
        );
    }
}

#[test]
fn director_85_loops_draw_a_nested_loops_frame_and_wait_for_it() {
    let lp = json!({"cast": 1, "number": 9, "left": 0, "top": 0, "width": 1, "height": 1, "filmScore": {"version": 13}});
    let (_, red) = bitmap(0, 1, 1, &solid(1, [255, 0, 0, 255]), 1);
    let (_, blue) = bitmap(0, 1, 1, &solid(1, [0, 0, 255, 255]), 1);
    let assets = HashMap::from([("red".to_string(), red), ("blue".to_string(), blue)]);
    let nested = json!({"cast": 1, "number": 4, "type": 2, "width": 1, "height": 1, "regX": 0, "regY": 0,
        "filmAssets": ["red", "blue"]});
    let movie = json!({"name": "M", "directorVersion": 850, "members": [nested]});
    let at = [(
        6,
        sprite(Sprite {
            cast: 65535,
            member: 4,
            ..Default::default()
        }),
    )];
    let frame = |n| {
        compose_at(&movie, &lp, n, &at, &assets, &[], &mut vec![])
            .unwrap()
            .data
    };
    assert_eq!(fdi_pixel(&frame(0), 0, 8), [255, 0, 0, 255]);
    assert_eq!(fdi_pixel(&frame(3), 0, 8), [0, 0, 255, 255]);
    let pending = json!({"name": "M", "directorVersion": 850, "members": [{"cast": 1, "number": 4, "type": 2, "width": 1, "height": 1}]});
    assert_eq!(
        compose_at(&pending, &lp, 0, &at, &assets, &[], &mut vec![])
            .err()
            .as_deref(),
        Some(NESTED_PENDING)
    );
    // Other versions keep their deferral of nested loops.
    let d10 = json!({"name": "M", "directorVersion": 1000, "members": [nested]});
    assert!(compose_at(&d10, &lp, 0, &at, &assets, &[], &mut vec![]).is_err());
}

#[test]
fn nonvisual_and_director_85_field_children_are_skipped_and_recorded() {
    let lp = json!({"cast": 1, "number": 9, "left": 0, "top": 0, "width": 1, "height": 1, "filmScore": {"version": 13}});
    let at = |member| {
        [(
            6,
            sprite(Sprite {
                cast: 65535,
                member,
                ..Default::default()
            }),
        )]
    };
    let none = HashMap::new();
    let members = json!([{"cast": 1, "number": 2, "type": 6}, {"cast": 1, "number": 3, "type": 3, "name": "Marker", "width": 9, "height": 9}]);
    let movie = json!({"name": "M", "directorVersion": 850, "members": members});
    let mut limits = vec![];
    compose_at(&movie, &lp, 0, &at(2), &none, &[], &mut limits).unwrap();
    compose_at(&movie, &lp, 0, &at(3), &none, &[], &mut limits).unwrap();
    assert_eq!(
        limits.iter().map(|l| l["kind"].clone()).collect::<Vec<_>>(),
        [
            json!("film-loop-nonvisual-child"),
            json!("film-loop-unflattened-child")
        ]
    );
    let d10 = json!({"name": "M", "directorVersion": 1000, "members": members});
    assert!(compose_at(&d10, &lp, 0, &at(3), &none, &[], &mut vec![]).is_err());
}

#[test]
fn mask_ink_without_a_bitmap_mask_uses_matte_and_on_a_shape_draws_nothing() {
    let lp = json!({"cast": 1, "number": 9, "left": 0, "top": 0, "width": 2, "height": 1, "filmScore": {"version": 13}});
    let (child, pixels) = bitmap(5, 2, 1, &[255, 255, 255, 0, 255, 0, 0, 255], 1);
    let assets = HashMap::from([(child["asset"].as_str().unwrap().to_string(), pixels)]);
    let movie = json!({"name": "M", "members": [child, {"cast": 1, "number": 6, "type": 3}]});
    let mut limits = vec![];
    let copied = compose(
        &movie,
        &lp,
        &[(
            6,
            sprite(Sprite {
                ink: 9,
                cast: 65535,
                member: 5,
                ..Default::default()
            }),
        )],
        &assets,
        &[],
        &mut limits,
    );
    assert_eq!(fdi_pixel(&copied.data, 1, 8), [255, 0, 0, 255]);
    assert_eq!(fdi_pixel(&copied.data, 0, 8)[3], 0);
    assert_eq!(limits[0]["kind"], "film-loop-mask-missing");
    assert_eq!(limits[0]["fallback"], "matte");
    assert_eq!(limits[0]["implemented"], true);
    let shape = json!({"cast": 1, "number": 2, "type": 8, "shape": 1, "pattern": 1, "filled": 1, "lineWidth": 0,
        "lineDirection": 5, "width": 2, "height": 1});
    let movie = json!({"name": "M", "members": [shape]});
    let mut limits = vec![];
    let hidden = compose(
        &movie,
        &lp,
        &[(
            6,
            sprite(Sprite {
                ink: 9,
                cast: 65535,
                member: 2,
                fore: Some([10, 20, 30]),
                ..Default::default()
            }),
        )],
        &assets,
        &[],
        &mut limits,
    );
    assert_eq!(fdi_pixel(&hidden.data, 0, 8)[3], 0);
    assert_eq!(limits[0]["kind"], "film-loop-shape-mask-ink");
}

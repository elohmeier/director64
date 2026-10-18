//! Static SWF rasterizing and Flash timelines (was
//! tests/node/director-swf.test.mjs).

mod common;

use common::*;
use director64_aot::convert::swf::{
    RasterLayer, VectorShape, flash_frames, flash_frames_into, flash_frames_with_layers,
    vector_shape_pixels,
};
use serde_json::json;

struct Line {
    width: u16,
    color: [u8; 4],
}

/// Shape records over 7-bit signed deltas, as the JS fixture builder wrote them.
struct Records(Writer);
impl Records {
    fn style(
        &mut self,
        mv: Option<(i64, i64)>,
        f0: Option<i64>,
        f1: Option<i64>,
        ln: Option<i64>,
    ) -> &mut Self {
        let flags = i64::from(mv.is_some())
            | if f0.is_some() { 2 } else { 0 }
            | if f1.is_some() { 4 } else { 0 }
            | if ln.is_some() { 8 } else { 0 };
        self.0.put(0, 1).put(flags, 5);
        if let Some((x, y)) = mv {
            self.0.put(7, 5).put(x, 7).put(y, 7);
        }
        for v in [f0, f1, ln].into_iter().flatten() {
            self.0.put(v, 4);
        }
        self
    }
    fn edge(&mut self, dx: i64, dy: i64) -> &mut Self {
        self.0
            .put(1, 1)
            .put(1, 1)
            .put(5, 4)
            .put(1, 1)
            .put(dx, 7)
            .put(dy, 7);
        self
    }
    fn curve(&mut self, cx: i64, cy: i64, ax: i64, ay: i64) -> &mut Self {
        self.0
            .put(1, 1)
            .put(0, 1)
            .put(5, 4)
            .put(cx, 7)
            .put(cy, 7)
            .put(ax, 7)
            .put(ay, 7);
        self
    }
    fn rect(&mut self, width: i64, height: i64) -> &mut Self {
        for (dx, dy) in [(width, 0), (0, height), (-width, 0), (0, -height)] {
            self.edge(dx, dy);
        }
        self
    }
}

fn shape_bytes(
    id: u16,
    fills: &[[u8; 4]],
    lines: &[Line],
    version: u32,
    records: impl FnOnce(&mut Records),
) -> Vec<u8> {
    let rgba = version >= 3;
    let color = |c: [u8; 4]| if rgba { c.to_vec() } else { c[..3].to_vec() };
    let mut styles = vec![fills.len() as u8];
    for &c in fills {
        styles.push(0);
        styles.extend(color(c));
    }
    styles.push(lines.len() as u8);
    for l in lines {
        styles.extend(l.width.to_le_bytes());
        styles.extend(color(l.color));
    }
    let mut w = Records(Writer::new());
    w.0.put(4, 4).put(4, 4);
    records(&mut w);
    w.0.put(0, 6);
    concat(&[&id.to_le_bytes(), &stage_rect(), &styles, &w.0.done()])
}
fn place(id: u16, depth: u16) -> Vec<u8> {
    let matrix = Writer::new()
        .put(0, 1)
        .put(0, 1)
        .put(7, 5)
        .put(0, 7)
        .put(0, 7)
        .done();
    tag(
        4,
        &concat(&[&id.to_le_bytes(), &depth.to_le_bytes(), &matrix]),
    )
}
fn place2(
    depth: u16,
    mv: bool,
    id: Option<u16>,
    at: Option<(i64, i64)>,
    name: Option<&str>,
) -> Vec<u8> {
    let flags = u8::from(mv)
        | if id.is_some() { 2 } else { 0 }
        | if at.is_some() { 4 } else { 0 }
        | if name.is_some() { 32 } else { 0 };
    let mut out = vec![flags];
    out.extend(depth.to_le_bytes());
    if let Some(id) = id {
        out.extend(id.to_le_bytes());
    }
    if let Some((tx, ty)) = at {
        out.extend(
            Writer::new()
                .put(0, 1)
                .put(0, 1)
                .put(7, 5)
                .put(tx, 7)
                .put(ty, 7)
                .done(),
        );
    }
    if let Some(name) = name {
        out.extend(latin1(&format!("{name}\0")));
    }
    tag(26, &out)
}
fn edit_text(id: u16, variable: &str, left: u16, right: u16, indent: u16) -> Vec<u8> {
    // wordWrap, multiline, readOnly and layout; no initial text.
    let flags: u16 = 0x40 | 0x20 | 8 | 0x2000;
    let mut layout = vec![2u8];
    layout.extend(left.to_le_bytes());
    layout.extend(right.to_le_bytes());
    layout.extend(indent.to_le_bytes());
    layout.extend(40i16.to_le_bytes());
    concat(&[
        &id.to_le_bytes(),
        &stage_rect(),
        &flags.to_le_bytes(),
        &layout,
        &latin1(&format!("{variable}\0")),
    ])
}
fn swf(body: &[u8], frames: u16) -> Vec<u8> {
    let mut data = concat(&[
        &latin1("FWS\x01\0\0\0\0"),
        &stage_rect(),
        &[0, 12],
        &frames.to_le_bytes(),
        body,
        &tag(0, &[]),
    ]);
    let length = data.len() as i64;
    le32(&mut data, 4, length);
    data
}

#[test]
fn long_flash_timelines_stream_frames_without_retaining_rgba() {
    let body = tag(1, &[]).repeat(359);
    let mut count = 0;
    let mut sink = |width, height, rgba: &[u8]| {
        assert_eq!(rgba.len(), width * height * 4);
        count += 1;
        Ok(())
    };
    let movie = flash_frames_into(&swf(&body, 359), Some(&mut sink)).unwrap();
    assert_eq!(count, 359);
    assert!(movie.frames.is_empty());
    assert!(
        flash_frames(&swf(&body, 358))
            .err()
            .unwrap()
            .contains("more frames than declared")
    );
}
fn envelope(data: &[u8]) -> Vec<u8> {
    let mut head = vec![0u8; 12];
    be32(&mut head, 0, data.len() as i64 + 12);
    be32(&mut head, 4, 1);
    be32(&mut head, 8, data.len() as i64);
    concat(&[&head, data])
}
fn pixel(image: &VectorShape, x: usize, y: usize) -> u16 {
    read_be16(&image.pixels, (y * image.width + x) * 2)
}
const RED: u16 = 0xf801;
const TRANSPARENT: u16 = 0xfffe;

fn vector(shape: &[u8]) -> VectorShape {
    vector_shape_pixels(&envelope(&swf(
        &concat(&[&tag(22, shape), &place(1, 1), &tag(1, &[])]),
        1,
    )))
    .unwrap()
}

#[test]
fn line_styles_stroke_with_minimum_one_pixel_coverage() {
    let shape = shape_bytes(
        1,
        &[],
        &[Line {
            width: 20,
            color: [255, 0, 0, 255],
        }],
        2,
        |e| {
            e.style(Some((0, 10)), None, None, Some(1)).edge(60, 0);
        },
    );
    let image = vector(&shape);
    for x in 0..3 {
        assert_eq!(pixel(&image, x, 0), RED);
        assert_eq!(pixel(&image, x, 1), TRANSPARENT);
    }
}

#[test]
fn multiple_solid_fills_paint_in_style_order() {
    let shape = shape_bytes(1, &[[255, 0, 0, 255], [0, 0, 255, 255]], &[], 2, |e| {
        e.style(Some((0, 0)), None, Some(1), None).rect(20, 40);
        e.style(Some((20, 0)), None, Some(2), None).rect(20, 40);
    });
    let image = vector(&shape);
    assert_eq!(pixel(&image, 0, 0), RED);
    assert_eq!(pixel(&image, 1, 0), (255 >> 3) << 1 | 1);
    assert_eq!(pixel(&image, 2, 0), TRANSPARENT);
}

#[test]
fn quadratic_edges_flatten_deterministically_and_fill() {
    let shape = shape_bytes(1, &[[255, 0, 0, 255]], &[], 2, |e| {
        e.style(Some((0, 0)), None, Some(1), None)
            .curve(30, -10, 30, 10)
            .edge(0, 40)
            .edge(-60, 0)
            .edge(0, -40);
    });
    let (image, again) = (vector(&shape), vector(&shape));
    assert_eq!(
        (image.width, image.height, &image.pixels),
        (again.width, again.height, &again.pixels)
    );
    for x in 0..3 {
        for y in 0..2 {
            assert_eq!(pixel(&image, x, y), RED);
        }
    }
}

fn red_block(id: u16) -> Vec<u8> {
    shape_bytes(id, &[[255, 0, 0, 255]], &[], 2, |e| {
        e.style(Some((0, 0)), None, Some(1), None).rect(20, 40);
    })
}

#[test]
fn flash_timelines_flatten_per_frame_with_move_and_removal_semantics() {
    let body = concat(&[
        &tag(22, &red_block(1)),
        &place2(1, false, Some(1), Some((0, 0)), None),
        &tag(1, &[]),
        &place2(1, true, None, Some((40, 0)), None),
        &tag(1, &[]),
        &tag(28, &[1, 0]),
        &tag(1, &[]),
    ]);
    let result = flash_frames(&swf(&body, 3)).unwrap();
    assert_eq!(result.frames.len(), 3);
    assert_eq!(result.rate, 12.0);
    assert_eq!((result.frames[0][3], result.frames[0][2 * 4 + 3]), (255, 0));
    assert_eq!((result.frames[1][3], result.frames[1][2 * 4 + 3]), (0, 255));
    assert!(result.frames[2].iter().all(|&v| v == 0));
    let scripted = flash_frames(&swf(&concat(&[&tag(12, &[7, 0]), &body]), 3)).unwrap();
    assert_eq!(scripted.stop_frames, [json!(1)]);
    let gated = flash_frames(&swf(
        &concat(&[&tag(12, &[0x96, 4, 0, 0, 0x30, 0x31, 0, 0x1c, 0]), &body]),
        3,
    ))
    .unwrap();
    assert_eq!(gated.ignored_actions.len(), 1);
    assert_eq!(gated.ignored_actions[0]["ops"], json!(["0x96", "0x1c"]));
    assert!(
        flash_frames(&swf(&concat(&[&body, &tag(19, &[])]), 3))
            .err()
            .unwrap()
            .contains("SWF sound block without header")
    );
    assert!(
        flash_frames(&swf(&body, 2))
            .err()
            .unwrap()
            .contains("frame count disagrees")
    );
}

#[test]
fn edit_fields_record_instance_names_geometry_and_margins() {
    let mut field = edit_text(1, "text1", 40, 20, 60);
    field[2 + stage_rect().len() + 1] |= 1; // DefineEditText UseOutlines.
    let body = concat(&[
        &tag(37, &field),
        &tag(22, &red_block(2)),
        &place2(1, false, Some(2), Some((0, 0)), Some("Selected")),
        &place2(2, false, Some(1), Some((20, 20)), Some("my_txt")),
        &tag(1, &[]),
    ]);
    let result = flash_frames(&swf(&body, 1)).unwrap();
    assert_eq!(result.fields.len(), 1);
    let f = &result.fields[0];
    assert_eq!(f["embedFonts"], true);
    assert_eq!(f["name"], "my_txt");
    assert_eq!(f["variable"], "text1");
    assert_eq!(
        (f["wordWrap"].clone(), f["multiline"].clone()),
        (json!(true), json!(true))
    );
    assert_eq!(f["align"], 2);
    assert_eq!(f["leading"], 2);
    assert_eq!(f["leftMargin"], 2);
    assert_eq!(f["rightMargin"], 1);
    assert_eq!(f["indent"], 3);
    assert_eq!(
        f["bounds"],
        json!({"left": 1, "top": 1, "width": 3, "height": 2})
    );
    // Named non-field children are the setFlashProperty surface.
    assert_eq!(
        result.named_children,
        [json!({"name": "Selected", "depth": 1, "kind": "shape"})]
    );
    // A move keeps the placed name.
    let moved = concat(&[
        &tag(37, &field),
        &place2(1, false, Some(1), Some((0, 0)), Some("my_txt")),
        &tag(1, &[]),
        &place2(1, true, None, Some((40, 0)), None),
        &tag(1, &[]),
    ]);
    assert_eq!(
        flash_frames(&swf(&moved, 2)).unwrap().fields[0]["name"],
        "my_txt"
    );
}

#[test]
fn partial_alpha_fills_survive_into_rgba_frames() {
    let shape = shape_bytes(1, &[[0, 0, 255, 128]], &[], 3, |e| {
        e.style(Some((0, 0)), None, Some(1), None).rect(60, 40);
    });
    let result = flash_frames(&swf(
        &concat(&[
            &tag(32, &shape),
            &place2(1, false, Some(1), Some((0, 0)), None),
            &tag(1, &[]),
        ]),
        1,
    ))
    .unwrap();
    assert_eq!(result.frames[0][..4], [0, 0, 255, 128]);
}

#[test]
fn named_flash_objects_keep_individual_depth_pixels_and_fractional_origins() {
    let blue = shape_bytes(2, &[[0, 0, 255, 255]], &[], 2, |e| {
        e.style(Some((0, 0)), None, Some(1), None).rect(20, 40);
    });
    let body = concat(&[
        &tag(22, &red_block(1)),
        &tag(22, &blue),
        &place2(2, false, Some(1), Some((10, 0)), Some("sun")),
        &place2(3, false, Some(2), Some((20, 0)), None),
        &tag(1, &[]),
        &tag(28, &[2, 0]),
        &tag(1, &[]),
    ]);
    let mut planes = Vec::new();
    let mut sink = |p: &RasterLayer| {
        planes.push((p.width, p.height, p.rgba.clone()));
        Ok(format!("plane-{}.fdi", planes.len()))
    };
    let result = flash_frames_with_layers(&swf(&body, 2), None, Some(&mut sink)).unwrap();
    assert_eq!(result.layers.len(), 2);
    assert_eq!(result.layers[0].as_array().unwrap().len(), 2);
    assert_eq!(result.layers[1].as_array().unwrap().len(), 1);
    let sun = &result.layers[0][0];
    assert_eq!(sun["name"], "sun");
    assert_eq!(sun["channel"], 2);
    assert_eq!(sun["originX"], 32768);
    assert_eq!((planes[0].0, planes[0].1), (1, 2));
    assert_eq!(&planes[0].2[..4], &[255, 0, 0, 255]);
    assert_eq!(result.layers[0][1]["name"], "");
    assert_eq!(result.layers[0][1]["channel"], 3);
    assert_eq!(&planes[1].2[..4], &[0, 0, 255, 255]);
}

#[test]
fn live_flash_text_is_a_depth_layer_instead_of_a_baked_or_topmost_overlay() {
    let body = concat(&[
        &tag(37, &edit_text(1, "text1", 0, 0, 0)),
        &tag(22, &red_block(2)),
        &place2(1, false, Some(1), Some((0, 0)), Some("my_txt")),
        &place2(2, false, Some(2), Some((0, 0)), None),
        &tag(1, &[]),
    ]);
    let mut sink = |_: &RasterLayer| Ok("foreground.fdi".to_string());
    let result = flash_frames_with_layers(&swf(&body, 1), None, Some(&mut sink)).unwrap();
    let layers = result.layers[0].as_array().unwrap();
    assert_eq!(layers.len(), 2);
    assert_eq!(layers[0]["name"], "my_txt");
    assert!(layers[0].get("asset").is_none());
    assert_eq!(layers[1]["asset"], "foreground.fdi");
    assert_eq!(result.fields[0]["name"], layers[0]["name"]);
}

#[test]
fn simple_flash_functions_translate_by_semantics_and_unknown_bodies_remain_blocked() {
    let action =
        |code: u8, data: &[u8]| concat(&[&[code], &(data.len() as u16).to_le_bytes(), data]);
    let pool = action(0x88, &concat(&[&3u16.to_le_bytes(), b"icon\0_y\0amount\0"]));
    let push = |indexes: &[u8]| {
        action(
            0x96,
            &indexes.iter().flat_map(|&i| [8, i]).collect::<Vec<_>>(),
        )
    };
    let body = concat(&[
        &push(&[0]),
        &[0x1c],
        &push(&[1, 0]),
        &[0x1c],
        &push(&[1]),
        &[0x4e],
        &push(&[2]),
        &[0x1c, 0x47, 0x4f],
    ]);
    let function = |body: &[u8]| {
        concat(&[
            &action(
                0x9b,
                &concat(&[
                    b"shift\0",
                    &1u16.to_le_bytes(),
                    b"amount\0",
                    &(body.len() as u16).to_le_bytes(),
                ]),
            ),
            body,
        ])
    };
    let actions = concat(&[&pool, &function(&body), &[7, 0]]);
    let result = flash_frames(&swf(&concat(&[&tag(12, &actions), &tag(1, &[])]), 1)).unwrap();
    assert!(result.ignored_actions.is_empty());
    assert_eq!(result.stop_frames, [json!(1)]);
    assert_eq!(
        result.helpers,
        [json!({"name":"shift", "target":"icon", "property":"_y", "operation":1, "argument":0})]
    );
    let unsupported = concat(&[&pool, &function(&[0x30]), &[0]]);
    let result = flash_frames(&swf(&concat(&[&tag(12, &unsupported), &tag(1, &[])]), 1)).unwrap();
    assert!(result.helpers.is_empty());
    assert_eq!(result.ignored_actions.len(), 1);
    assert!(
        flash_frames(&swf(&concat(&[&tag(12, &[0x9b, 0, 0]), &tag(1, &[])]), 1))
            .err()
            .unwrap()
            .contains("function header")
    );
}

#[test]
fn fullscreen_host_request_is_explicit_but_unrelated_geturl_stays_blocked() {
    for (url, handled) in [
        ("FSCommand:fullscreen", true),
        ("https://example.invalid/", false),
    ] {
        let payload = format!("{url}\0true\0");
        let actions = concat(&[
            &[0x83],
            &(payload.len() as u16).to_le_bytes(),
            payload.as_bytes(),
            &[0],
        ]);
        let result = flash_frames(&swf(&concat(&[&tag(12, &actions), &tag(1, &[])]), 1)).unwrap();
        assert_eq!(result.host_fullscreen, handled);
        assert_eq!(result.ignored_actions.is_empty(), handled);
    }
}

#[test]
fn nested_initial_play_preserves_the_existing_playing_state() {
    let sprite = concat(&[
        &2u16.to_le_bytes(),
        &2u16.to_le_bytes(),
        &tag(12, &[6, 0]),
        &place(1, 1),
        &tag(1, &[]),
        &tag(28, &[1, 0]),
        &tag(1, &[]),
        &tag(0, &[]),
    ]);
    let body = concat(&[
        &tag(22, &red_block(1)),
        &tag(39, &sprite),
        &place(2, 1),
        &tag(1, &[]),
        &tag(1, &[]),
    ]);
    let result = flash_frames(&swf(&body, 2)).unwrap();
    assert!(result.ignored_actions.is_empty());
    assert_eq!(result.frames[0][3], 255);
    assert!(result.frames[1].iter().all(|&v| v == 0));
}

#[test]
fn live_text_keeps_fractional_local_bounds_and_each_placement_matrix() {
    let placement = |moving: bool, values: [i64; 6]| {
        let mut data = vec![if moving { 5 } else { 38 }, 1, 0];
        if !moving {
            data.extend(1u16.to_le_bytes());
        }
        data.extend(
            Writer::new()
                .put(1, 1)
                .put(20, 5)
                .put(values[0], 20)
                .put(values[3], 20)
                .put(1, 1)
                .put(20, 5)
                .put(values[1], 20)
                .put(values[2], 20)
                .put(7, 5)
                .put(values[4], 7)
                .put(values[5], 7)
                .done(),
        );
        if !moving {
            data.extend(b"label\0");
        }
        tag(26, &data)
    };
    let body = concat(&[
        &tag(37, &edit_text(1, "text", 0, 0, 0)),
        &placement(false, [98304, 16384, -32768, 49152, 10, 20]),
        &tag(1, &[]),
        &placement(true, [0, 65536, -65536, 0, 60, 0]),
        &tag(1, &[]),
    ]);
    let mut sink = |_: &RasterLayer| -> Result<String, String> {
        panic!("live text must not become a raster")
    };
    let result = flash_frames_with_layers(&swf(&body, 2), None, Some(&mut sink)).unwrap();
    assert_eq!(
        result.layers[0][0]["matrix"],
        json!([1.5, 0.25, -0.5, 0.75, 0.5, 1])
    );
    assert_eq!(result.layers[1][0]["matrix"], json!([0, 1, -1, 0, 3, 0]));
    assert_eq!(result.layers[0][0]["affine"], true);
    assert_eq!(
        result.fields[0]["localBounds"],
        json!({"left":0,"top":0,"width":3,"height":2})
    );
}

fn morph_shape(curved: bool) -> Vec<u8> {
    let mut start = Records(Writer::new());
    start.0.put(4, 4).put(4, 4);
    start.style(Some((0, 0)), None, Some(1), None).rect(20, 40);
    start.0.put(0, 6);
    let mut end = Records(Writer::new());
    end.0.put(0, 4).put(0, 4);
    end.style(Some((0, 0)), None, None, None);
    if curved {
        end.curve(30, -20, 30, 20);
    } else {
        end.edge(60, 0);
    }
    end.edge(0, 40).edge(-60, 0).edge(0, -40);
    end.0.put(0, 6);
    let styles = [1, 0, 255, 0, 0, 255, 0, 0, 255, 255, 0];
    let start = concat(&[&styles, &start.0.done()]);
    concat(&[
        &1u16.to_le_bytes(),
        &stage_rect(),
        &stage_rect(),
        &(start.len() as u32).to_le_bytes(),
        &start,
        &end.0.done(),
    ])
}

fn morph_place(ratio: u16, moving: bool) -> Vec<u8> {
    let mut data = vec![if moving { 17 } else { 22 }, 1, 0];
    if !moving {
        data.extend(1u16.to_le_bytes());
        data.extend(Writer::new().put(0, 1).put(0, 1).put(0, 5).done());
    }
    data.extend(ratio.to_le_bytes());
    tag(26, &data)
}

#[test]
fn morph_shapes_interpolate_geometry_colors_and_straight_to_curve_pairs() {
    for curved in [false, true] {
        let body = concat(&[
            &tag(46, &morph_shape(curved)),
            &morph_place(0, false),
            &tag(1, &[]),
            &morph_place(32768, true),
            &tag(1, &[]),
            &morph_place(65535, true),
            &tag(1, &[]),
            &place2(1, true, None, Some((0, 0)), None),
            &tag(1, &[]),
        ]);
        let result = flash_frames(&swf(&body, 4)).unwrap();
        assert_eq!(&result.frames[0][..4], &[255, 0, 0, 255]);
        assert_eq!(result.frames[0][7], 0);
        assert_eq!(&result.frames[1][4..8], &[127, 0, 128, 255]);
        assert_eq!(&result.frames[2][8..12], &[0, 0, 255, 255]);
        assert_eq!(result.frames[2], result.frames[3]); // omitted ratio retains it
    }
}

#[test]
fn malformed_morph_edge_offsets_and_counts_fail_explicitly() {
    let mut shape = morph_shape(false);
    shape.pop();
    assert!(
        flash_frames(&swf(
            &concat(&[&tag(46, &shape), &morph_place(0, false), &tag(1, &[])]),
            1
        ))
        .is_err()
    );
    let mut shape = morph_shape(false);
    let offset = 2 + stage_rect().len() * 2;
    shape[offset..offset + 4].fill(255);
    assert!(
        flash_frames(&swf(&concat(&[&tag(46, &shape), &tag(1, &[])]), 1))
            .err()
            .unwrap()
            .contains("edge offset")
    );
}

#[test]
fn empty_morphs_with_zero_padded_rectangles_remain_empty() {
    let mut shape = vec![0; 48];
    shape[0] = 1;
    shape[2] = 8;
    shape[20] = 8;
    shape[38] = 4;
    let result = flash_frames(&swf(
        &concat(&[&tag(46, &shape), &morph_place(32768, false), &tag(1, &[])]),
        1,
    ))
    .unwrap();
    assert!(result.frames[0].iter().all(|&v| v == 0));
    shape[10] = 1;
    assert!(flash_frames(&swf(&concat(&[&tag(46, &shape), &tag(1, &[])]), 1)).is_err());
}

#[test]
fn zero_area_vector_shapes_remain_transparent_and_nonempty_content_is_rejected() {
    let empty = shape_bytes(1, &[], &[], 2, |_| {});
    for (shape, valid) in [(&empty, true), (&red_block(1), false)] {
        let mut data = concat(&[
            b"FWS\x01\0\0\0\0",
            &[0],
            &[0, 12],
            &1u16.to_le_bytes(),
            &tag(22, shape),
            &place(1, 1),
            &tag(1, &[]),
            &tag(0, &[]),
        ]);
        let size = data.len();
        le32(&mut data, 4, size as i64);
        let result = flash_frames(&data);
        if valid {
            let r = result.unwrap();
            assert_eq!((r.width, r.height), (1, 1));
            assert_eq!(r.frames[0], [0, 0, 0, 0]);
        } else {
            assert!(result.err().unwrap().contains("zero-area"));
        }
    }
}

#[test]
fn nested_stop_holds_its_own_frame_and_descendant_timelines_keep_advancing() {
    let sprite = |id: u16, frames: u16, body: &[u8]| {
        tag(
            39,
            &concat(&[&id.to_le_bytes(), &frames.to_le_bytes(), body, &tag(0, &[])]),
        )
    };
    let child = sprite(
        2,
        2,
        &concat(&[&place(1, 1), &tag(1, &[]), &tag(28, &[1, 0]), &tag(1, &[])]),
    );
    let stopped_parent = sprite(
        3,
        2,
        &concat(&[&tag(1, &[]), &place(2, 1), &tag(12, &[7, 0]), &tag(1, &[])]),
    );
    let body = concat(&[
        &tag(22, &red_block(1)),
        &child,
        &stopped_parent,
        &place(3, 1),
        &tag(1, &[]).repeat(5),
    ]);
    let result = flash_frames(&swf(&body, 5)).unwrap();
    assert!(result.ignored_actions.is_empty());
    assert_eq!(
        result.frames.iter().map(|f| f[3]).collect::<Vec<_>>(),
        [0, 255, 0, 255, 0]
    );
}

#[test]
fn nested_play_after_stop_on_the_same_frame_resumes_in_action_order() {
    let sprite = concat(&[
        &2u16.to_le_bytes(),
        &2u16.to_le_bytes(),
        &place(1, 1),
        &tag(12, &[7, 0]),
        &tag(12, &[6, 0]),
        &tag(1, &[]),
        &tag(28, &[1, 0]),
        &tag(1, &[]),
        &tag(0, &[]),
    ]);
    let body = concat(&[
        &tag(22, &red_block(1)),
        &tag(39, &sprite),
        &place(2, 1),
        &tag(1, &[]).repeat(4),
    ]);
    let result = flash_frames(&swf(&body, 4)).unwrap();
    assert!(result.ignored_actions.is_empty());
    assert_eq!(
        result.frames.iter().map(|f| f[3]).collect::<Vec<_>>(),
        [255, 0, 255, 0]
    );
}

#[test]
fn registered_text_helpers_lower_semantics_and_preserve_receiver_scope() {
    let action =
        |code: u8, data: &[u8]| concat(&[&[code], &(data.len() as u16).to_le_bytes(), data]);
    let pool = action(0x88, &concat(&[&8u16.to_le_bytes(),
        b"resizeLabel\0caption\0getTextFormat\0size\0setTextFormat\0decodeLabel\0text\0unescape\0"]));
    let push = |data: &[u8]| action(0x96, data);
    let function = |body: &[u8], method: bool| {
        let header = concat(&[
            &[0],
            &1u16.to_le_bytes(),
            &[3],
            &(if method { 0x29u16 } else { 0x2au16 }).to_le_bytes(),
            &[2],
            b"argument\0",
            &(body.len() as u16).to_le_bytes(),
        ]);
        concat(&[&action(0x8e, &header), body])
    };
    let font_body = concat(&[
        &push(&[7, 0, 0, 0, 0, 8, 1]),
        &[0x1c],
        &push(&[8, 2]),
        &[0x52],
        &action(0x87, &[1]),
        &[0x17],
        &push(&[4, 1, 8, 3, 4, 2]),
        &[0x4f],
        &push(&[4, 1, 7, 1, 0, 0, 0, 8, 1]),
        &[0x1c],
        &push(&[8, 4]),
        &[0x52, 0x17],
    ]);
    let text_body = concat(&[
        &push(&[4, 1, 8, 6, 4, 2, 7, 1, 0, 0, 0, 8, 7]),
        &[0x3d, 0x4f],
    ]);
    let script = concat(&[
        &pool,
        &[7],
        &push(&[8, 0]),
        &function(&font_body, false),
        &[0x1d],
        &push(&[8, 1]),
        &[0x1c],
        &push(&[8, 5]),
        &function(&text_body, true),
        &[0x4f, 0],
    ]);
    let result = flash_frames(&swf(&concat(&[&tag(12, &script), &tag(1, &[])]), 1)).unwrap();
    assert!(result.ignored_actions.is_empty());
    assert_eq!(result.stop_frames, [json!(1)]);
    assert_eq!(result.helpers,json!([
        {"name":"resizeLabel","target":"caption","property":"size","operation":3,"argument":0,"method":false},
        {"name":"decodeLabel","target":"caption","property":"text","operation":4,"argument":0,"method":true}
    ]).as_array().unwrap().to_vec());
    let mut bad = font_body.clone();
    bad.push(0x30); // A random side effect is not a proven text helper.
    let script = concat(&[&pool, &push(&[8, 0]), &function(&bad, false), &[0x1d, 0]]);
    let result = flash_frames(&swf(&concat(&[&tag(12, &script), &tag(1, &[])]), 1)).unwrap();
    assert!(result.helpers.is_empty());
    assert_eq!(result.ignored_actions.len(), 1);
}

fn bevel_placement(id: u16, depth: u16, strength: u16) -> Vec<u8> {
    let matrix = Writer::new()
        .put(0, 1)
        .put(0, 1)
        .put(1, 5)
        .put(0, 1)
        .put(0, 1)
        .done();
    let mut data = vec![6, 1];
    data.extend(depth.to_le_bytes());
    data.extend(id.to_le_bytes());
    data.extend(matrix);
    data.extend([1, 7, 3]);
    data.extend([255, 0, 0, 255, 0, 0, 0, 0, 0, 0, 255, 255]);
    data.extend([0, 128, 255]);
    for value in [0i32, 0, 0, 65536] {
        data.extend(value.to_le_bytes());
    }
    data.extend(strength.to_le_bytes());
    data.push(0xa1);
    tag(70, &data)
}
#[test]
fn gradient_bevel_is_applied_at_placement_depth_and_inherited_by_move() {
    let shape = shape_bytes(1, &[[0, 255, 0, 255]], &[], 3, |e| {
        e.style(Some((0, 0)), None, Some(1), None).rect(40, 40);
    });
    let cover = shape_bytes(2, &[[255, 255, 0, 255]], &[], 3, |e| {
        e.style(Some((0, 0)), None, Some(1), None).rect(20, 40);
    });
    let body = concat(&[
        &tag(32, &shape),
        &tag(32, &cover),
        &bevel_placement(1, 1, 256),
        &place(2, 2),
        &tag(1, &[]),
        &tag(28, &2u16.to_le_bytes()),
        &place2(1, true, None, Some((20, 0)), None),
        &tag(1, &[]),
    ]);
    let result = flash_frames(&swf(&body, 2)).unwrap();
    assert_eq!(result.effects.applied_filters, 1);
    assert_eq!(result.effects.filters, 0);
    assert_eq!(&result.frames[0][0..8], &[255, 255, 0, 255, 255, 0, 0, 255]);
    assert_eq!(
        &result.frames[1][0..12],
        &[0, 0, 0, 0, 0, 0, 255, 255, 255, 0, 0, 255]
    );
}
#[test]
fn gradient_bevel_filters_whole_subtree_instead_of_each_child_separately() {
    let shape = shape_bytes(1, &[[0, 255, 0, 255]], &[], 3, |e| {
        e.style(Some((0, 0)), None, Some(1), None).rect(20, 40);
    });
    let sprite = concat(&[
        &2u16.to_le_bytes(),
        &1u16.to_le_bytes(),
        &place(1, 1),
        &place2(2, false, Some(1), Some((20, 0)), None),
        &tag(1, &[]),
        &tag(0, &[]),
    ]);
    let body = concat(&[
        &tag(32, &shape),
        &tag(39, &sprite),
        &bevel_placement(2, 1, 256),
        &tag(1, &[]),
    ]);
    let result = flash_frames(&swf(&body, 1)).unwrap();
    assert_eq!(&result.frames[0][0..8], &[0, 0, 255, 255, 255, 0, 0, 255]);
}

fn random_phase_action(guard: &str, count: u32) -> Vec<u8> {
    let action = |code: usize, data: &[u8]| {
        let mut out = vec![code as u8];
        if code >= 128 {
            out.extend((data.len() as u16).to_le_bytes());
        }
        out.extend(data);
        out
    };
    let mut number = vec![7];
    number.extend(count.to_le_bytes());
    let body = concat(&[
        &action(0x96, &number),
        &[0x30],
        &action(0x96, &[7, 1, 0, 0, 0]),
        &[0x47],
        &action(0x9f, &[1]),
        &action(0x96, &[8, 0, 5, 1]),
        &[0x1d],
    ]);
    concat(&[
        &action(0x88, &concat(&[&[1, 0], &latin1(&format!("{guard}\0"))])),
        &action(0x96, &[8, 0]),
        &[0x1c],
        &action(0x96, &[3]),
        &[0x49, 0x12],
        &action(0x9d, &(body.len() as i16).to_le_bytes()),
        &body,
        &[0],
    ])
}
fn random_phase_movie(root: &[u8], action: &[u8], root_frames: u16) -> Vec<u8> {
    let sprite = concat(&[
        &2u16.to_le_bytes(),
        &2u16.to_le_bytes(),
        &place(1, 1),
        &tag(12, action),
        &tag(1, &[]),
        &tag(28, &[1, 0]),
        &tag(1, &[]),
        &tag(0, &[]),
    ]);
    swf(
        &concat(&[&tag(22, &red_block(1)), &tag(39, &sprite), root]),
        root_frames,
    )
}
#[test]
fn once_only_random_child_phase_preserves_every_variant_per_root_pose() {
    let body = concat(&[
        &place(2, 1),
        &tag(1, &[]),
        &place2(1, true, None, Some((20, 0)), None),
        &tag(1, &[]),
        &tag(1, &[]),
    ]);
    let result = flash_frames(&random_phase_movie(
        &body,
        &random_phase_action("arbitrary_guard", 2),
        3,
    ))
    .unwrap();
    assert!(result.ignored_actions.is_empty());
    assert_eq!(result.variant_count, 2);
    assert_eq!(result.frames.len(), 6);
    let opaque = |frame: usize| result.frames[frame].chunks_exact(4).any(|p| p[3] != 0);
    assert_eq!(
        (0..6).map(opaque).collect::<Vec<_>>(),
        vec![true, false, false, true, true, false]
    );
    assert_eq!(result.frames[3][3], 0); // Root translation still changes each phase variant.
    assert_eq!(result.frames[3][7], 255);
}
#[test]
fn random_phase_rejects_lifetime_changes_and_unproven_programs() {
    let action = random_phase_action("go", 2);
    for body in [
        concat(&[&place(2, 1), &tag(1, &[]), &tag(28, &[1, 0]), &tag(1, &[])]),
        concat(&[&place(2, 1), &tag(1, &[]), &place(2, 1), &tag(1, &[])]),
        concat(&[&tag(1, &[]), &place(2, 1), &tag(1, &[])]),
    ] {
        assert!(
            flash_frames(&random_phase_movie(&body, &action, 2))
                .err()
                .unwrap()
                .contains("random initial phase")
        );
    }
    let root = concat(&[&place(2, 1), &tag(1, &[])]);
    let mut wrong = action.clone();
    let jump = wrong.iter().position(|&b| b == 0x9d).unwrap();
    wrong[jump + 3] -= 1;
    let result = flash_frames(&random_phase_movie(&root, &wrong, 1)).unwrap();
    assert_eq!(result.variant_count, 1);
    assert!(!result.ignored_actions.is_empty());
    assert!(flash_frames(&random_phase_movie(&root, &random_phase_action("go", 3), 1)).is_err());
}

#[test]
fn clip_actions_alone_require_retained_root_and_nested_timelines() {
    let shape = shape_bytes(1, &[[255, 0, 0, 255]], &[], 3, |e| {
        e.style(Some((0, 0)), None, Some(1), None).rect(40, 40);
    });
    // SWF6 ClipEventLoad invokes Stop. There are no sound/button/export tags
    // to select the interactive route on behalf of these placements.
    let actions = concat(&[
        &[0, 0],
        &128u32.to_le_bytes(),
        &128u32.to_le_bytes(),
        &2u32.to_le_bytes(),
        &[7, 0],
        &0u32.to_le_bytes(),
    ]);
    let matrix = Writer::new()
        .put(0, 1)
        .put(0, 1)
        .put(1, 5)
        .put(0, 1)
        .put(0, 1)
        .done();
    let placement = tag(26, &concat(&[&[134, 1, 0, 1, 0], &matrix, &actions]));
    for nested in [false, true] {
        let child = tag(
            39,
            &concat(&[&[2, 0, 1, 0], &placement, &tag(1, &[]), &tag(0, &[])]),
        );
        let root_placement = if nested { place(2, 1) } else { Vec::new() };
        let body = concat(&[
            &tag(32, &shape),
            if nested { &child } else { &placement },
            &root_placement,
            &tag(1, &[]),
        ]);
        let mut source = swf(&body, 1);
        source[3] = 6;
        let mut sink = |_: &RasterLayer| Ok("plane.fda".into());
        let result = flash_frames_with_layers(&source, None, Some(&mut sink)).unwrap();
        let runtime = result.runtime.unwrap();
        let layer = if nested {
            &runtime["clips"][1]["frames"][0][0]
        } else {
            &result.layers[0][0]
        };
        assert_eq!(layer["actions"][0]["event"], 128);
        assert_eq!(layer["actions"][0]["code"], json!([7, 0]));
    }
}

#[test]
fn retained_flash_preserves_clips_buttons_actions_sound_and_placement_lifetime() {
    let shape = shape_bytes(1, &[[255, 0, 0, 255]], &[], 3, |e| {
        e.style(Some((0, 0)), None, Some(1), None).rect(40, 40);
    });
    let child = concat(&[
        &2u16.to_le_bytes(),
        &2u16.to_le_bytes(),
        &place(1, 1),
        &tag(12, &[7, 0]),
        &tag(15, &[4, 0, 0]),
        &tag(43, b"start\0"),
        &tag(1, &[]),
        &tag(12, &[6, 0]),
        &tag(1, &[]),
        &tag(0, &[]),
    ]);
    // DefineButton2: all four states, identity matrix/color transform, press action.
    let matrix = Writer::new()
        .put(0, 1)
        .put(0, 1)
        .put(1, 5)
        .put(0, 1)
        .put(0, 1)
        .done();
    let cx = Writer::new().put(0, 1).put(0, 1).put(0, 4).done();
    let records = concat(&[&[15, 1, 0, 1, 0], &matrix, &cx, &[0]]);
    let button = concat(&[
        &[3, 0, 0],
        &((records.len() + 2) as u16).to_le_bytes(),
        &records,
        &[0, 0, 4, 0, 7, 0],
    ]);
    let sound = concat(&[
        &[4, 0, 0x3e],
        &2u32.to_le_bytes(),
        &1000i16.to_le_bytes(),
        &(-1000i16).to_le_bytes(),
    ]);
    let body = concat(&[
        &tag(32, &shape),
        &tag(14, &sound),
        &tag(39, &child),
        &tag(34, &button),
        &tag(56, &[1, 0, 2, 0, b'c', 0]),
        &tag(59, &[2, 0, 7, 0]),
        &place2(1, false, Some(2), Some((10, 20)), Some("child")),
        &place(3, 2),
        &tag(1, &[]),
        &place2(1, true, None, Some((20, 30)), None),
        &tag(1, &[]),
        &tag(28, &1u16.to_le_bytes()),
        &place2(1, false, Some(2), Some((10, 20)), Some("again")),
        &tag(1, &[]),
    ]);
    let mut assets = Vec::new();
    let mut sink = |p: &RasterLayer| {
        assets.push(p.rgba.clone());
        Ok(format!("layer{}.fda", assets.len()))
    };
    let result = flash_frames_with_layers(&swf(&body, 3), None, Some(&mut sink)).unwrap();
    assert_eq!(result.frames.len(), 3);
    assert_eq!(result.layers.len(), 3);
    assert_eq!(result.sounds.len(), 1);
    let runtime = result.runtime.unwrap();
    let child = &runtime["clips"][1];
    assert_eq!(child["id"], 2);
    assert_eq!(child["frameCount"], 2);
    assert_eq!(runtime["initializers"][0]["frame"], 1);
    assert_eq!(runtime["initializers"][0]["clipId"], 2);
    assert_eq!(child["actions"][0]["code"], json!([7, 0]));
    assert_eq!(child["actions"][1]["code"], json!([6, 0]));
    assert_eq!(child["soundEvents"][0]["sound"], 4);
    assert_eq!(child["labels"][0]["name"], "start");
    assert_eq!(runtime["buttons"][0]["states"].as_array().unwrap().len(), 4);
    assert_eq!(runtime["buttons"][0]["actions"][0]["event"], 4);
    assert_eq!(runtime["exports"][0], json!({"id":2,"name":"c"}));
    assert_eq!(result.layers[0][0]["clipId"], 2);
    assert_eq!(
        result.layers[0][0]["matrix"],
        json!([1.0, 0.0, 0.0, 1.0, 0.5, 1.0])
    );
    assert_eq!(
        result.layers[0][0]["placementGeneration"],
        result.layers[1][0]["placementGeneration"]
    );
    assert_ne!(
        result.layers[0][0]["placementGeneration"],
        result.layers[2][0]["placementGeneration"]
    );
}
#[test]
fn exact_font_diagnostics_preserve_quadratic_controls_without_record_alignment() {
    let mut records = Records(Writer::new());
    records.0.put(4, 4).put(4, 4);
    records
        .style(Some((4, -6)), None, Some(1), None)
        .curve(10, -20, 30, 20)
        .edge(-40, 0);
    records.0.put(0, 6);
    let glyph = records.0.done();
    let mut font = vec![1, 0, 0, 0, 4];
    font.extend(b"Test");
    font.extend(1u16.to_le_bytes());
    font.extend(4u16.to_le_bytes());
    font.extend((4 + glyph.len() as u16).to_le_bytes());
    font.extend(glyph);
    font.push(b'A');
    let data = swf(&concat(&[&tag(48, &font), &tag(1, &[])]), 1);
    let fonts = director64_aot::convert::swf::font_outlines(&data).unwrap();
    assert_eq!(
        fonts[0]["glyphs"][0]["commands"],
        json!([
            ["M", 4.0, -6.0],
            ["Q", 14.0, -26.0, 44.0, -6.0],
            ["L", 4.0, -6.0]
        ])
    );
    assert_eq!(fonts[0]["glyphs"][0]["codepoint"], 65);
    assert_eq!(fonts[0]["unitsPerEm"], 1024.0);
    assert_eq!(fonts[0]["hasLayout"], false);
    assert!(fonts[0]["glyphs"][0]["advance"].is_null());
}

#[test]
fn geometric_masks_ignore_fill_and_placement_alpha_and_exclude_strokes() {
    for alpha in [0, 1, 128, 255] {
        for stroke_only in [false, true] {
            for nested in [false, true] {
                let fills = [[0, 0, 255, alpha]];
                let lines = [Line {
                    width: 20,
                    color: [0, 0, 255, 255],
                }];
                let shape = shape_bytes(
                    1,
                    if stroke_only { &[] } else { &fills },
                    if stroke_only { &lines } else { &[] },
                    3,
                    |e| {
                        e.style(
                            Some((0, 0)),
                            None,
                            if stroke_only { None } else { Some(1) },
                            if stroke_only { Some(1) } else { None },
                        )
                        .rect(40, 40);
                    },
                );
                let content = shape_bytes(2, &[[255, 0, 0, 255]], &[], 3, |e| {
                    e.style(Some((0, 0)), None, Some(1), None).rect(60, 60);
                });
                let child = tag(
                    39,
                    &concat(&[&[3, 0, 1, 0], &place(1, 1), &tag(1, &[]), &tag(0, &[])]),
                );
                let matrix = Writer::new()
                    .put(0, 1)
                    .put(0, 1)
                    .put(1, 5)
                    .put(0, 1)
                    .put(0, 1)
                    .done();
                // RGB multipliers unity, alpha multiplier zero. Neither this nor a
                // zero-alpha fill makes an ordinary Flash geometric mask disappear.
                let cx = Writer::new()
                    .put(0, 1)
                    .put(1, 1)
                    .put(10, 4)
                    .put(256, 10)
                    .put(256, 10)
                    .put(256, 10)
                    .put(0, 10)
                    .done();
                let mask = tag(
                    26,
                    &concat(&[
                        &[78, 1, 0],
                        &if nested { 3u16 } else { 1 }.to_le_bytes(),
                        &matrix,
                        &cx,
                        &2u16.to_le_bytes(),
                    ]),
                );
                let body = concat(&[
                    &tag(32, &shape),
                    &tag(32, &content),
                    &child,
                    &mask,
                    &place(2, 2),
                    &tag(1, &[]),
                ]);
                let result = flash_frames(&swf(&body, 1)).unwrap();
                let expected = if stroke_only {
                    &[0, 0, 0, 0]
                } else {
                    &[255, 0, 0, 255]
                };
                assert_eq!(
                    &result.frames[0][..4],
                    expected,
                    "alpha {alpha}, stroke {stroke_only}, nested {nested}"
                );
                assert_eq!(
                    result.frames[0][2 * 4 + 3],
                    0,
                    "content outside mask must remain transparent"
                );
                // Retained/exported definitions carry both regular pixels and an
                // explicit geometric plane, including leaves nested inside clips.
                let retained = concat(&[&body, &tag(56, &[1, 0, 3, 0, b'c', 0])]);
                let mut assets = Vec::new();
                let mut sink = |p: &RasterLayer| {
                    assets.push(p.rgba.clone());
                    Ok(format!("{}.fda", assets.len() - 1))
                };
                let result =
                    flash_frames_with_layers(&swf(&retained, 1), None, Some(&mut sink)).unwrap();
                let runtime = result.runtime.unwrap();
                let layer = if nested {
                    &runtime["clips"][1]["frames"][0][0]
                } else {
                    &result.layers[0][0]
                };
                let index = layer["maskAsset"]
                    .as_str()
                    .unwrap()
                    .trim_end_matches(".fda")
                    .parse::<usize>()
                    .unwrap();
                assert_eq!(assets[index][3], if stroke_only { 0 } else { 255 });
                if alpha == 0 && !stroke_only {
                    assert_ne!(layer["asset"], layer["maskAsset"]);
                }
            }
        }
    }
}

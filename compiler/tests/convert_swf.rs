//! Static SWF rasterizing and Flash timelines (was
//! tests/node/director-swf.test.mjs).

mod common;

use common::*;
use director64_aot::convert::swf::{flash_frames, vector_shape_pixels, VectorShape};
use serde_json::json;

struct Line {
    width: u16,
    color: [u8; 4],
}

/// Shape records over 7-bit signed deltas, as the JS fixture builder wrote them.
struct Records(Writer);
impl Records {
    fn style(&mut self, mv: Option<(i64, i64)>, f0: Option<i64>, f1: Option<i64>, ln: Option<i64>) -> &mut Self {
        let flags = i64::from(mv.is_some()) | if f0.is_some() { 2 } else { 0 } | if f1.is_some() { 4 } else { 0 }
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
        self.0.put(1, 1).put(1, 1).put(5, 4).put(1, 1).put(dx, 7).put(dy, 7);
        self
    }
    fn curve(&mut self, cx: i64, cy: i64, ax: i64, ay: i64) -> &mut Self {
        self.0.put(1, 1).put(0, 1).put(5, 4).put(cx, 7).put(cy, 7).put(ax, 7).put(ay, 7);
        self
    }
    fn rect(&mut self, width: i64, height: i64) -> &mut Self {
        for (dx, dy) in [(width, 0), (0, height), (-width, 0), (0, -height)] {
            self.edge(dx, dy);
        }
        self
    }
}

fn shape_bytes(id: u16, fills: &[[u8; 4]], lines: &[Line], version: u32, records: impl FnOnce(&mut Records)) -> Vec<u8> {
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
    let matrix = Writer::new().put(0, 1).put(0, 1).put(7, 5).put(0, 7).put(0, 7).done();
    tag(4, &concat(&[&id.to_le_bytes(), &depth.to_le_bytes(), &matrix]))
}
fn place2(depth: u16, mv: bool, id: Option<u16>, at: Option<(i64, i64)>, name: Option<&str>) -> Vec<u8> {
    let flags = u8::from(mv) | if id.is_some() { 2 } else { 0 } | if at.is_some() { 4 } else { 0 }
        | if name.is_some() { 32 } else { 0 };
    let mut out = vec![flags];
    out.extend(depth.to_le_bytes());
    if let Some(id) = id {
        out.extend(id.to_le_bytes());
    }
    if let Some((tx, ty)) = at {
        out.extend(Writer::new().put(0, 1).put(0, 1).put(7, 5).put(tx, 7).put(ty, 7).done());
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
    concat(&[&id.to_le_bytes(), &stage_rect(), &flags.to_le_bytes(), &layout, &latin1(&format!("{variable}\0"))])
}
fn swf(body: &[u8], frames: u16) -> Vec<u8> {
    let mut data = concat(&[&latin1("FWS\x01\0\0\0\0"), &stage_rect(), &[0, 12], &frames.to_le_bytes(), body, &tag(0, &[])]);
    let length = data.len() as i64;
    le32(&mut data, 4, length);
    data
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
    vector_shape_pixels(&envelope(&swf(&concat(&[&tag(22, shape), &place(1, 1), &tag(1, &[])]), 1))).unwrap()
}

#[test]
fn line_styles_stroke_with_minimum_one_pixel_coverage() {
    let shape = shape_bytes(1, &[], &[Line { width: 20, color: [255, 0, 0, 255] }], 2, |e| {
        e.style(Some((0, 10)), None, None, Some(1)).edge(60, 0);
    });
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
        e.style(Some((0, 0)), None, Some(1), None).curve(30, -10, 30, 10).edge(0, 40).edge(-60, 0).edge(0, -40);
    });
    let (image, again) = (vector(&shape), vector(&shape));
    assert_eq!((image.width, image.height, &image.pixels), (again.width, again.height, &again.pixels));
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
    let body = concat(&[&tag(22, &red_block(1)), &place2(1, false, Some(1), Some((0, 0)), None), &tag(1, &[]),
        &place2(1, true, None, Some((40, 0)), None), &tag(1, &[]), &tag(28, &[1, 0]), &tag(1, &[])]);
    let result = flash_frames(&swf(&body, 3)).unwrap();
    assert_eq!(result.frames.len(), 3);
    assert_eq!(result.rate, 12.0);
    assert_eq!((result.frames[0][3], result.frames[0][2 * 4 + 3]), (255, 0));
    assert_eq!((result.frames[1][3], result.frames[1][2 * 4 + 3]), (0, 255));
    assert!(result.frames[2].iter().all(|&v| v == 0));
    let scripted = flash_frames(&swf(&concat(&[&tag(12, &[7, 0]), &body]), 3)).unwrap();
    assert_eq!(scripted.stop_frames, [json!(1)]);
    let gated =
        flash_frames(&swf(&concat(&[&tag(12, &[0x96, 4, 0, 0, 0x30, 0x31, 0, 0x1c, 0]), &body]), 3)).unwrap();
    assert_eq!(gated.ignored_actions.len(), 1);
    assert_eq!(gated.ignored_actions[0]["ops"], json!(["0x96", "0x1c"]));
    assert!(flash_frames(&swf(&concat(&[&body, &tag(6, &[])]), 3)).err().unwrap().contains("unsupported static SWF tag 6"));
    assert!(flash_frames(&swf(&body, 2)).err().unwrap().contains("frame count disagrees"));
}

#[test]
fn edit_fields_record_instance_names_geometry_and_margins() {
    let field = edit_text(1, "text1", 40, 20, 60);
    let body = concat(&[&tag(37, &field), &tag(22, &red_block(2)),
        &place2(1, false, Some(2), Some((0, 0)), Some("Selected")),
        &place2(2, false, Some(1), Some((20, 20)), Some("my_txt")), &tag(1, &[])]);
    let result = flash_frames(&swf(&body, 1)).unwrap();
    assert_eq!(result.fields.len(), 1);
    let f = &result.fields[0];
    assert_eq!(f["name"], "my_txt");
    assert_eq!(f["variable"], "text1");
    assert_eq!((f["wordWrap"].clone(), f["multiline"].clone()), (json!(true), json!(true)));
    assert_eq!(f["align"], 2);
    assert_eq!(f["leading"], 2);
    assert_eq!(f["leftMargin"], 2);
    assert_eq!(f["rightMargin"], 1);
    assert_eq!(f["indent"], 3);
    assert_eq!(f["bounds"], json!({"left": 1, "top": 1, "width": 3, "height": 2}));
    // Named non-field children are the setFlashProperty surface.
    assert_eq!(result.named_children, [json!({"name": "Selected", "depth": 1, "kind": "shape"})]);
    // A move keeps the placed name.
    let moved = concat(&[&tag(37, &field), &place2(1, false, Some(1), Some((0, 0)), Some("my_txt")), &tag(1, &[]),
        &place2(1, true, None, Some((40, 0)), None), &tag(1, &[])]);
    assert_eq!(flash_frames(&swf(&moved, 2)).unwrap().fields[0]["name"], "my_txt");
}

#[test]
fn partial_alpha_fills_survive_into_rgba_frames() {
    let shape = shape_bytes(1, &[[0, 0, 255, 128]], &[], 3, |e| {
        e.style(Some((0, 0)), None, Some(1), None).rect(60, 40);
    });
    let result = flash_frames(&swf(&concat(&[&tag(32, &shape), &place2(1, false, Some(1), Some((0, 0)), None), &tag(1, &[])]), 1)).unwrap();
    assert_eq!(result.frames[0][..4], [0, 0, 255, 128]);
}

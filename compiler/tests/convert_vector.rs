//! Authored source-free FLSH descriptor; no game payload bytes.
use director64_aot::convert::vector::vector_shape_model;
use serde_json::json;
fn word(bytes: &mut Vec<u8>, value: u32) {
    bytes.extend_from_slice(&value.to_be_bytes());
}
fn descriptor() -> Vec<u8> {
    let mut b = vec![0u8; 228];
    b[..4].copy_from_slice(b"FLSH");
    b[8..12].copy_from_slice(&23u32.to_be_bytes());
    b[128..132].copy_from_slice(&1u32.to_be_bytes());
    b[132..136].copy_from_slice(&2.5f32.to_be_bytes());
    b[136..140].copy_from_slice(&1u32.to_be_bytes());
    for (at, rgb) in [(164, [0x12u32, 0x34, 0x56]), (180, [0xa1, 0xb2, 0xc3])] {
        b[at..at + 4].copy_from_slice(&18u32.to_be_bytes());
        for (i, value) in rgb.iter().enumerate() {
            b[at + 4 + i * 4..at + 8 + i * 4].copy_from_slice(&value.to_be_bytes());
        }
    }
    word(&mut b, 7);
    word(&mut b, 3);
    for (i, (x, y)) in [(-8i32, 3i32), (9, -4), (2, 11)].into_iter().enumerate() {
        word(&mut b, 10);
        word(&mut b, 1);
        word(&mut b, 2);
        if i == 0 {
            word(&mut b, 6);
            b.extend_from_slice(b"vertex");
        } else {
            word(&mut b, 0x80000000);
        }
        word(&mut b, 8);
        word(&mut b, y as u32);
        word(&mut b, x as u32);
    }
    let len = b.len() as u32;
    b[4..8].copy_from_slice(&len.to_be_bytes());
    b
}
#[test]
fn source_polygon_retains_signed_xy_color_and_fractional_stroke() {
    let p = descriptor();
    let v = vector_shape_model(&p).unwrap().unwrap();
    assert_eq!(
        v,
        json!({"vertices":[[-8,3],[9,-4],[2,11]],"closed":true,"filled":true,
        "strokeWidth":2.5,"strokeColor":0x123456,"fillColor":0xa1b2c3})
    );
}
#[test]
fn unsupported_curves_and_versions_remain_static_and_bad_values_fail() {
    let mut p = descriptor();
    p[240..244].copy_from_slice(&3u32.to_be_bytes());
    assert!(vector_shape_model(&p).unwrap_err().contains("property key"));
    let mut p = descriptor();
    p[8..12].copy_from_slice(&27u32.to_be_bytes());
    assert!(vector_shape_model(&p).unwrap().is_none());
    let mut p = descriptor();
    p[132..136].copy_from_slice(&f32::NAN.to_be_bytes());
    assert!(vector_shape_model(&p).unwrap_err().contains("stroke/fill"));
    let mut p = descriptor();
    p[168..172].copy_from_slice(&256u32.to_be_bytes());
    assert!(
        vector_shape_model(&p)
            .unwrap_err()
            .contains("color channel")
    );
    let mut p = descriptor();
    p.pop();
    assert!(vector_shape_model(&p).unwrap_err().contains("length"));
}

fn v26(mut bytes: Vec<u8>, discriminator: &str) -> Vec<u8> {
    bytes[8..12].copy_from_slice(&26u32.to_be_bytes());
    word(&mut bytes, 1);
    word(&mut bytes, 2);
    word(&mut bytes, discriminator.len() as u32);
    bytes.extend_from_slice(discriminator.as_bytes());
    let len = bytes.len() as u32;
    bytes[4..8].copy_from_slice(&len.to_be_bytes());
    bytes
}
#[test]
fn director_ten_descriptor_retains_authored_points_and_style_with_checked_trailer() {
    let source = descriptor();
    let expected = vector_shape_model(&source).unwrap();
    let modern = v26(source, "vectorShape");
    assert_eq!(vector_shape_model(&modern).unwrap(), expected);
    assert_eq!(
        vector_shape_model(&v26(descriptor(), "none")).unwrap(),
        expected
    );
    assert!(
        vector_shape_model(&v26(descriptor(), "unknown"))
            .unwrap()
            .is_none()
    );
    let mut bad = modern.clone();
    bad.pop();
    let len = bad.len() as u32;
    bad[4..8].copy_from_slice(&len.to_be_bytes());
    assert!(
        vector_shape_model(&bad)
            .unwrap_err()
            .contains("truncated vector symbol")
    );
    let mut bad = modern;
    let end = descriptor().len();
    bad[end..end + 4].copy_from_slice(&2u32.to_be_bytes());
    assert!(
        vector_shape_model(&bad)
            .unwrap_err()
            .contains("trailer flag")
    );
}

#[test]
fn empty_templates_and_contour_markers_preserve_the_static_source_shape() {
    let mut empty = descriptor();
    empty.truncate(236);
    empty[232..236].copy_from_slice(&0u32.to_be_bytes());
    word(&mut empty, 0);
    let len = empty.len() as u32;
    empty[4..8].copy_from_slice(&len.to_be_bytes());
    assert!(vector_shape_model(&empty).unwrap().is_none());
    let mut compound = descriptor();
    compound[232..236].copy_from_slice(&4u32.to_be_bytes());
    word(&mut compound, 7);
    word(&mut compound, 1);
    word(&mut compound, 2);
    word(&mut compound, 8);
    compound.extend_from_slice(b"newCurve");
    let len = compound.len() as u32;
    compound[4..8].copy_from_slice(&len.to_be_bytes());
    assert!(vector_shape_model(&compound).unwrap().is_none());
    let last = compound.len() - 8;
    compound[last..].copy_from_slice(b"badCurve");
    assert!(
        vector_shape_model(&compound)
            .unwrap_err()
            .contains("separator")
    );
}
#[test]
fn bezier_handles_are_validated_before_deferring_live_curve_edits() {
    let mut curve = descriptor();
    curve[240..244].copy_from_slice(&3u32.to_be_bytes());
    // Append handle properties immediately after the first integer point.
    let first_end = 258 + 12;
    let tail = curve.split_off(first_end);
    for name in ["handle1", "handle2"] {
        word(&mut curve, 2);
        word(&mut curve, name.len() as u32);
        curve.extend_from_slice(name.as_bytes());
        word(&mut curve, 8);
        word(&mut curve, 2);
        word(&mut curve, 3);
    }
    curve.extend(tail);
    let len = curve.len() as u32;
    curve[4..8].copy_from_slice(&len.to_be_bytes());
    assert!(vector_shape_model(&curve).unwrap().is_none());
}

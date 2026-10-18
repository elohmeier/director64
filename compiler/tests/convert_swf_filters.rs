//! Synthetic filter contracts derived from Adobe's SWF/AS2 documentation.
//! These are independent mathematical expectations, not original-player captures.
use director64_aot::convert::swf::filters::{GradientBevel, apply, read_list};
fn filter() -> GradientBevel {
    GradientBevel {
        colors: vec![[255, 0, 0, 255], [0, 0, 0, 0], [0, 0, 255, 255]],
        ratios: vec![0, 128, 255],
        blur_x: 0.0,
        blur_y: 0.0,
        angle: 0.0,
        distance: 1.0,
        strength: 1.0,
        inner: true,
        knockout: false,
        composite_source: true,
        on_top: false,
        passes: 1,
    }
}
fn rectangle() -> Vec<u8> {
    let mut pixels = vec![0; 12 * 5 * 4];
    for y in 1..4 {
        for x in 3..9 {
            pixels[(y * 12 + x) * 4..(y * 12 + x) * 4 + 4].copy_from_slice(&[0, 255, 0, 255]);
        }
    }
    pixels
}
fn pixel(pixels: &[u8], x: usize, y: usize) -> &[u8] {
    &pixels[(y * 12 + x) * 4..(y * 12 + x) * 4 + 4]
}
#[test]
fn gradient_bevel_uses_opposite_edges_and_preserves_unaffected_fill() {
    let mut pixels = rectangle();
    apply(&filter(), 12, 5, &mut pixels).unwrap();
    assert_eq!(pixel(&pixels, 3, 2), [0, 0, 255, 255]);
    assert_eq!(pixel(&pixels, 8, 2), [255, 0, 0, 255]);
    assert_eq!(pixel(&pixels, 5, 2), [0, 255, 0, 255]);
    assert_eq!(pixel(&pixels, 2, 2), [0, 0, 0, 0]);
}
#[test]
fn inner_blur_blends_gradient_alpha_instead_of_replacing_whole_fill() {
    let mut pixels = rectangle();
    let mut effect = filter();
    effect.blur_x = 3.0;
    apply(&effect, 12, 5, &mut pixels).unwrap();
    assert_eq!(pixel(&pixels, 3, 2), [0, 85, 113, 255]);
    assert_eq!(pixel(&pixels, 8, 2), [113, 85, 0, 255]);
    assert_eq!(pixel(&pixels, 2, 2), [0, 0, 0, 0]);
}
#[test]
fn knockout_removes_fill_and_outer_bevel_keeps_original_fill() {
    let mut pixels = rectangle();
    let mut effect = filter();
    effect.knockout = true;
    apply(&effect, 12, 5, &mut pixels).unwrap();
    assert_eq!(pixel(&pixels, 5, 2), [0, 0, 0, 0]);
    assert_eq!(pixel(&pixels, 3, 2), [0, 0, 255, 255]);
    effect.knockout = false;
    effect.inner = false;
    pixels = rectangle();
    apply(&effect, 12, 5, &mut pixels).unwrap();
    assert_eq!(pixel(&pixels, 3, 2), [0, 255, 0, 255]);
    assert_eq!(pixel(&pixels, 2, 2), [0, 0, 255, 255]);
    assert_eq!(pixel(&pixels, 9, 2), [255, 0, 0, 255]);
}
#[test]
fn strength_zero_is_exact_identity_even_with_opaque_gradient_center() {
    let mut pixels = rectangle();
    pixels[0..4].copy_from_slice(&[12, 34, 56, 78]);
    let original = pixels.clone();
    let mut effect = filter();
    effect.strength = 0.0;
    effect.colors[1] = [255, 255, 255, 255];
    apply(&effect, 12, 5, &mut pixels).unwrap();
    assert_eq!(pixels, original);
}
fn encoded() -> Vec<u8> {
    let mut bytes = vec![1, 7, 3];
    bytes.extend([255, 0, 0, 255, 0, 0, 0, 0, 0, 0, 255, 255]);
    bytes.extend([0, 128, 255]);
    for value in [5i32, 5, 0, 5] {
        bytes.extend((value * 65536).to_le_bytes());
    }
    bytes.extend(256u16.to_le_bytes());
    bytes.push(0xa1);
    bytes
}
#[test]
fn filter_parser_retains_all_fields_and_rejects_every_truncation() {
    let bytes = encoded();
    let mut offset = 0;
    let list = read_list(&bytes, &mut offset).unwrap();
    assert_eq!(offset, bytes.len());
    assert_eq!(list.len(), 1);
    let effect = &list[0];
    assert_eq!(effect.blur_x, 5.0);
    assert_eq!(effect.distance, 5.0);
    assert!(effect.inner && effect.composite_source && !effect.knockout && !effect.on_top);
    assert_eq!(effect.passes, 1);
    assert_eq!(effect.strength, 1.0);
    for end in 0..bytes.len() {
        assert!(read_list(&bytes[..end], &mut 0).is_err(), "end={end}");
    }
    let mut other = bytes.clone();
    other[1] = 1;
    assert!(
        read_list(&other, &mut 0)
            .unwrap_err()
            .contains("unsupported SWF filter 1")
    );
    other = bytes;
    other[17] = 0;
    assert!(
        read_list(&other, &mut 0)
            .unwrap_err()
            .contains("not ordered")
    );
}
#[test]
fn filter_surface_bound_and_alpha_zero_are_explicit() {
    assert!(apply(&filter(), 12, 5, &mut []).is_err());
    let mut empty = vec![0; 12 * 5 * 4];
    apply(&filter(), 12, 5, &mut empty).unwrap();
    assert!(empty.iter().all(|x| *x == 0));
}

#[test]
fn original_adobe_player_boundary_and_inner_samples() {
    // Sparse numerical behavior observations from our generated green rectangle,
    // tools/reference/generate_filter_probe.py --standalone-trace. Raw captures
    // remain ignored. Adobe Windows debugger32.0.0.465, SHA256
    // 9d51df0c26e9ec4717a3da2e184cde831ca139d49e4a7c2f9e26028f8c4dfa52,
    // under Wine. This is not selected Director Xtra or N64 hardware evidence.
    // Compare premultiplied color for translucent outer/knockout samples: very
    // low alpha amplifies straight-RGB rounding without the same visible error.
    let samples: &[(usize, usize, usize, u32, u8, u16)] = &[
        (0, 5, 16, 0x00000000, 0, 0),
        (0, 8, 16, 0xff0000ff, 0, 0),
        (0, 9, 16, 0xff00ff00, 0, 0),
        (0, 15, 16, 0xff00ff00, 0, 0),
        (0, 23, 16, 0xffff0000, 0, 0),
        (0, 16, 8, 0xff00ff00, 0, 0),
        (1, 5, 16, 0x00000000, 0, 0),
        (1, 8, 16, 0xff00ff00, 0, 0),
        (1, 9, 16, 0xff00ff00, 0, 0),
        (1, 15, 16, 0xff00ff00, 0, 0),
        (1, 23, 16, 0xff00ff00, 0, 0),
        (1, 16, 8, 0xff0000ff, 0, 0),
        (2, 5, 16, 0x00000000, 0, 0),
        (2, 8, 16, 0xff005571, 0, 0),
        (2, 9, 16, 0xff00ab1b, 0, 1),
        (2, 15, 16, 0xff00ff00, 0, 0),
        (2, 23, 16, 0xff705600, 0, 1),
        (2, 16, 8, 0xff00ff00, 0, 0),
        (3, 5, 16, 0x00000000, 0, 0),
        (3, 8, 16, 0xffff0000, 0, 0),
        (3, 9, 16, 0xffef0800, 0, 1),
        (3, 15, 16, 0xff00ff00, 0, 0),
        (3, 23, 16, 0xff0000ff, 0, 0),
        (3, 16, 8, 0xffff0000, 0, 0),
        (4, 5, 16, 0x00000000, 0, 0),
        (4, 8, 16, 0xff00ff00, 0, 0),
        (4, 9, 16, 0xff00ff00, 0, 0),
        (4, 15, 16, 0xff00ff00, 0, 0),
        (4, 23, 16, 0xff00ff00, 0, 0),
        (4, 16, 8, 0xff00ff00, 0, 0),
        (5, 5, 16, 0x00000000, 0, 0),
        (5, 8, 16, 0xffff0000, 0, 0),
        (5, 9, 16, 0xffff0000, 0, 0),
        (5, 15, 16, 0xff00ff00, 0, 0),
        (5, 23, 16, 0xff0000ff, 0, 0),
        (5, 16, 8, 0xffff0000, 0, 0),
        (6, 5, 16, 0x00000000, 0, 0),
        (6, 8, 16, 0xffff0000, 0, 0),
        (6, 9, 16, 0xf7f60000, 1, 1),
        (6, 15, 16, 0x00000000, 0, 0),
        (6, 23, 16, 0xff0000ff, 0, 0),
        (6, 16, 8, 0xffff0000, 0, 0),
        (7, 5, 16, 0xc3c20000, 0, 1),
        (7, 8, 16, 0xff00ff00, 0, 0),
        (7, 9, 16, 0xff00ff00, 0, 0),
        (7, 15, 16, 0xff00ff00, 0, 0),
        (7, 23, 16, 0xff00ff00, 0, 0),
        (7, 16, 8, 0xff00ff00, 0, 0),
        (8, 5, 16, 0xc3c20000, 0, 1),
        (8, 8, 16, 0xffff0000, 0, 0),
        (8, 9, 16, 0xffef0800, 0, 1),
        (8, 15, 16, 0xff00ff00, 0, 0),
        (8, 23, 16, 0xff0000ff, 0, 0),
        (8, 16, 8, 0xffff0000, 0, 0),
        // Samples at the largest measured premultiplied-color differences.
        (2, 23, 22, 0xff705600, 0, 1),
        (3, 22, 22, 0xff0000ff, 0, 3),
        (5, 19, 9, 0xffe30e00, 0, 5),
        (6, 18, 8, 0xd3d20000, 2, 4),
        (7, 25, 13, 0xea0000e8, 2, 5),
        (8, 25, 13, 0xea0000e8, 2, 5),
    ];
    let inputs = [
        (0., 0., 1., 1., 0xa1),
        (0., 90., 1., 1., 0xa1),
        (3., 0., 1., 1., 0xa1),
        (5., 220., 5., 1., 0xa1),
        (5., 220., 5., 0., 0xa1),
        (5., 220., 5., 2., 0xa1),
        (5., 220., 5., 1., 0xe1),
        (5., 220., 5., 1., 0x21),
        (5., 220., 5., 1., 0x31),
    ];
    for (case, &(blur, angle, distance, strength, flags)) in inputs.iter().enumerate() {
        let mut effect = filter();
        effect.blur_x = blur;
        effect.blur_y = blur;
        effect.angle = ((angle as f64).to_radians() * 65536.).round() / 65536.;
        effect.distance = distance;
        effect.strength = strength;
        effect.inner = flags & 128 != 0;
        effect.knockout = flags & 64 != 0;
        effect.on_top = flags & 16 != 0;
        let mut pixels = vec![0; 32 * 32 * 4];
        for y in 8..24 {
            for x in 8..24 {
                pixels[(y * 32 + x) * 4..(y * 32 + x) * 4 + 4].copy_from_slice(&[0, 255, 0, 255]);
            }
        }
        apply(&effect, 32, 32, &mut pixels).unwrap();
        for &(_, x, y, argb, alpha_error, color_error) in
            samples.iter().filter(|sample| sample.0 == case)
        {
            let expected = [
                (argb >> 16) as u8,
                (argb >> 8) as u8,
                argb as u8,
                (argb >> 24) as u8,
            ];
            let actual = &pixels[(y * 32 + x) * 4..(y * 32 + x) * 4 + 4];
            assert!(
                actual[3].abs_diff(expected[3]) <= alpha_error,
                "case{case} ({x},{y}) alpha"
            );
            for channel in 0..3 {
                let a = (actual[channel] as u16 * actual[3] as u16 + 127) / 255;
                let b = (expected[channel] as u16 * expected[3] as u16 + 127) / 255;
                assert!(
                    a.abs_diff(b) <= color_error,
                    "case{case} ({x},{y}) channel{channel}: {a} vs {b}"
                );
            }
        }
    }
}

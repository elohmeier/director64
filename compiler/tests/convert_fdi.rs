//! The stored image format (was tests/node/fdi-model.test.mjs).

mod common;

use common::*;
use director64_aot::convert::fdi::{
    FdiMeta, fdi_composite, fdi_image, fdi_pixel, plane_pixels, prescale_image, tile_index, tiled,
};
use std::collections::HashSet;

fn alpha_meta(width: usize, height: usize) -> FdiMeta {
    FdiMeta {
        width,
        height,
        use_alpha: true,
        ..Default::default()
    }
}
fn tinted(alpha: &[u8]) -> Vec<u8> {
    alpha.iter().flat_map(|&a| [37, 118, 209, a]).collect()
}

#[test]
fn soft_coverage_and_rgb_survive_fdi2_exactly_including_alpha_below_128() {
    let rgba = tinted(&[0, 1, 127, 128, 254, 255]);
    let meta = FdiMeta {
        width: 6,
        height: 1,
        reg_x: -3,
        reg_y: 4,
        use_alpha: true,
        alpha_threshold: Some(1),
        rgba32: true,
        source_depth: 32,
        rich_background: None,
    };
    let data = fdi_image(&meta, &rgba).unwrap();
    assert_eq!(&data[..4], b"FDI2");
    assert_eq!(data[32..], rgba[..]);
    assert_eq!(i16::from_be_bytes([data[12], data[13]]), -3);
    for i in 0..6 {
        assert_eq!(fdi_pixel(&data, i, 0)[..], rgba[i * 4..i * 4 + 4]);
    }
}

#[test]
fn fdia_retains_every_alpha_byte_with_aligned_compact_texture_planes() {
    let alpha = [0, 1, 127, 128, 254, 255];
    let data = fdi_image(&alpha_meta(3, 2), &tinted(&alpha)).unwrap();
    assert_eq!(&data[..4], b"FDIA");
    let offset = read_be32(&data, 20) as usize;
    assert_eq!(offset % 8, 0);
    assert_eq!(data.len(), offset + 6);
    assert_eq!(data[offset..], alpha);
    for (i, &a) in alpha.iter().enumerate() {
        assert_eq!(fdi_pixel(&data, i, 0), [33, 115, 214, a]);
    }
}

#[test]
fn binary_coverage_uses_compact_fdi1_without_losing_coverage() {
    let data = fdi_image(&alpha_meta(2, 1), &[255, 255, 255, 0, 255, 0, 0, 255]).unwrap();
    assert_eq!(&data[..4], b"FDI1");
    assert_eq!(data.len(), 36);
    assert_eq!(fdi_pixel(&data, 0, 0), [255, 255, 255, 0]);
    assert_eq!(fdi_pixel(&data, 1, 8), [255, 0, 0, 255]);
}

#[test]
fn bitmap_depth_survives_prescale_and_unknown_depth_remains_unknown() {
    let meta = FdiMeta {
        width: 25,
        height: 25,
        source_depth: 1,
        ..Default::default()
    };
    let data = fdi_image(&meta, &vec![255; 25 * 25 * 4]).unwrap();
    let scaled = prescale_image(&data).unwrap().unwrap();
    assert_eq!(data[19], 1);
    assert_eq!(scaled[19], 1);
    let generated = fdi_image(
        &FdiMeta {
            source_depth: 0,
            ..meta
        },
        &vec![255; 25 * 25 * 4],
    )
    .unwrap();
    assert_eq!(generated[19], 0);
}

#[test]
fn film_layers_compose_straight_alpha_preserving_unoccluded_source_bytes() {
    let mut dest = [0u8; 4];
    fdi_composite(&mut dest, 0, [255, 0, 0, 128]);
    assert_eq!(dest, [255, 0, 0, 128]);
    fdi_composite(&mut dest, 0, [0, 0, 255, 128]);
    assert_eq!(dest, [85, 0, 170, 192]);
    fdi_composite(&mut dest, 0, [1, 2, 3, 0]);
    assert_eq!(dest, [85, 0, 170, 192]);
}

#[test]
fn prescale_snaps_an_isolated_sub_opaque_pixel_into_the_opaque_format() {
    let mut rgba = tinted(&[255; 25 * 25]);
    rgba[12 * 4 + 3] = 200;
    let scaled = prescale_image(&fdi_image(&alpha_meta(25, 25), &rgba).unwrap())
        .unwrap()
        .unwrap();
    assert_eq!(&scaled[..4], b"FDI1");
    assert_eq!((read_be16(&scaled, 8), read_be16(&scaled, 10)), (20, 20));
    for i in 0..400 {
        assert_eq!(fdi_pixel(&scaled, i, 8)[3], 255);
    }
}

#[test]
fn prescale_keeps_genuinely_soft_coverage_in_the_alpha_format() {
    let alpha: Vec<u8> = (0..25 * 25)
        .map(|i| if i % 25 < 12 { 160 } else { 255 })
        .collect();
    let scaled = prescale_image(&fdi_image(&alpha_meta(25, 25), &tinted(&alpha)).unwrap())
        .unwrap()
        .unwrap();
    assert_eq!(&scaled[..4], b"FDIA");
    assert_eq!(fdi_pixel(&scaled, 0, 8)[3], 160);
}

#[test]
fn prescale_snaps_sparse_deep_strays_density_not_depth_gates_it() {
    // A room background scatters isolated noise pixels at any alpha; one per
    // mille of the plane still snaps to the opaque format.
    let size = 40;
    let mut rgba = tinted(&vec![255; size * size]);
    rgba[5 * 4 + 3] = 35;
    let scaled = prescale_image(&fdi_image(&alpha_meta(size, size), &rgba).unwrap())
        .unwrap()
        .unwrap();
    assert_eq!(&scaled[..4], b"FDI1");
    for i in 0..32 * 32 {
        assert_eq!(fdi_pixel(&scaled, i, 8)[3], 255);
    }
}

#[test]
fn an_image_past_the_texture_limit_is_stored_as_padded_32x32_tiles() {
    // The console reads these planes through bitmap_tile_index without
    // rearranging them, so the converter has to write that order itself.
    let (width, height) = (1100usize, 40usize);
    let mut rgba = vec![0u8; width * height * 4];
    for i in 0..width * height {
        rgba[i * 4] = (i * 3 % 256) as u8;
        rgba[i * 4 + 1] = (i * 7 % 256) as u8;
        rgba[i * 4 + 2] = (i * 13 % 256) as u8;
        rgba[i * 4 + 3] = if i % 3 != 0 { 255 } else { 0 };
    }
    let data = fdi_image(&alpha_meta(width, height), &rgba).unwrap();
    assert_eq!(&data[..4], b"FDI1");
    assert!(tiled(width, height));
    let tiles = width.div_ceil(32) * height.div_ceil(32) * 1024;
    assert_eq!(plane_pixels(width, height), tiles);
    assert_eq!(data.len(), 32 + tiles * 2);
    assert_eq!(read_be32(&data, 4) as usize, data.len());
    // Header dimensions stay authored; both sides derive the layout from them.
    assert_eq!(
        (read_be16(&data, 8) as usize, read_be16(&data, 10) as usize),
        (width, height)
    );
    let mut mapped = HashSet::new();
    for i in 0..width * height {
        let slot = tile_index(width, i % width, i / width);
        assert_eq!(read_be16(&data, 32 + slot * 2) & 1, u16::from(i % 3 != 0));
        mapped.insert(slot);
    }
    // Padding a partial edge tile leaves is zero, which every ink leaves alone.
    assert_eq!(mapped.len(), width * height);
    for slot in (0..tiles).filter(|s| !mapped.contains(s)) {
        assert_eq!(read_be16(&data, 32 + slot * 2), 0);
    }
}

#[test]
fn a_tiled_fdia_puts_its_coverage_plane_after_the_padded_colour_plane() {
    let (width, height) = (1030usize, 35usize);
    let alpha: Vec<u8> = (0..width * height)
        .map(|i| [0, 137, 255, 255][i % 4])
        .collect();
    let rgba: Vec<u8> = alpha.iter().flat_map(|&a| [0, 0, 0, a]).collect();
    let data = fdi_image(&alpha_meta(width, height), &rgba).unwrap();
    assert_eq!(&data[..4], b"FDIA");
    let tiles = plane_pixels(width, height);
    let offset = read_be32(&data, 20) as usize;
    assert_eq!(offset, 32 + tiles * 2);
    assert_eq!(offset % 8, 0);
    assert_eq!(data.len(), offset + tiles);
    for (i, &a) in alpha.iter().enumerate() {
        assert_eq!(fdi_pixel(&data, i, 0)[3], a);
    }
}

#[test]
fn film_poses_trim_to_their_coverage_and_keep_their_registration() {
    use director64_aot::convert::fdi::{FdiMeta, fdi_image, fdi_pixel, trim_image};
    let (w, h) = (8usize, 6usize);
    let mut rgba = vec![0u8; w * h * 4];
    for (x, y) in [(3, 2), (4, 2), (3, 3), (4, 3)] {
        rgba[(y * w + x) * 4..(y * w + x) * 4 + 4].copy_from_slice(&[200, 40, 8, 255]);
    }
    let meta = FdiMeta {
        width: w,
        height: h,
        reg_x: 4,
        reg_y: 3,
        use_alpha: true,
        alpha_threshold: Some(1),
        rgba32: false,
        source_depth: 1,
        rich_background: None,
    };
    let pose = fdi_image(&meta, &rgba).unwrap();
    let cut = trim_image(&pose)
        .unwrap()
        .expect("a quarter-covered pose trims");
    assert_eq!(pose[19], 1);
    assert_eq!(cut[19], 1);
    let be16 = |at: usize| u16::from_be_bytes([cut[at], cut[at + 1]]) as i64;
    assert_eq!((be16(8), be16(10)), (2, 2));
    // The registration point moves with the cut, so the pose draws in place.
    assert_eq!((be16(12), be16(14)), (1, 1));
    for i in 0..4 {
        assert_eq!(
            fdi_pixel(&cut, i, 8),
            fdi_pixel(&pose, (2 + i / 2) * w + 3 + i % 2, 8)
        );
    }
    // A pose that already fills its loop stays as it is.
    let full = fdi_image(&meta, &vec![255u8; w * h * 4]).unwrap();
    assert!(trim_image(&full).unwrap().is_none());
    // An empty one keeps a single transparent pixel.
    let empty = trim_image(&fdi_image(&meta, &vec![0u8; w * h * 4]).unwrap())
        .unwrap()
        .unwrap();
    assert_eq!(
        (
            u16::from_be_bytes([empty[8], empty[9]]),
            u16::from_be_bytes([empty[10], empty[11]])
        ),
        (1, 1)
    );
    assert_eq!(fdi_pixel(&empty, 0, 8)[3], 0);
}

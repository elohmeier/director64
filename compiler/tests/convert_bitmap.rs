//! BITD decoding and palettes (was tests/node/director-bitmap.test.mjs).

use director64_aot::convert::bitmap::{decode_packbits, decode_palette};

#[test]
fn bitd_raw_and_bounded_compressed_decoding() {
    assert!(!decode_packbits(&[1, 2, 3], 3).unwrap().compressed);
    assert_eq!(decode_packbits(&[0xfe, 9], 3).unwrap().pixels, [9, 9, 9]);
    assert_eq!(
        decode_packbits(&[1, 4, 5, 0xfe, 9], 5).unwrap().pixels,
        [1, 4, 5, 254, 9]
    );
    assert_eq!(
        decode_packbits(&[0, 4, 0xfe, 9], 4).unwrap().pixels,
        [0, 4, 254, 9]
    );
    assert_eq!(
        decode_packbits(&[1, 4, 5, 0xfd, 9], 6).unwrap().pixels,
        [4, 5, 9, 9, 9, 9]
    );
}

#[test]
fn bitd_malformed_lengths_runs_and_trailing_bytes_fail_closed() {
    let cases: [(&[u8], usize); 6] = [
        (&[], 1),
        (&[255], 2),
        (&[1, 4], 3),
        (&[254, 8], 4),
        (&[128, 7], 128),
        (&[254, 8, 0, 5], 3),
    ];
    for (data, size) in cases {
        assert!(decode_packbits(data, size).is_err(), "{data:?} -> {size}");
    }
    assert!(decode_packbits(&[0], 0).is_err());
    assert!(decode_packbits(&[0], (1usize << 53) - 1).is_err());
}

#[test]
fn missing_palettes_never_become_grayscale_approximations() {
    let palette = decode_palette(&[255, 255, 127, 127, 0, 0]).unwrap();
    assert_eq!(palette[0], [255, 127, 0]);
    assert!(palette.get(1).is_none());
    assert!(decode_palette(&[1, 2, 3]).is_err());
}

#[test]
fn original_palette_indices_survive_padding_and_rle_without_rgb_roundtrip() {
    use director64_aot::convert::bitmap::{Meta, bitmap_indices};
    let meta = Meta {
        width: 3,
        height: 2,
        depth: 8,
        pitch: 4,
        palette: 0,
        palette_cast: 0,
        reg_x: 0,
        reg_y: 0,
        use_alpha: false,
        alpha_threshold: 0,
        update_flags: 0,
    };
    let raw = [246, 6, 249, 99, 0, 255, 3, 88];
    assert_eq!(
        bitmap_indices(&meta, &raw).unwrap(),
        [246, 6, 249, 0, 255, 3]
    );
    let mut packed = vec![7];
    packed.extend(raw);
    assert_eq!(
        bitmap_indices(&meta, &packed).unwrap(),
        [246, 6, 249, 0, 255, 3]
    );
    assert!(bitmap_indices(&meta, &[7, 246]).is_err());
    assert!(
        bitmap_indices(
            &Meta {
                depth: 4,
                ..meta.clone()
            },
            &raw
        )
        .is_err()
    );
    assert!(bitmap_indices(&Meta { pitch: 2, ..meta }, &raw).is_err());
}

//! Synthetic 32x32 quadrant animation, encoded with nihav-encoder
//! df36346566d9870550275883931a46f98dbd0c64 / NihAV
//! 13f31633cf325b0036d697af7bf05c611b4b8967, vp62 quant63.
//! No game media: input is three arrangements of constant YUV quadrants.
//! Expected planar hashes were independently decoded by strict FFmpeg vp6f.
mod common;
use common::*;
use director64_aot::convert::{swf, vp6};
use sha2::{Digest, Sha256};
const PACKETS: [&str; 3] = [
    "7f40002d020202020cc8b344770b508607c7fffde8589681fb03f607f81fe000d1fffd8ce58d6ffffbc1f80000fd0783f60d7efcd84024c8c0bc61de439ece44664e6404d62cb73ccb8f10",
    "ff001a0bdffe02c7ff2abfded2f1fffff0001e703a39db2b6ee000000000",
    "ff000e1d95fb00002eb4e214d98000000000",
];
fn packet(n: usize) -> Vec<u8> {
    PACKETS[n]
        .as_bytes()
        .chunks_exact(2)
        .map(|v| u8::from_str_radix(std::str::from_utf8(v).unwrap(), 16).unwrap())
        .collect()
}
#[test]
fn vp6_key_and_interframes_match_independent_decoder_and_crop() {
    let hashes = [
        "5f0b21fa8aecf3870173f0319db5c31117076647c391c050ce89d3d44b2ef0f5",
        "f95491cb89830f784067dd7fd63fbbe91bc9355adb6fc987230b73074148a021",
        "90871837dc0f2e341b81857758eac63f7712c7622ce319059487210ee08548a3",
    ];
    let mut decoder = vp6::Decoder::new(32, 32).unwrap();
    let mut cropped = vp6::Decoder::new(29, 27).unwrap();
    for n in 0..3 {
        let frame = decoder.decode(&packet(n)).unwrap();
        assert_eq!(format!("{:x}", Sha256::digest(&frame.yuv)), hashes[n]);
        assert_eq!(decoder.decode(&[]).unwrap().yuv, frame.yuv);
        let crop = cropped.decode(&packet(n)).unwrap();
        for y in 0..27 {
            assert_eq!(
                &crop.yuv[y * 29..(y + 1) * 29],
                &frame.yuv[y * 32..y * 32 + 29]
            );
        }
        assert_eq!(frame.rgba().len(), 1024);
        assert!(frame.rgba().iter().all(|p| p[3] == 255));
    }
}
#[test]
fn vp6_rejects_bad_dimensions_missing_references_and_truncated_headers() {
    assert!(vp6::Decoder::new(0, 1).is_err());
    assert!(vp6::Decoder::new(2049, 16).is_err());
    for bytes in [Vec::new(), packet(1), vec![0, 0, 0]] {
        assert!(vp6::Decoder::new(32, 32).unwrap().decode(&bytes).is_err());
    }
    assert!(
        vp6::Decoder::new(64, 32)
            .unwrap()
            .decode(&packet(0))
            .is_err()
    );
}
fn video_swf(codec: u8, order: &[usize], declared: u16) -> Vec<u8> {
    let mut body = tag(
        60,
        &[
            1,
            0,
            declared as u8,
            (declared >> 8) as u8,
            32,
            0,
            32,
            0,
            0,
            codec,
        ],
    );
    for (n, &frame) in order.iter().enumerate() {
        body.extend(tag(
            26,
            &if n == 0 {
                vec![6, 1, 0, 1, 0, 0]
            } else {
                vec![17, 1, 0, n as u8, 0]
            },
        ));
        body.extend(tag(61, &concat(&[&[1, 0, frame as u8, 0], &packet(frame)])));
        body.extend(tag(1, &[]));
    }
    body.extend(tag(0, &[]));
    let rect = Writer::new()
        .put(11, 5)
        .put(0, 11)
        .put(640, 11)
        .put(0, 11)
        .put(640, 11)
        .done();
    let mut file = concat(&[
        b"FWS\x08\0\0\0\0",
        &rect,
        &[0, 12],
        &(order.len() as u16).to_le_bytes(),
        &body,
    ]);
    let size = file.len();
    file[4..8].copy_from_slice(&(size as u32).to_le_bytes());
    file
}
#[test]
fn embedded_video_follows_authored_ratio_and_rejects_missing_packets() {
    let movie = swf::flash_frames(&video_swf(4, &[0, 1, 2], 3)).unwrap();
    assert_eq!((movie.width, movie.height, movie.frames.len()), (32, 32, 3));
    let mut decoder = vp6::Decoder::new(32, 32).unwrap();
    for n in 0..3 {
        assert_eq!(
            movie.frames[n],
            decoder
                .decode(&packet(n))
                .unwrap()
                .rgba()
                .into_iter()
                .flatten()
                .collect::<Vec<_>>()
        );
    }
    assert!(
        swf::flash_frames(&video_swf(2, &[0], 1))
            .err()
            .unwrap()
            .contains("codec 2")
    );
    assert!(
        swf::flash_frames(&video_swf(4, &[0, 2], 3))
            .err()
            .unwrap()
            .contains("unordered")
    );
    assert!(
        swf::flash_frames(&video_swf(4, &[0], 3))
            .err()
            .unwrap()
            .contains("frame count")
    );
}

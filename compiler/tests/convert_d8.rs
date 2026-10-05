//! D8 scores, palettes, FollowAlpha bitmaps, text Xtras and vector shapes
//! (was tests/node/director-d8-assets.test.mjs).

mod common;

use common::*;
use director64_aot::convert::bitmap::{
    bitmap_metadata, bitmap_pixels, bitmap_source_alpha, builtin_palette, decode_palette, score_model, Meta,
};
use director64_aot::convert::fdi::{fdi_image, fdi_pixel, FdiMeta};
use director64_aot::convert::js::hex;
use director64_aot::convert::swf::vector_shape_pixels;
use director64_aot::convert::xtra::text_xtra_model;
use serde_json::json;

#[test]
fn score_retains_48_byte_partial_updates_and_high_sprite_channels() {
    let mut bytes = vec![0u8; 48];
    bytes[24] = 17;
    be32(&mut bytes, 28, 9000);
    let mut score = json!({"fields": {"frames_version": 13, "channel_record_size": 48, "unindexed_tail_length": 0},
        "records": [], "frames": [{"number": 1, "changed_channels": [{"index": 805, "record_hex": hex(&bytes)}],
        "deltas": [{"channel_offset": 805 * 48 + 24, "payload": {"length": 8}}]}]});
    let model = score_model(&score, None).unwrap();
    let channel = &model["frames"][0]["channels"][0];
    assert_eq!((model["recordSize"].clone(), model["version"].clone()), (48.into(), 13.into()));
    assert_eq!(channel["channel"], 805);
    let recorded: Vec<u8> = channel["bytes"].as_array().unwrap().iter().map(|v| v.as_u64().unwrap() as u8).collect();
    assert_eq!(recorded.len(), 48);
    let changed: Vec<i64> = (0..48).map(|i| i64::from((24..32).contains(&i))).collect();
    assert_eq!(channel["changed"], json!(changed));
    assert_eq!(read_be32(&recorded, 28), 9000);
    score["fields"]["frames_version"] = json!(11);
    assert!(score_model(&score, None).unwrap_err().contains("layout"));
}

#[test]
fn builtin_palette_operands_resolve_exact_mac_cube_and_grayscale_ramps() {
    let mac = decode_palette(&builtin_palette(0, 8).unwrap()).unwrap();
    let gray = decode_palette(&builtin_palette(-2, 8).unwrap()).unwrap();
    assert_eq!(mac.len(), 256);
    assert_eq!(mac[0], [255, 255, 255]);
    assert_eq!(mac[1], [255, 255, 204]);
    assert_eq!(mac[214], [0, 0, 51]);
    assert_eq!(mac[215], [238, 0, 0]);
    assert_eq!(mac[245], [238, 238, 238]);
    assert_eq!(mac[255], [0, 0, 0]);
    assert_eq!(gray[91], [164, 164, 164]);
    assert_eq!(decode_palette(&builtin_palette(-2, 4).unwrap()).unwrap()[1], [238, 238, 238]);
    assert!(builtin_palette(-8, 8).unwrap_err().contains("unsupported"));
}

#[test]
fn authored_empty_bitmaps_remain_empty_and_truncated_color_metadata_fails() {
    let mut bytes = vec![0u8; 40];
    be32(&mut bytes, 8, 28);
    be16(&mut bytes, 12, 0x8000);
    bytes[35] = 32;
    let meta = bitmap_metadata(&bytes, 600).unwrap();
    assert_eq!((meta.width, meta.height), (0, 0));
    be32(&mut bytes, 8, 22);
    assert!(bitmap_metadata(&bytes, 600).unwrap_err().contains("truncated color"));
}

fn fdi_meta(meta: &Meta) -> FdiMeta {
    FdiMeta {
        width: meta.width as usize,
        height: meta.height as usize,
        reg_x: meta.reg_x,
        reg_y: meta.reg_y,
        use_alpha: meta.use_alpha,
        alpha_threshold: meta.use_alpha.then_some(meta.alpha_threshold as u8),
        rgba32: false,
    }
}

#[test]
fn follow_alpha_survives_interleaved_and_planar_bitd_conversion_to_target_fdia() {
    let mut cast = vec![0u8; 40];
    be32(&mut cast, 8, 28);
    be16(&mut cast, 12, 0x8000 | 16);
    be16(&mut cast, 18, 1);
    be16(&mut cast, 20, 4);
    cast[22] = 1;
    cast[34] = 0x10;
    cast[35] = 32;
    let meta = bitmap_metadata(&cast, 800).unwrap();
    assert!(meta.use_alpha);
    assert_eq!(meta.alpha_threshold, 1);
    assert!(!bitmap_metadata(&cast, 600).unwrap().use_alpha);
    let alpha = [0u8, 127, 128, 255];
    let argb: Vec<u8> = alpha.iter().flat_map(|&a| [a, 255, 255, 255]).collect();
    assert_eq!(bitmap_source_alpha(&meta, &argb).unwrap(), alpha);
    let words = bitmap_pixels(&meta, &argb, None, false).unwrap();
    let coverage: Vec<u8> = words.iter().skip(1).step_by(2).map(|v| v & 1).collect();
    assert_eq!(coverage, [0, 0, 1, 1]);
    let full = bitmap_pixels(&meta, &argb, None, true).unwrap();
    let target = fdi_image(&fdi_meta(&meta), &full).unwrap();
    assert_eq!(&target[..4], b"FDIA");
    for (i, &a) in alpha.iter().enumerate() {
        assert_eq!(fdi_pixel(&target, i, 0), [255, 255, 255, a]);
    }
    // A compressed BITD stores A/R/G/B planes separately. Opaque white must
    // retain authored alpha instead of being removed by the legacy matte fill.
    let planar = [3, 0, 127, 128, 255, 245, 255];
    assert_eq!(bitmap_source_alpha(&meta, &planar).unwrap(), alpha);
    assert_eq!(bitmap_pixels(&meta, &planar, None, false).unwrap(), words);
    assert_eq!(bitmap_pixels(&meta, &planar, None, true).unwrap(), full);
    cast[34] = 0;
    let plain = bitmap_metadata(&cast, 800).unwrap();
    assert!(!plain.use_alpha);
    assert!(bitmap_source_alpha(&plain, &argb).unwrap_err().contains("authored alpha"));
}

fn xmed(records: &[(u32, Vec<u8>)], suffix: &str) -> Vec<u8> {
    let mut out = latin1("FFFF0000000600040001\x0177AA");
    for (id, body) in records {
        out.extend(latin1(&format!("\x03{id:04X}{:08X}00000000", body.len() + 1)));
        out.extend_from_slice(body);
    }
    out.extend(latin1(&format!("\x03{suffix}")));
    out
}
fn text_payload() -> Vec<u8> {
    let mut b = vec![0u8; 76];
    be32(&mut b, 36, 20);
    be32(&mut b, 40, 80);
    b
}

#[test]
fn xmed_text_uses_bounded_section_2_rather_than_font_name_heuristics() {
    let document = xmed(&[(0, vec![0x81]), (1, vec![0x82]), (2, latin1("\x005,H\u{8a}llo")),
        (8, latin1("\x00B,Fake text!!"))], "");
    let result = text_xtra_model(&text_payload(), &document, 800).unwrap();
    assert_eq!(result["text"], "Hällo");
    assert_eq!((result["width"].clone(), result["height"].clone()), (80.into(), 20.into()));
    assert_eq!(result["textEncoding"], "macintosh");
    assert_eq!(result["textPresentation"], "native-target-font");
    assert_eq!(text_xtra_model(&text_payload(), &xmed(&[(0, vec![]), (1, vec![])], "FF"), 800).unwrap()["text"], "");
    assert!(text_xtra_model(&text_payload(), &document[..document.len() - 1], 800).unwrap_err().contains("record range"));
    let short = xmed(&[(0, vec![]), (1, vec![]), (2, latin1("\x006,short"))], "");
    assert!(text_xtra_model(&text_payload(), &short, 800).unwrap_err().contains("text length"));
}

fn vector_fixture(extra: &[u8]) -> Vec<u8> {
    let mut edges = Writer::new();
    edges.put(0, 1).put(3, 5).put(1, 5).put(0, 1).put(0, 1).put(1, 1);
    for (vertical, delta) in [(0, 60), (1, 40), (0, -60), (1, -40)] {
        edges.put(1, 1).put(1, 1).put(5, 4).put(0, 1).put(vertical, 1).put(delta, 7);
    }
    edges.put(0, 1).put(0, 5);
    let shape = concat(&[&[1, 0], &stage_rect(), &[1, 0, 255, 0, 0, 0, 0x10], &edges.done()]);
    let placement = [1, 0, 1, 0, 0];
    let mut swf = concat(&[&latin1("FWS\x01\0\0\0\0"), &stage_rect(), &[0, 12, 1, 0], &tag(22, &shape),
        &tag(4, &placement), extra, &tag(1, &[]), &tag(0, &[])]);
    let length = swf.len() as i64;
    le32(&mut swf, 4, length);
    let mut envelope = vec![0u8; 12];
    be32(&mut envelope, 0, length + 12);
    be32(&mut envelope, 4, 1);
    be32(&mut envelope, 8, length);
    concat(&[&envelope, &swf])
}

#[test]
fn static_vector_xmed_rasterizes_authored_geometry_and_rejects_unknown_swf_tags() {
    let image = vector_shape_pixels(&vector_fixture(&[])).unwrap();
    assert_eq!((image.width, image.height), (3, 2));
    for p in (0..image.pixels.len()).step_by(2) {
        assert_eq!(read_be16(&image.pixels, p), 0xf801);
    }
    let error = |extra: &[u8]| vector_shape_pixels(&vector_fixture(extra)).err().unwrap();
    assert!(error(&tag(12, &[7, 0])).contains("needs explicit conversion"));
    assert!(error(&tag(6, &[])).contains("unsupported static SWF tag 6"));
    let whole = vector_fixture(&[]);
    assert!(vector_shape_pixels(&whole[..whole.len() - 1]).err().unwrap().contains("envelope"));
}

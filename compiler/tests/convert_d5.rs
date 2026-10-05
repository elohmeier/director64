//! D5 scores and media (was tests/node/director-d5.test.mjs).

mod common;

use common::*;
use director64_aot::convert::bitmap::{bitmap_metadata, score_model};
use director64_aot::convert::d5media::{cast_strings, sound_resource, styled_text};
use director64_aot::convert::js::hex;
use serde_json::{json, Value};

fn channel(frame: &Value, index: i64) -> &Value {
    frame["channels"].as_array().unwrap().iter().find(|c| c["channel"] == index).unwrap()
}
fn first(value: &Value, n: usize) -> Value {
    Value::Array(value.as_array().unwrap()[..n].to_vec())
}

#[test]
fn direct_scripts_two_main_records_and_byte_ownership_normalize_independently() {
    let (mut main, mut sprite) = (vec![0u8; 24], vec![0u8; 24]);
    be16(&mut main, 0, 1);
    be16(&mut main, 2, 17);
    main[21] = 137;
    be16(&mut main, 4, 2);
    be16(&mut main, 6, 44);
    be16(&mut main, 8, 1);
    be16(&mut main, 10, 55);
    sprite[0] = 16;
    be16(&mut sprite, 2, 2);
    be16(&mut sprite, 4, 5);
    be16(&mut sprite, 6, 1);
    be16(&mut sprite, 8, 9);
    sprite[10] = 255;
    sprite[11] = 7;
    let mut score = json!({"fields": {"frames_version": 7, "channel_record_size": 24}, "frames": [{"number": 1,
        "changed_channels": [{"index": 0, "record_hex": hex(&main)}, {"index": 2, "record_hex": hex(&sprite)}],
        "deltas": [{"channel_offset": 0, "payload": {"length": 24}}, {"channel_offset": 48, "payload": {"length": 24}}]}]});
    let model = score_model(&score, None).unwrap();
    let frame = &model["frames"][0];
    assert_eq!(channel(frame, 0)["behaviors"], json!([{"cast": 1, "member": 17}]));
    assert_eq!(channel(frame, 1)["tempoRaw"], 137);
    assert_eq!(first(&channel(frame, 4)["bytes"], 4), json!([0, 2, 0, 44]));
    assert_eq!(first(&channel(frame, 3)["bytes"], 4), json!([0, 1, 0, 55]));
    let normalized = channel(frame, 6);
    assert_eq!(Value::Array(normalized["bytes"].as_array().unwrap()[2..8].to_vec()), json!([255, 7, 0, 2, 0, 5]));
    assert_eq!(normalized["behaviors"], json!([{"cast": 1, "member": 9}]));
    score["frames"][0]["deltas"] = json!([{"channel_offset": 58, "payload": {"length": 1}}]);
    let changed: Vec<i64> = (0..24).map(|i| i64::from(i == 2)).collect();
    assert_eq!(score_model(&score, None).unwrap()["frames"][0]["channels"][0]["changed"], json!(changed));
}

#[test]
fn stxt_preserves_source_metrics_and_decodes_german_mac_roman_text() {
    let mut b = vec![0u8; 12 + 4 + 22];
    be32(&mut b, 0, 12);
    be32(&mut b, 4, 4);
    be32(&mut b, 8, 22);
    put(&mut b, 12, &[0x86, 0x8a, 0x9a, 0x9f]);
    be16(&mut b, 16, 1);
    be16(&mut b, 22, 16);
    be16(&mut b, 24, 12);
    be16(&mut b, 26, 3);
    be16(&mut b, 30, 12);
    let text = styled_text(&b).unwrap();
    assert_eq!(text["text"], "Üäöü");
    assert_eq!(text["sourceTextStyles"][0]["sourceFontId"], 3);
    assert_eq!(text["sourceTextStyles"][0]["ascent"], 12);
    for end in 0..b.len() {
        assert!(styled_text(&b[..end]).is_err(), "truncated at {end}");
    }
    be32(&mut b, 18, 5);
    assert!(styled_text(&b).unwrap_err().contains("offset"));
}

#[test]
fn monochrome_cast_ends_before_the_d6_update_flag() {
    let mut b = vec![0u8; 34];
    be32(&mut b, 0, 1);
    be32(&mut b, 8, 22);
    be16(&mut b, 12, 2);
    be16(&mut b, 18, 8);
    be16(&mut b, 20, 16);
    let meta = bitmap_metadata(&b, 500).unwrap();
    assert_eq!((meta.depth, meta.width, meta.height), (1, 16, 8));
}

#[test]
fn extended_snd_keeps_sample_rate_loop_bounds_and_unsigned_pcm() {
    let mut b = vec![0u8; 14 + 64 + 4];
    be16(&mut b, 0, 2);
    be16(&mut b, 4, 1);
    be16(&mut b, 6, 0x8051);
    be32(&mut b, 10, 14);
    be32(&mut b, 18, 1);
    be16(&mut b, 22, 22050);
    be32(&mut b, 30, 4);
    b[34] = 255;
    b[35] = 60;
    be32(&mut b, 36, 4);
    be16(&mut b, 62, 8);
    put(&mut b, 78, &[0, 127, 128, 255]);
    let sound = sound_resource(&b).unwrap();
    assert_eq!((sound.rate, sound.frames, sound.loop_end), (22050, 4, 4));
    assert_eq!(sound.samples, [0, 127, 128, 255]);
    for end in 0..b.len() {
        assert!(sound_resource(&b[..end]).is_err(), "truncated at {end}");
    }
    be32(&mut b, 30, 5);
    assert!(sound_resource(&b).err().unwrap().contains("bounds"));
}

#[test]
fn cast_string_offsets_cannot_escape_their_info_payload() {
    let mut b = vec![0u8; 12 + 22 + 8 + 3];
    let length = b.len() as i64 - 12;
    be32(&mut b, 4, length);
    be32(&mut b, 12, 20);
    be16(&mut b, 32, 1);
    be32(&mut b, 38, 3);
    put(&mut b, 42, &[2, 65, 66]);
    assert_eq!(cast_strings(&b).unwrap(), ["AB"]);
    be32(&mut b, 38, 4);
    assert!(cast_strings(&b).unwrap_err().contains("outside"));
}

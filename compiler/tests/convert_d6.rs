//! D6 score behaviors (was tests/node/director-d6.test.mjs).

mod common;

use common::*;
use director64_aot::convert::{bitmap::score_model, js::hex};
use serde_json::json;

#[test]
fn behavior_details_and_initializer_records_use_their_distinct_indices() {
    let mut channel = vec![0u8; 24];
    let mut details = vec![0u8; 16];
    be32(&mut channel, 8, 1);
    be16(&mut details, 0, 2);
    be16(&mut details, 2, 19);
    be32(&mut details, 4, 3);
    be16(&mut details, 8, 1);
    be16(&mut details, 10, 4);
    be32(&mut details, 12, 99);
    let parameters = "[#direction:#left,#position:point(3,4)]";
    let record = |b: &[u8]| json!({"length": b.len(), "hex": hex(b)});
    let score = json!({
        "fields": {"frames_version": 11, "channel_record_size": 24, "unindexed_tail_length": 7},
        "records": [null, null, record(&details), record(format!("{parameters}\0unused").as_bytes())],
        "frames": [{"number": 1, "changed_channels": [{"index": 6, "record_hex": hex(&channel)}],
            "deltas": [{"channel_offset": 144, "payload": {"length": 24}}]}],
    });
    let model = score_model(&score, Some(&json!({"labels": []}))).unwrap();
    let channel = &model["frames"][0]["channels"][0];
    assert_eq!(channel["behaviors"], json!([
        {"cast": 2, "member": 19, "parameters": parameters}, {"cast": 1, "member": 4, "missingInitializer": 99},
    ]));
    assert_eq!(model["unknown_tail_bytes"], 7);
    assert_eq!(channel["changed"], json!(vec![1; 24]));
}

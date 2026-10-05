//! Source identities, KEY* obligations, bytecode sites and projector views
//! (was tests/node/source-model.test.mjs).

mod common;

use common::*;
use director64_aot::convert::js::serialize;
use director64_aot::convert::projector::projector_view;
use director64_aot::convert::source::{active_key_entries, script_sites, stable_id};
use serde_json::json;
use std::collections::HashSet;

#[test]
fn canonical_field_order_and_source_identity() {
    assert_eq!(serialize(&json!({"b": 1, "a": {"d": 4, "c": 3}})), serialize(&json!({"a": {"c": 3, "d": 4}, "b": 1})));
    let id = |movie: &str| stable_id("script", &[json!(movie), json!(1)]);
    assert_eq!(id("movie"), id("movie"));
    assert_ne!(id("movie"), id("other"));
}

#[test]
fn only_live_key_entries_are_resource_obligations() {
    let live = json!({"fourCC": "BITD", "sectionID": 8, "castID": 6});
    let table = json!({"usedCount": 1, "entryCount": 2, "entries": [live, {"stale": true}]});
    assert_eq!(active_key_entries(Some(&table)).unwrap(), [live.clone()]);
    for used in [json!(-1), json!(3), json!(1.5), json!(null)] {
        let table = json!({"usedCount": used, "entryCount": 2, "entries": [live, live]});
        assert!(active_key_entries(Some(&table)).is_err(), "usedCount {used}");
    }
}

#[test]
fn bytecode_sites_disambiguate_handler_local_offsets() {
    let script = json!({"scriptId": 1, "lingo": "on first\nend\non second\nend\n",
        "bytecode": "on first\n  [  0] ret\nend\non second\n  [  0] ret\nend\n"});
    let library = json!(1024);
    let result = script_sites(&script, "source-hash", &library).unwrap();
    assert_eq!(result.1.len(), 2);
    assert_eq!(result.2.iter().map(|s| s["id"].as_str().unwrap()).collect::<HashSet<_>>().len(), 2);
    let with = |key: &str, value: &str| {
        let mut s = script.clone();
        s[key] = json!(value);
        s
    };
    assert_eq!(script_sites(&with("lingo", "reformatted"), "source-hash", &library).unwrap(), result);
    assert!(script_sites(&with("bytecode", "  [0] ret"), "source-hash", &library).is_err());
    assert!(script_sites(&with("bytecode", "on f\n [0] ret\n [0] ret"), "source-hash", &library).is_err());
}

#[test]
fn embedded_projector_view_retains_absolute_chunk_positions_and_source_bytes() {
    let mut input = vec![0u8; 256];
    put(&mut input, 64, b"XFIR");
    le32(&mut input, 68, 184);
    put(&mut input, 72, b"39VMpami");
    le32(&mut input, 80, 24);
    le32(&mut input, 88, 128);
    put(&mut input, 128, b"pamm");
    let original = input.clone();
    let result = projector_view(&input, None).unwrap();
    assert_eq!(result.offset, 64);
    assert_eq!(&result.bytes[..4], b"XFIR");
    assert_eq!(u32::from_le_bytes(result.bytes[24..28].try_into().unwrap()), 128);
    assert_eq!(input, original);
    le32(&mut input, 88, 1024);
    assert!(projector_view(&input, None).err().unwrap().contains("bounds"));
    assert!(projector_view(&[0u8; 100], None).err().unwrap().contains("expected one"));
}

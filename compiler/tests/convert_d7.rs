//! D7 scripts, multi-archive projectors and palettes (was
//! tests/node/director-d7.test.mjs).

mod common;

use common::*;
use director64_aot::convert::bitmap::{bitmap_pixels, builtin_palette, decode_palette, Meta};
use director64_aot::convert::projector::projector_view;
use director64_aot::convert::source::{normalized_scripts, sha256};
use serde_json::{json, Value};

#[test]
fn repeated_external_cast_ids_restore_only_identical_local_scripts() {
    let entries = json!([{"id": 1024, "name": "Internal", "filePath": ""}, {"id": 1024, "name": "Shared", "filePath": "SHARED.CXT"}]);
    let parsed = vec![json!({"fourCC": "MCsL", "data": {"entries": entries}})];
    let cast = json!({"name": "Shared", "scripts": [{"lingo": "on f\nend", "bytecode": "on f\n [0] ret"}]});
    let casts = json!([cast, cast]);
    let scripts = json!({"version": 700, "casts": casts});
    assert_eq!(normalized_scripts(&parsed, &scripts).unwrap()["casts"],
        json!([{"name": "Internal", "scripts": cast["scripts"]}]));
    let mut d6 = scripts.clone();
    d6["version"] = json!(600);
    assert_eq!(normalized_scripts(&parsed, &d6).unwrap()["casts"], casts);
    let external: Vec<Value> = entries.as_array().unwrap().iter().map(|e| {
        let mut e = e.clone();
        e["filePath"] = json!("external");
        e
    }).collect();
    let all_external = vec![json!({"fourCC": "MCsL", "data": {"entries": external}})];
    assert_eq!(normalized_scripts(&all_external, &scripts).unwrap(), scripts);
    let mut disagreeing = scripts.clone();
    disagreeing["casts"] = json!([cast, {"scripts": []}]);
    assert!(normalized_scripts(&parsed, &disagreeing).unwrap_err().contains("disagree"));
}

#[test]
fn multi_archive_projectors_require_the_complete_pinned_inventory_and_selected_offset() {
    let mut input = vec![0u8; 512];
    for offset in [64usize, 288] {
        put(&mut input, offset, b"XFIR");
        le32(&mut input, offset + 4, 184);
        put(&mut input, offset + 8, b"39VMpami");
        le32(&mut input, offset + 16, 24);
        le32(&mut input, offset + 24, offset as i64 + 64);
        put(&mut input, offset + 64, b"pamm");
    }
    assert!(projector_view(&input, None).err().unwrap().contains("expected one"));
    let archives = |input: &[u8]| -> Value {
        Value::Array([64usize, 288].iter().map(|&o| json!({"offset": o, "bytes": 192, "sha256": sha256(&input[o..o + 192])})).collect())
    };
    let pinned = archives(&input);
    let select = |offset: usize, archives: &Value| json!({"offset": offset, "archives": archives});
    assert_eq!(projector_view(&input, Some(&select(288, &pinned))).unwrap().offset, 288);
    assert!(projector_view(&input, Some(&select(0, &pinned))).err().unwrap().contains("selection changed"));
    let partial = Value::Array(pinned.as_array().unwrap()[..1].to_vec());
    assert!(projector_view(&input, Some(&select(64, &partial))).err().unwrap().contains("selection changed"));
    input[200] += 1;
    assert!(projector_view(&input, Some(&select(64, &pinned))).err().unwrap().contains("selection changed"));
}

#[test]
fn two_bit_pixels_and_modern_windows_palette_retain_their_source_colors() {
    let palette = builtin_palette(0, 2).unwrap();
    assert_eq!(decode_palette(&palette).unwrap(), [[255, 255, 255], [163, 163, 163], [101, 101, 101], [0, 0, 0]]);
    let meta = Meta {
        width: 4, height: 1, depth: 2, pitch: 1, palette: 0, palette_cast: 0, reg_x: 0, reg_y: 0,
        use_alpha: false, alpha_threshold: 0, update_flags: 0,
    };
    let pixels = bitmap_pixels(&meta, &[0x1b], Some(&palette), false).unwrap();
    assert_eq!(pixels.len(), 8);
    assert_eq!(read_be16(&pixels, 0), 0xfffe);
    assert_eq!(read_be16(&pixels, 6), 1);
    assert_eq!(decode_palette(&builtin_palette(-101, 4).unwrap()).unwrap()[8], [160, 160, 164]);
    assert_eq!(decode_palette(&builtin_palette(-100, 4).unwrap()).unwrap()[8], [192, 192, 192]);
    assert_eq!(decode_palette(&builtin_palette(-101, 8).unwrap()).unwrap()[249], [0, 128, 128]);
}

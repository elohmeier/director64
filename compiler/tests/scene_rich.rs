use director64_aot::scene::rich;
use serde_json::json;
fn run(offset: u32) -> serde_json::Value {
    json!({"offset":offset,"fontId":1,"size":18,"face":3,"align":1,
        "ascent":16,"lineHeight":21,"color":0x123456})
}
#[test]
fn text_run_offsets_use_utf16_and_preserve_face_and_metrics() {
    let runs = rich::read(&json!([run(0), run(4)]), "A😀B", true, 3).unwrap();
    assert_eq!(runs[1].offset, 4);
    assert_eq!(runs[0].face, 3);
    assert_eq!(runs[0].line_height, 21);
    assert_eq!(runs[0].rgb, 0x123456);
    assert_eq!(runs[0].char_spacing, 0);
    for spacing in [-32768, -1000, -1, 1, 32767] {
        let mut spaced = run(0);
        spaced["charSpacing"] = json!(spacing);
        assert_eq!(
            rich::read(&json!([spaced]), "AB", true, 7).unwrap()[0].char_spacing,
            spacing
        );
    }
    let mut justified = run(0);
    justified["align"] = json!(3);
    assert_eq!(
        rich::read(&json!([justified]), "AB", true, 7).unwrap()[0].align,
        3
    );
    let mut zero = run(0);
    zero["size"] = json!(0);
    assert_eq!(
        rich::read(&json!([zero]), "AB", true, 7).unwrap()[0].size,
        0
    );
    assert!(rich::read(&json!([run(0), run(5)]), "A😀B", true, 3).is_err());
    assert!(rich::read(&json!([run(1)]), "A😀B", true, 3).is_err());
    assert!(rich::read(&json!([run(0), run(0)]), "AB", true, 3).is_err());
}
#[test]
fn text_runs_reject_wrong_member_profile_and_unbounded_metadata() {
    for (modern, kind) in [(false, 3), (true, 1)] {
        assert!(rich::read(&json!([run(0)]), "AB", modern, kind).is_err());
    }
    let many = vec![run(0); 4097];
    assert!(rich::read(&json!(many), "AB", true, 3).is_err());
    for (key, value) in [
        ("size", json!(256)),
        ("fontId", json!(256)),
        ("face", json!(16)),
        ("align", json!(4)),
        ("ascent", json!(-32769)),
        ("lineHeight", json!(-1)),
        ("color", json!(0x1000000)),
        ("offset", json!(1.5)),
        ("charSpacing", json!(-32769)),
        ("charSpacing", json!(32768)),
        ("charSpacing", json!(1.5)),
        ("charSpacing", json!(null)),
    ] {
        let mut bad = run(0);
        bad[key] = value;
        assert!(rich::read(&json!([bad]), "AB", true, 7).is_err(), "{key}");
    }
    assert!(rich::read(&json!([]), "", false, 3).unwrap().is_empty());
}

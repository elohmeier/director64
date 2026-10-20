use director64_aot::{
    capabilities,
    scene::{self, Profile},
};
use serde_json::{Value, json};
fn model(version: u16, cast: i16, member: i16) -> Value {
    let mut bytes = vec![0u8; if version >= 700 { 48 } else { 24 }];
    bytes[..2].copy_from_slice(&cast.to_be_bytes());
    bytes[2..4].copy_from_slice(&member.to_be_bytes());
    bytes[4] = 30;
    bytes[9] = 1;
    bytes[11] = 1;
    json!({"problems":[],"extendedD6":true,"movies":[
 {"id":1,"name":"MAIN.DXR","directorVersion":version,"casts":[{"number":1,"name":"Shared","file":"COLORS.CXT"}],"members":[],
 "score":{"frames":[{"number":10,"channels":[{"channel":5,"bytes":bytes,"changed":vec![true;bytes.len()],"behaviors":[]}]}]}},
 {"id":2,"name":"COLORS.CXT","directorVersion":version,"casts":[{"number":1,"name":"Internal","file":"COLORS.CXT","patternAssets":["tile.fdi",null,null,null,null,null,null,null]}],
 "members":[{"cast":1,"number":3,"type":4,"palette":vec![0x123456u32;256]}]}]})
}
#[test]
fn palette_commands_preserve_builtins_custom_cast_identity_and_rgb() {
    for version in [500, 600, 700, 800, 850, 1000] {
        let p = Profile {
            version: if version == 850 { 1000 } else { version },
            extended: true,
            full_engine: false,
        };
        for id in [-1, -3, -102] {
            let model = model(version, if id == -1 { 0 } else { -1 }, id);
            let scenes = scene::scenes(&model, p).unwrap();
            assert_eq!(scenes[0].deltas[0].spec.member, id as u16 as u32);
            assert!(
                capabilities::audit(&model, capabilities::Target::Web, None).unwrap()["findings"]
                    .as_array()
                    .unwrap()
                    .is_empty()
            );
        }
        let scenes = scene::scenes(&model(version, 1, 3), p).unwrap();
        assert_eq!(scenes[0].deltas[0].spec.member, 2 << 21 | 1 << 16 | 3);
        assert_eq!(scenes[1].members[0].palette[17], 0x123456);
        assert_eq!(scenes[1].casts[0].pattern_assets[0], "tile.fdi");
        assert_eq!(scenes[1].casts[0].pattern_assets[1], "");
    }
}
#[test]
fn unsupported_palette_interpolation_and_missing_colors_fail_before_build() {
    let p = Profile {
        version: 1000,
        extended: true,
            full_engine: false,
    };
    for (offset, value) in [
        (5, 0x80),
        (5, 4),
        (9, 2),
        (11, 2),
        (12, 1),
        (13, 1),
        (14, 1),
    ] {
        let mut m = model(850, -1, -1);
        m["movies"][0]["score"]["frames"][0]["channels"][0]["bytes"][offset] = json!(value);
        assert!(
            scene::scenes(&m, p)
                .err()
                .unwrap()
                .contains("interpolation")
        );
        assert_eq!(
            capabilities::audit(&m, capabilities::Target::Web, None).unwrap()["findings"][0]["code"],
            "palette-interpolation-unsupported"
        );
    }
    let mut m = model(850, 1, 3);
    m["movies"][1]["members"][0]["palette"] = Value::Null;
    assert_eq!(
        capabilities::audit(&m, capabilities::Target::Web, None).unwrap()["findings"][0]["code"],
        "palette-member-colors-missing"
    );
}

#[test]
fn black_and_white_palette_fades_keep_visual_substitute_metadata() {
    let p = Profile {version:600,extended:true,full_engine:true};
    for flag in [0x20,0x40] {
        let mut m = model(600,-1,-102);
        m["movies"][0]["score"]["frames"][0]["channels"][0]["bytes"][5] = json!(flag);
        let scenes = scene::scenes(&m,p).unwrap();
        assert_eq!(scenes[0].deltas[0].spec.ink, flag);
        assert_eq!(scenes[0].deltas[0].spec.blend,30);
        let report=capabilities::audit(&m,capabilities::Target::Web,Some(p)).unwrap();
        assert!(report["findings"].as_array().unwrap().is_empty());
        assert_eq!(report["palette_fade_substitutes"][0]["behavior"],"truecolor-scene-fade-substitute");
    }
}

#[test]
fn deleted_and_nonpalette_score_references_are_audited_as_content_not_capabilities() {
    for kind in [11, 8] {
        let mut m = model(850, 1, 3);
        m["movies"][1]["members"][0]["type"] = json!(kind);
        let report = capabilities::audit(&m, capabilities::Target::Web, None).unwrap();
        assert!(report["findings"].as_array().unwrap().is_empty());
        assert_eq!(
            report["ignored_palette_commands"][0]["reason"],
            "not-palette"
        );
    }
    let mut m = model(850, 1, 3);
    m["movies"][1]["members"] = json!([]);
    let report = capabilities::audit(&m, capabilities::Target::Web, None).unwrap();
    assert!(report["findings"].as_array().unwrap().is_empty());
    assert_eq!(report["ignored_palette_commands"][0]["reason"], "missing");
}

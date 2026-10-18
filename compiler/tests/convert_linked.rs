use director64_aot::convert::linked::{metadata, resolve};
use serde_json::{Value, json};

#[test]
fn linked_info_ignores_later_binary_authoring_slots() {
    use director64_aot::convert::d5media::cast_string;
    let entries: [&[u8]; 5] = [
        b"",
        b"\x03Dog",
        b"\x04DATA",
        b"\x09Child.dir",
        &[0xff, 0, 0, 1],
    ];
    let mut info = vec![0u8; 20];
    info[..4].copy_from_slice(&20u32.to_be_bytes());
    info.extend_from_slice(&(entries.len() as u16).to_be_bytes());
    let mut offset = 0u32;
    for entry in entries {
        info.extend_from_slice(&offset.to_be_bytes());
        offset += entry.len() as u32;
    }
    info.extend_from_slice(&offset.to_be_bytes());
    for entry in entries {
        info.extend_from_slice(entry);
    }
    let mut cast = vec![0u8; 12];
    cast[4..8].copy_from_slice(&(info.len() as u32).to_be_bytes());
    cast.extend_from_slice(&info);
    assert_eq!(cast_string(&cast, 2).unwrap(), "DATA");
    assert_eq!(cast_string(&cast, 3).unwrap(), "Child.dir");
    assert!(cast_string(&cast, 4).is_err());
}

fn sources() -> serde_json::Map<String, Value> {
    json!({"ROOM__INTRO.DXR":"ROOM/INTRO.DCR", "OTHER__INTRO.DXR":"OTHER/INTRO.DIR",
        "LEVELWAA.DXR":"LEVELWAA.DCR"})
    .as_object()
    .unwrap()
    .clone()
}

#[test]
fn linked_cast_preserves_nonzero_signed_origin_and_raw_flags() {
    let mut bytes = Vec::new();
    for n in [-12i16, 192, 88, 392] {
        bytes.extend_from_slice(&n.to_be_bytes());
    }
    bytes.extend_from_slice(&0x39u32.to_be_bytes());
    bytes.extend_from_slice(&17u16.to_be_bytes());
    let m = metadata(&bytes, "@\\ROOM\\", "Intro.dir").unwrap();
    assert_eq!(
        (m["linkedTop"].clone(), m["linkedLeft"].clone()),
        (json!(-12), json!(192))
    );
    assert_eq!(
        (m["width"].clone(), m["height"].clone()),
        (json!(200), json!(100))
    );
    assert_eq!(
        (m["regX"].clone(), m["regY"].clone()),
        (json!(100), json!(50))
    );
    assert_eq!(m["linkedFlags"], 0x39);
    assert_eq!(m["linkedUnknown"], 17);
    assert_eq!(m["linkedFile"], "Intro.dir");
    assert!(metadata(&bytes[..13], "", "Intro.dir").is_err());
    assert!(metadata(&bytes, "", "").is_err());
}

#[test]
fn linked_targets_resolve_exact_folder_paths_and_canonical_extensions() {
    let policy = json!({"folder_qualified_names":true});
    let all = sources();
    assert_eq!(
        resolve("", "Intro.dir", "ROOM/MAIN.DCR", &all, &policy).unwrap(),
        "ROOM__INTRO.DXR"
    );
    assert_eq!(
        resolve(
            "@\\..\\OTHER\\",
            "Intro.dcr",
            "ROOM/MAIN.DCR",
            &all,
            &policy
        )
        .unwrap(),
        "OTHER__INTRO.DXR"
    );
    assert_eq!(
        resolve(
            "D:\\author\\OTHER\\",
            "Intro.dir",
            "ROOM/MAIN.DCR",
            &all,
            &policy
        )
        .unwrap(),
        "OTHER__INTRO.DXR"
    );
    assert!(resolve("", "Intro.dir", "MAIN.DCR", &all, &policy).is_err());
    assert!(
        resolve("", "Levelwaage.dir", "MAIN.DCR", &all, &policy).is_err(),
        "never guess a DOS stem"
    );
}

#[test]
fn verified_joliet_aliases_resolve_long_names_without_truncation_guesses() {
    let policy = json!({"source_path_aliases":{"Levelwaage.dir":"LEVELWAA.DIR"}});
    assert_eq!(
        resolve("", "Levelwaage.dir", "MAIN.DCR", &sources(), &policy).unwrap(),
        "LEVELWAA.DXR"
    );
    assert!(resolve("", "Levelwaage2.dir", "MAIN.DCR", &sources(), &policy).is_err());
    assert_eq!(
        resolve(
            "OLD DISK:Project:DATA",
            "Levelwaage.dir",
            "MAIN.DCR",
            &sources(),
            &policy
        )
        .unwrap(),
        "LEVELWAA.DXR"
    );
}

#[test]
fn extensionless_movie_names_still_require_an_exact_manifest_basename() {
    assert_eq!(
        resolve("@\\OTHER\\", "Intro", "MAIN.DCR", &sources(), &json!({})).unwrap(),
        "OTHER__INTRO.DXR"
    );
    assert!(resolve("@\\OTHER\\", "Int", "MAIN.DCR", &sources(), &json!({})).is_err());
}

#[test]
fn scene_member_resolves_linked_registry_identity_without_losing_origin() {
    let model = json!({"problems":[],"movies":[
        {"id":1,"name":"MAIN.DXR","directorVersion":850,"members":[
            {"cast":1,"number":1,"type":9,"width":200,"height":100,
             "linkedMovie":"CHILD.DXR","linkedFlags":0x19,"linkedLeft":192,"linkedTop":-12}],
         "casts":[{"number":1,"name":"Internal","file":"MAIN.DXR"}]},
        {"id":2,"name":"CHILD.DXR","directorVersion":850,"members":[],"casts":[]}]});
    let scenes = director64_aot::scene::scenes(
        &model,
        director64_aot::scene::Profile {
            version: 1000,
            extended: false,
        },
    )
    .unwrap();
    let member = &scenes[0].members[0];
    assert_eq!(
        (
            member.linked_movie,
            member.linked_flags,
            member.linked_left,
            member.linked_top
        ),
        (2, 0x19, 192, -12)
    );
}

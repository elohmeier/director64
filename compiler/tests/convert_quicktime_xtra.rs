use director64_aot::convert::quicktime_xtra::{media_candidates, metadata};
use director64_aot::ports::{Port, PortFonts, postprocess_model};
use serde_json::{Value, json};

fn payload(flags: u32) -> Vec<u8> {
    let mut bytes = vec![0; 54];
    bytes[..4].copy_from_slice(&[0x11, 0, 0x11, 4]);
    bytes[4..8].copy_from_slice(&flags.to_be_bytes());
    bytes[48..50].copy_from_slice(&320u16.to_be_bytes());
    bytes[50..52].copy_from_slice(&240u16.to_be_bytes());
    bytes[52..54].copy_from_slice(&16u16.to_be_bytes());
    bytes
}

#[test]
fn quicktime_payload_preserves_timing_flags_and_rejects_unhandled_geometry() {
    let result = metadata(
        &payload(0x18218 | 0x140),
        "Mac:DATA:MPEG",
        "Test.mpg",
        "MPEG/VIDEOS.CCT",
        &json!({}),
    )
    .unwrap();
    assert_eq!(result["type"], 10);
    assert_eq!(result["sourceType"], 15);
    assert_eq!(result["width"], 320);
    assert_eq!(result["height"], 240);
    assert_eq!(result["regX"], 160);
    assert_eq!(result["videoFlags"], 8 | 16 | 256);
    assert!(metadata(&payload(3), "", "x.mpg", "", &json!({})).is_err());
    assert!(metadata(&payload(8)[..53], "", "x.mpg", "", &json!({})).is_err());
}

#[test]
fn external_paths_use_verified_aliases_without_short_name_guessing() {
    let policy = json!({"source_path_aliases":{"MPEG/Long Film.mpg":"MPEG/LONGFI~1.MPG"}});
    assert!(
        media_candidates("Old:DATA:MPEG", "Long Film.mpg", "MPEG/VIDEOS.CCT", &policy)
            .contains(&"MPEG/LONGFI~1.MPG".into())
    );
    assert!(
        !media_candidates("", "Long Film2.mpg", "MPEG/VIDEOS.CCT", &policy)
            .contains(&"MPEG/LONGFI~1.MPG".into())
    );
    assert!(
        !media_candidates("", "Long Film.mpg", "MPEG/VIDEOS.CCT", &json!({}))
            .contains(&"MPEG/LONGFILM.MPG".into())
    );
}

fn bind(model: &mut Value, records: Vec<Value>) -> Result<(), String> {
    postprocess_model(
        model,
        &Port {
            slug: "loewenzahn-7".into(),
            ..Port::default()
        },
        &PortFonts {
            external_media: records,
            ..PortFonts::default()
        },
        &mut |_| unreachable!(),
    )
}
fn model() -> Value {
    json!({"movies":[{"name":"MAIN.DXR","members":[
        {"type":10,"xtra":{"symbol":"quickTimeMedia"},"linkedFile":"Long Film.mpg",
         "linkedSourceCandidates":["MPEG/Long Film.mpg","MPEG/LONGFI~1.MPG"],"videoFlags":280},
        {"type":15,"xtra":{"symbol":"TBDIRECTMEDIA"},"linkedFile":"Long Film.mpg",
         "linkedSourceCandidates":["MPEG/LONGFI~1.MPG"],"videoDurationMs":2050}]}]})
}
fn record() -> Value {
    json!({"source":"MPEG/LONGFI~1.MPG","video":"movie.vp8","audio":"audio.wav64","duration":2.25})
}
#[test]
fn both_xtras_bind_one_stream_with_their_own_duration_and_pause_flags() {
    let mut model = model();
    bind(&mut model, vec![record()]).unwrap();
    let members = model["movies"][0]["members"].as_array().unwrap();
    assert_eq!(members[0]["frames"], 1350);
    assert_eq!(members[0]["videoFlags"], 280);
    assert_eq!(members[1]["frames"], 1230);
    assert_eq!(members[1]["videoFlags"], 8);
    assert_eq!(members[1]["type"], 10);
    for member in members {
        assert_eq!(member["asset"], "movie.vp8");
        assert_eq!(member["rate"], 600);
    }
}
#[test]
fn unconverted_or_absent_video_is_an_error_not_an_ended_clip() {
    assert!(
        bind(&mut model(), vec![])
            .unwrap_err()
            .contains("unresolved external video")
    );
    let mut missing = record();
    missing["video"] = json!("");
    assert!(
        bind(&mut model(), vec![missing])
            .unwrap_err()
            .contains("was not converted")
    );
    let mut missing = record();
    missing["duration"] = json!(0);
    assert!(
        bind(&mut model(), vec![missing])
            .unwrap_err()
            .contains("invalid external video duration")
    );
}
#[test]
fn scene_preserves_quicktime_identity_and_video_streams() {
    let m = json!({"problems":[],"movies":[{"id":1,"name":"MAIN.DXR","directorVersion":850,
       "casts":[{"number":1,"name":"Internal","file":"MAIN.DXR"}],
       "members":[{"type":10,"number":1,"cast":1,"xtra":{"symbol":"quickTimeMedia"},
       "asset":"movie.vp8","videoAudio":"audio.wav64","frames":1350,"rate":600,"videoFlags":280}]}]});
    let scenes = director64_aot::scene::scenes(
        &m,
        director64_aot::scene::Profile {
            version: 1000,
            extended: false,
        },
    )
    .unwrap();
    let m = &scenes[0].members[0];
    assert_eq!(m.source_xtra, 4);
    assert_eq!(m.samples, 1350);
    assert_eq!(m.video_flags, 280);
}

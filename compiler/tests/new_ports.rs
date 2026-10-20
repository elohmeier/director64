//! Source-free contracts for the additional D6-D8 ports.
use director64_aot::convert::{
    d5media::styled_text, film, patterns::inline_tile, source::sha256, text::bind_text_fonts,
};
use director64_aot::{
    parse_json,
    ports::{self, Port, PortFonts},
    scene,
};
use serde_json::{Value, json};

#[test]
fn recovered_deep_control_flow_is_bounded_before_json_deserialization() {
    let source = format!("{}0{}", "[".repeat(490), "]".repeat(490));
    assert!(parse_json(&source).is_ok());
    assert!(
        parse_json(&"[".repeat(1025))
            .unwrap_err()
            .contains("nesting")
    );
    assert!(parse_json("[] garbage").is_err());
    assert_eq!(parse_json(r#"{"text":"[\"[]"}"#).unwrap()["text"], "[\"[]");
}

#[test]
fn extended_d6_keeps_high_frame_rates_separate_from_wait_opcodes() {
    let mut record = [0u8; 24];
    record[4..6].copy_from_slice(&500u16.to_be_bytes());
    record[6] = 246;
    assert_eq!(scene::score_tempo(&record, 6, true).unwrap(), 500);
    assert!(scene::score_tempo(&record, 6, false).is_err());
    for opcode in [248, 254, 255] {
        record[6] = opcode;
        assert_eq!(scene::score_tempo(&record, 6, true).unwrap(), 0);
    }
}

#[test]
fn styled_text_accepts_a_bounded_style_table_larger_than_256_records() {
    let count = 266u16;
    let mut bytes = 12u32.to_be_bytes().to_vec();
    bytes.extend(1u32.to_be_bytes());
    bytes.extend((2 + count as u32 * 20).to_be_bytes());
    bytes.push(b'A');
    bytes.extend(count.to_be_bytes());
    bytes.extend(vec![0; count as usize * 20]);
    assert_eq!(
        styled_text(&bytes).unwrap()["sourceTextStyles"]
            .as_array()
            .unwrap()
            .len(),
        266
    );
    bytes.pop();
    assert!(styled_text(&bytes).is_err());
}

#[test]
fn stock_tiles_come_from_a_hash_pinned_bounded_archive() {
    let mut data = Vec::new();
    for index in 0..8u8 {
        let length = if index == 0 { 2048u32 } else { 256 };
        data.extend(length.to_be_bytes());
        data.extend(vec![index; length as usize]);
    }
    let spec = json!({"offset":3,"bytes":data.len(),"sha256":sha256(&data),"palette":"systemWin"});
    let input = [vec![0, 0, 0], data].concat();
    for index in 0..8 {
        let (w, h, pixels) = inline_tile(&input, &spec, index).unwrap();
        assert_eq!((w, h), if index == 0 { (64, 32) } else { (16, 16) });
        assert_eq!(pixels.len(), w * h * 4);
    }
    let mut changed = input.clone();
    changed[9] ^= 1;
    assert!(inline_tile(&changed, &spec, 0).is_err());
    assert!(inline_tile(&input[..input.len() - 1], &spec, 0).is_err());
}

#[test]
fn an_imported_films_host_cast_must_have_one_identity() {
    let external =
        json!({"name":"DATA.CXT","casts":[{"number":1,"file":"DATA.CXT","filmCastMap":[3]}]});
    let film = json!({"cast":1});
    let host = json!({"name":"MAIN.DXR","casts":[{"number":2,"file":"DATA.CXT"},{"number":3,"file":"MAIN.DXR"}]});
    let movies = vec![external.clone(), host.clone()];
    let (target, cast) = film::reference(&external, &film, 0, &movies).unwrap();
    assert_eq!((&target["name"], cast), (&json!("MAIN.DXR"), 3));
    let mut other = host;
    other["name"] = json!("OTHER.DXR");
    other["casts"][1]["file"] = json!("OTHER.DXR");
    assert!(film::reference(&external, &film, 0, &[movies, vec![other]].concat()).is_err());
    let missing = json!({"name":"EMPTY.DXR","casts":[{"number":2,"file":"DATA.CXT"}]});
    assert!(film::reference(&external, &film, 0, &[external.clone(), missing]).is_err());
}

#[test]
fn identical_font_family_names_resolve_in_the_owning_movie_scope() {
    let movie = |name: &str| {
        json!({"name":name,"casts":[{"file":name,"fontMap":{"1":"Face"}}],
        "members":[{"cast":1,"number":1,"type":3,"text":"A","sourceTextStyles":[
            {"sourceFontId":1,"size":12,"lineHeight":14,"ascent":11,"color":0}]}]})
    };
    let mut fonts = vec![
        json!({"number":1,"aliases":["Face"],"sources":[{"movie":"A.DXR"}]}),
        json!({"number":2,"aliases":["Face"],"sources":[{"movie":"B.DXR"}]}),
    ];
    let mut movies = vec![movie("A.DXR"), movie("B.DXR"), movie("C.DXR")];
    let limits = bind_text_fonts(&mut movies, &mut fonts, true).unwrap();
    assert_eq!(movies[0]["members"][0]["textStyle"]["fontId"], 1);
    assert_eq!(movies[1]["members"][0]["textStyle"]["fontId"], 2);
    assert_eq!(limits[0]["movie"], "C.DXR");
    movies[0]["casts"]
        .as_array_mut()
        .unwrap()
        .push(json!({"file":"B.DXR"}));
    fonts[0]["sources"] = json!([{"movie":"B.DXR"}]);
    assert!(bind_text_fonts(&mut movies, &mut fonts, true).is_err());
}

#[test]
fn linked_sound_duplicates_bind_by_authored_folder_and_keep_cues() {
    let mut model = json!({"problems":[],"movies":[{"members":[
        {"type":6,"empty":true,"name":"voice","linkedDirectory":"Disk:B"}]}]});
    let records = vec![
        json!({"source":"A/voice.aif","audio":"a.wav64","rate":1000,"frames":10}),
        json!({"source":"B/voice.aif","audio":"b.wav64","rate":1000,"frames":20,
            "cuePoints":[{"milliseconds":10,"name":"talk"}]}),
    ];
    ports::postprocess_model(
        &mut model,
        &Port {
            convert: json!({"external_media":{"bind":"linked-sounds"}}),
            ..Port::default()
        },
        &PortFonts {
            external_media: records,
            ..PortFonts::default()
        },
        &mut |_| Err("unexpected font request".into()),
    )
    .unwrap();
    let member = &model["movies"][0]["members"][0];
    assert_eq!(member["asset"], "b.wav64");
    assert_eq!(member["cuePoints"][0]["name"], "talk");
}

#[test]
fn pinned_startup_initializers_preserve_the_recovered_handler() {
    let program = json!({"handlers":[{"movie":"START.DXR","member":1,"cast":"Internal","name":"startmovie",
        "source_sha256":"pin","body":[{"op":"return","value":["number",7],"line":1}]}]});
    let patch = json!([{"movie":"START.DXR","member":1,"handler":"startmovie","source_sha256":"pin","prepend":"global drive\nset drive to 1"}]);
    let result = ports::patch_program(&program, &patch).unwrap();
    let body = &result["handlers"][0]["body"];
    assert_eq!(
        body.as_array().unwrap().last().unwrap(),
        &program["handlers"][0]["body"][0]
    );
    let mut changed = program;
    changed["handlers"][0]["source_sha256"] = Value::Null;
    assert!(ports::patch_program(&changed, &patch).is_err());
}

#[test]
fn full_engine_keeps_the_director6_live_film_record_layout() {
    let mut bytes = vec![0u8; 24];
    bytes[0] = 16;
    bytes[1] = 32;
    bytes[2] = 2;
    bytes[4..6].copy_from_slice(&65535u16.to_be_bytes());
    bytes[7] = 1;
    bytes[13] = 3;
    bytes[15] = 4;
    bytes[17] = 5;
    bytes[19] = 6;
    let mut palette = vec![0xffffffu32; 256];
    palette[2] = 0x123456;
    let model = json!({"problems":[],"extendedD6":true,"fullEngine":true,"movies":[
        {"id":1,"name":"MAIN.DXR","directorVersion":600,"fullEngine":true,"palette":palette,
         "casts":[{"number":1,"name":"Internal","file":"MAIN.DXR"}],"members":[
          {"cast":1,"number":1,"type":1,"asset":"pixel.fdi","width":6,"height":5},
          {"cast":1,"number":2,"type":2,"filmRuntime":true,"filmAssets":["pose.fdi"],
           "filmScore":{"version":11,"recordSize":24,"frames":[{"channels":[{"channel":6,"bytes":bytes}]}]}}]}]});
    let scenes = scene::scenes(
        &model,
        scene::Profile {
            version: 600,
            extended: true,
            full_engine: true,
        },
    )
    .unwrap();
    let layer = &scenes[0].members[1].film_tracks[0].keys[0].value;
    assert_eq!((layer.x, layer.y, layer.width, layer.height), (4, 3, 6, 5));
    assert_eq!(
        (layer.fore_rgb, layer.rotation, layer.skew),
        (0x123456, 0, 0)
    );
}

#[test]
fn aifc_ima4_and_aiff_marker_positions_keep_source_timing() {
    use director64_aot::convert::quicktime::{aiff_cues, aiff_wav};
    fn chunk(tag: &[u8; 4], body: &[u8]) -> Vec<u8> {
        let mut out = tag.to_vec();
        out.extend((body.len() as u32).to_be_bytes());
        out.extend(body);
        if body.len() % 2 != 0 {
            out.push(0);
        }
        out
    }
    let mut comm = 1u16.to_be_bytes().to_vec();
    comm.extend(1u32.to_be_bytes());
    comm.extend(16u16.to_be_bytes());
    comm.extend([0x40, 0x0d, 0xac, 0x44, 0, 0, 0, 0, 0, 0]);
    comm.extend(b"ima4");
    comm.extend([0, 0]);
    let mut ssnd = vec![0; 8];
    ssnd.extend([1, 0]);
    ssnd.extend([0x88; 32]);
    let mut mark = 1u16.to_be_bytes().to_vec();
    mark.extend(7u16.to_be_bytes());
    mark.extend(22050u32.to_be_bytes());
    mark.extend([1, b'X']);
    let body = [
        b"AIFC".to_vec(),
        chunk(b"COMM", &comm),
        chunk(b"MARK", &mark),
        chunk(b"SSND", &ssnd),
    ]
    .concat();
    let bytes = [
        b"FORM".to_vec(),
        (body.len() as u32).to_be_bytes().to_vec(),
        body,
    ]
    .concat();
    let wav = aiff_wav(&bytes).unwrap();
    assert_eq!(wav.len(), 44 + 64 * 2);
    assert!(wav[44..].chunks_exact(2).all(|s| s == [0, 1]));
    assert_eq!(
        aiff_cues(&bytes).unwrap()[0],
        json!({"milliseconds":1000,"name":"X"})
    );
    let mut truncated = bytes;
    truncated.truncate(truncated.len() - 2);
    assert!(aiff_wav(&truncated).is_err());
}

#[test]
fn a_truncated_sound_can_only_use_its_exact_pinned_same_header_copy() {
    use director64_aot::convert::{
        files::{Files, Memory},
        movie::{MovieJob, Sources, convert_movie},
        plan,
    };
    use std::collections::HashMap;
    let mut header = vec![0u8; 78];
    for (at, value) in [(0, 2u16), (4, 1), (6, 0x8051), (22, 22050), (62, 16)] {
        header[at..at + 2].copy_from_slice(&value.to_be_bytes());
    }
    for (at, value) in [(10, 14u32), (18, 2), (36, 4)] {
        header[at..at + 4].copy_from_slice(&value.to_be_bytes());
    }
    header[34] = 255;
    header[35] = 60;
    let broken = [header.clone(), vec![0; 4]].concat();
    let complete = [header, vec![0; 16]].concat();
    fn dump(records: Value, chunks: Value, data: &[u8]) -> Vec<u8> {
        let header =
            serde_json::to_vec(&json!({"json":records,"scripts":{},"chunks":chunks})).unwrap();
        [
            b"D64DUMP1".to_vec(),
            (header.len() as u32).to_le_bytes().to_vec(),
            header,
            data.to_vec(),
        ]
        .concat()
    }
    let records = json!([
      {"fourCC":"DRCF","id":1,"data":{"minMember":1,"maxMember":1,"frameRate":30}},
      {"fourCC":"KEY*","id":2,"data":{"usedCount":2,"entryCount":2,"entries":[
       {"castID":1024,"fourCC":"CAS*","sectionID":3},{"castID":4,"fourCC":"snd ","sectionID":5}]}},
      {"fourCC":"CAS*","id":3,"data":{"memberIDs":[4]}},
      {"fourCC":"CASt","id":4,"data":{"type":6,"info":{"name":"voice","flags":16}}}]);
    let mut fs = Memory::new();
    fs.mkdir_all("dumps").unwrap();
    fs.mkdir_all("out/wav").unwrap();
    for (file, bytes) in [
        (
            "TARGET.DCR",
            dump(
                records,
                json!([["CASt", 4, 12], ["snd ", 5, broken.len()]]),
                &[vec![0; 12], broken.clone()].concat(),
            ),
        ),
        (
            "DONOR.DCR",
            dump(json!([]), json!([["snd ", 5, complete.len()]]), &complete),
        ),
    ] {
        let key = sha256(plan::file_key(file).as_bytes());
        fs.write(&format!("dumps/{}.dump", &key[..24]), &bytes)
            .unwrap();
    }
    let source = json!({"name":"TARGET.DXR","source_path":"TARGET.DCR","director_version":850});
    let mut policy = json!({"sound_resource_copies":[{"movie":"TARGET.DXR","cast":1,"member":1,
        "source_sha256":sha256(&broken),"file":"DONOR.DCR","resource":5,"replacement_sha256":sha256(&complete)}]});
    let paths = serde_json::Map::new();
    for valid in [true, false] {
        if !valid {
            policy["sound_resource_copies"][0]["replacement_sha256"] = json!("changed");
        }
        let mut sources = Sources {
            media: "",
            dumps: "dumps",
            source_paths: &paths,
            palettes: HashMap::new(),
        };
        let job = MovieJob {
            source: &source,
            movie_id: 1,
            policy: &policy,
            recovery: "",
            output: "out",
            common_palette: None,
            common_palette_hash: None,
            d5_stage_palette: None,
        };
        let result = convert_movie(&mut fs, &mut sources, &job).unwrap();
        if valid {
            assert!(result.problems.is_empty(), "{:?}", result.problems);
            assert_eq!(result.movie["members"][0]["frames"], 4);
        } else {
            assert!(
                result.problems[0]["error"]
                    .as_str()
                    .unwrap()
                    .contains("identity/header changed")
            );
        }
    }
}

mod common;
use common::*;
use director64_aot::convert::{
    files::{Files, Memory},
    movie::{MovieJob, Sources, convert_movie},
    plan,
};
use serde_json::json;
use std::collections::HashMap;

#[test]
fn stripped_original_image_keeps_complete_runtime_pixels_and_rejects_incomplete_pixels() {
    for pixels in [Some(vec![255, 255, 0, 0]), Some(vec![255]), None] {
        let mut cast = vec![0; 40];
        be32(&mut cast, 0, 1);
        be32(&mut cast, 8, 28);
        be16(&mut cast, 12, 0x8004);
        be16(&mut cast, 18, 1);
        be16(&mut cast, 20, 1);
        cast[35] = 32;
        let mut chunks = vec![json!(["CASt", 4, cast.len()])];
        if let Some(pixels) = &pixels {
            chunks.push(json!(["BITD", 5, pixels.len()]));
        }
        let header = json!({"json": [
            {"fourCC":"DRCF","id":1,"data":{"minMember":1,"maxMember":1,"frameRate":30}},
            {"fourCC":"KEY*","id":2,"data":{"usedCount":3,"entryCount":3,"entries":[
                {"castID":1024,"fourCC":"CAS*","sectionID":3},
                {"castID":4,"fourCC":"BITD","sectionID":5},
                {"castID":4,"fourCC":"ediM","sectionID":6}]}},
            {"fourCC":"CAS*","id":3,"data":{"memberIDs":[4]}},
            {"fourCC":"CASt","id":4,"data":{"type":1,"info":{"name":"pixel"}}}
        ],"scripts":{},"chunks":chunks});
        let json = serde_json::to_vec(&header).unwrap();
        let mut dump = b"D64DUMP1".to_vec();
        dump.extend((json.len() as u32).to_le_bytes());
        dump.extend(json);
        dump.extend(cast);
        if let Some(pixels) = &pixels {
            dump.extend(pixels);
        }
        let mut fs = Memory::new();
        fs.mkdir_all("dumps").unwrap();
        fs.mkdir_all("out/images").unwrap();
        let key = director64_aot::convert::source::sha256(plan::file_key("TEST.DCR").as_bytes());
        fs.write(&format!("dumps/{}.dump", &key[..24]), &dump)
            .unwrap();
        let source = json!({"name":"TEST.DXR","source_path":"TEST.DCR","director_version":850});
        let policy = json!({});
        let paths = serde_json::Map::new();
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
        if pixels.as_ref().is_some_and(|p| p.len() == 4) {
            assert!(result.problems.is_empty(), "{:?}", result.problems);
            let member = &result.movie["members"][0];
            assert_ne!(member["empty"], true);
            let asset = member["asset"].as_str().unwrap();
            let bytes = fs.read(&format!("out/images/{asset}")).unwrap();
            // A complete opaque red pixel survives the actual movie path.
            assert_eq!(&bytes[32..], &[0xf8, 0x01]);
        } else {
            assert_eq!(result.problems.len(), 1);
            assert!(result.movie["members"][0].get("asset").is_none());
        }
    }
}

#[test]
fn mp3_cast_decodes_when_needed_and_preserves_complete_pcm_with_or_without_classic_sound() {
    for (valid, paired, classic) in [
        (true, false, true),
        (false, false, true),
        (false, true, true),
        (false, true, false),
    ] {
        let cast = vec![0; 12];
        let mut mp3 = vec![0; 417];
        mp3[..4].copy_from_slice(&[255, 251, 144, 192]);
        mp3 = mp3.repeat(3);
        if !valid {
            mp3.truncate(mp3.len() - 1);
        }
        let mut pcm_header = vec![0u8; 84];
        for (offset, value) in [(40, 4u32), (44, 22050), (68, 16), (76, 1)] {
            pcm_header[offset..offset + 4].copy_from_slice(&value.to_be_bytes());
        }
        let mut entries = vec![
            json!({"castID":1024,"fourCC":"CAS*","sectionID":3}),
            json!({"castID":4,"fourCC":"ediM","sectionID":6}),
        ];
        let mut chunks = vec![
            json!(["CASt", 4, cast.len()]),
            json!(["ediM", 6, mp3.len()]),
        ];
        if classic {
            entries.push(json!({"castID":4,"fourCC":"snd ","sectionID":5}));
            chunks.push(json!(["snd ", 5, 0]));
        }
        if paired {
            entries.extend([
                json!({"castID":4,"fourCC":"sndH","sectionID":7}),
                json!({"castID":4,"fourCC":"sndS","sectionID":8}),
            ]);
            chunks.extend([json!(["sndH", 7, 84]), json!(["sndS", 8, 8])]);
        }
        let header = json!({"json":[
            {"fourCC":"DRCF","id":1,"data":{"minMember":1,"maxMember":1,"frameRate":30}},
            {"fourCC":"KEY*","id":2,"data":{"usedCount":entries.len(),"entryCount":entries.len(),"entries":entries}},
            {"fourCC":"CAS*","id":3,"data":{"memberIDs":[4]}},
            {"fourCC":"CASt","id":4,"data":{"type":6,"info":{"name":"sound","flags":16}}}
        ],"scripts":{},"chunks":chunks});
        let json = serde_json::to_vec(&header).unwrap();
        let mut dump = b"D64DUMP1".to_vec();
        dump.extend((json.len() as u32).to_le_bytes());
        dump.extend(json);
        dump.extend(cast);
        dump.extend(mp3);
        if paired {
            dump.extend(pcm_header);
            dump.extend([0; 8]);
        }
        let mut fs = Memory::new();
        fs.mkdir_all("dumps").unwrap();
        fs.mkdir_all("out/wav").unwrap();
        let key = director64_aot::convert::source::sha256(plan::file_key("TEST.DCR").as_bytes());
        fs.write(&format!("dumps/{}.dump", &key[..24]), &dump)
            .unwrap();
        let source = json!({"name":"TEST.DXR","source_path":"TEST.DCR","director_version":850});
        let paths = serde_json::Map::new();
        let mut sources = Sources {
            media: "",
            dumps: "dumps",
            source_paths: &paths,
            palettes: HashMap::new(),
        };
        let policy = json!({});
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
        if valid || paired {
            assert!(result.problems.is_empty(), "{:?}", result.problems);
            let member = &result.movie["members"][0];
            if paired {
                assert!(member.get("sourceEncoding").is_none());
            } else {
                assert_eq!(member["sourceEncoding"], "MPEG3");
            }
            let frames = if paired { 4 } else { 3456 };
            assert_eq!(member["frames"], frames);
            assert_eq!(member["rate"], if paired { 22050 } else { 44100 });
            let asset = member["asset"].as_str().unwrap().replace(".wav64", ".wav");
            let wav = fs.read(&format!("out/wav/{asset}")).unwrap();
            assert_eq!(&wav[..4], b"RIFF");
            assert_eq!(wav.len(), 44 + frames as usize * 2);
            assert!(wav[44..].iter().all(|b| *b == 0));
        } else {
            assert_eq!(result.problems.len(), 1);
            assert!(result.movie["members"][0].get("asset").is_none());
        }
    }
}

#[test]
fn movie_retains_owner_cast_film_mapping_and_resolves_external_source_aliases() {
    for (present, resolved) in [(false, false), (true, false), (true, true)] {
        let path = b"C:\\Authoring\\Long Sounds.cst";
        let mut mapping = 4u32.to_be_bytes().to_vec();
        mapping.extend(3u16.to_be_bytes());
        for offset in [0u32, 4, 8, 12 + path.len() as u32] {
            mapping.extend(offset.to_be_bytes());
        }
        mapping.extend([0, 3, 0, 0, 0, 2, 0, 0, 0, 0, path.len() as u8]);
        mapping.extend(path);
        mapping.push(0);
        let mut entries = vec![json!({"castID":1024,"fourCC":"CAS*","sectionID":3})];
        if present {
            entries.push(json!({"castID":1024,"fourCC":"ccl ","sectionID":4}));
        }
        let header = json!({"json":[
            {"fourCC":"DRCF","id":1,"data":{"minMember":1,"maxMember":0,"frameRate":30}},
            {"fourCC":"KEY*","id":2,"data":{"usedCount":entries.len(),"entryCount":entries.len(),"entries":entries}},
            {"fourCC":"CAS*","id":3,"data":{"memberIDs":[]}}
        ],"scripts":{},"chunks":if present {json!([["ccl ",4,mapping.len()]])} else {json!([])}});
        let json = serde_json::to_vec(&header).unwrap();
        let mut dump = b"D64DUMP1".to_vec();
        dump.extend((json.len() as u32).to_le_bytes());
        dump.extend(json);
        if present {
            dump.extend(mapping);
        }
        let mut fs = Memory::new();
        fs.mkdir_all("dumps").unwrap();
        let key = director64_aot::convert::source::sha256(plan::file_key("TEST.DCR").as_bytes());
        fs.write(&format!("dumps/{}.dump", &key[..24]), &dump)
            .unwrap();
        let source = json!({"name":"TEST.DXR","source_path":"TEST.DCR","director_version":850});
        let paths = json!({"LONGSOUN.CXT":"DATA/LONGSOUN.CXT"});
        let mut sources = Sources {
            media: "",
            dumps: "dumps",
            source_paths: paths.as_object().unwrap(),
            palettes: HashMap::new(),
        };
        let policy = if resolved {
            json!({"source_path_aliases":{"Authoring/Long Sounds.cst":"DATA/LONGSOUN.CXT"}})
        } else {
            json!({})
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
        let cast = &result.movie["casts"][0];
        if present {
            let target = if resolved {
                json!({"file":"LONGSOUN.CXT","cast":1})
            } else {
                json!({"path":"C:\\Authoring\\Long Sounds.cst","cast":0})
            };
            assert_eq!(cast["filmCastMap"], json!([3, 2, target]));
            assert_eq!(
                cast["filmCastMapSource"][2]["path"],
                "C:\\Authoring\\Long Sounds.cst"
            );
        } else {
            assert!(cast.get("filmCastMap").is_none());
        }
    }
}

#[test]
fn palette_cast_colors_and_archive_pattern_catalog_are_preserved() {
    let mut clut = Vec::new();
    for index in 0..256u16 {
        clut.extend([index as u8, 0, 0x34, 0, 0x56, 0]);
    }
    let mut tiles = vec![0u8; 128];
    be16(&mut tiles, 4, 2);
    be16(&mut tiles, 6, 9);
    be16(&mut tiles, 12, 8);
    be16(&mut tiles, 14, 8);
    let mut cast = vec![0; 12];
    be32(&mut cast, 0, 4);
    let header = json!({"json":[
        {"fourCC":"DRCF","id":1,"data":{"minMember":1,"maxMember":1,"frameRate":30}},
        {"fourCC":"MCsL","id":8,"data":{"entries":[{"id":66560,"name":"Internal","filePath":"","minMember":1,"maxMember":1}]}},
        {"fourCC":"KEY*","id":2,"data":{"usedCount":3,"entryCount":3,"entries":[
            {"castID":66560,"fourCC":"CAS*","sectionID":3},
            {"castID":4,"fourCC":"CLUT","sectionID":5},
            {"castID":1024,"fourCC":"VWTL","sectionID":6}]}},
        {"fourCC":"CAS*","id":3,"data":{"memberIDs":[4]}},
        {"fourCC":"CASt","id":4,"data":{"type":4,"info":{"name":"Colors"}}}
    ],"scripts":{},"chunks":[["CASt",4,cast.len()],["CLUT",5,clut.len()],["VWTL",6,tiles.len()]]});
    let bytes = serde_json::to_vec(&header).unwrap();
    let mut dump = b"D64DUMP1".to_vec();
    dump.extend((bytes.len() as u32).to_le_bytes());
    dump.extend(bytes);
    dump.extend(cast);
    dump.extend(clut);
    dump.extend(tiles);
    let mut fs = Memory::new();
    fs.mkdir_all("dumps").unwrap();
    let key = director64_aot::convert::source::sha256(plan::file_key("TEST.DCR").as_bytes());
    fs.write(&format!("dumps/{}.dump", &key[..24]), &dump)
        .unwrap();
    let source = json!({"name":"TEST.DXR","source_path":"TEST.DCR","director_version":850});
    let policy = json!({});
    let paths = serde_json::Map::new();
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
    assert!(result.problems.is_empty(), "{:?}", result.problems);
    assert_eq!(result.movie["members"][0]["palette"][255], 0xff3456);
    assert_eq!(
        result.movie["casts"][0]["patternTiles"][0],
        json!({"cast":2,"member":9,"rect":[0,0,8,8]})
    );
    assert_eq!(
        result.movie["casts"][0]["patternTiles"][1],
        serde_json::Value::Null
    );
}

#[test]
fn embedded_image_geometry_replaces_the_stale_member_rectangle() {
    // A synthetic 2x1 PNG inside a cast whose authoring rectangle says 1x1.
    let png = vec![137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,1,8,6,0,0,0,244,34,127,138,0,0,0,14,73,68,65,84,120,156,99,248,207,192,240,31,4,1,16,248,3,253,78,149,193,111,0,0,0,0,73,69,78,68,174,66,96,130];
    let mut cast=vec![0u8;40];be32(&mut cast,0,1);be32(&mut cast,8,28);
    be16(&mut cast,12,0x8004);be16(&mut cast,18,1);be16(&mut cast,20,1);cast[35]=32;
    let header=json!({"json":[
        {"fourCC":"DRCF","id":1,"data":{"minMember":1,"maxMember":1,"frameRate":30}},
        {"fourCC":"KEY*","id":2,"data":{"usedCount":2,"entryCount":2,"entries":[
            {"castID":1024,"fourCC":"CAS*","sectionID":3},
            {"castID":4,"fourCC":"ediM","sectionID":6}]}},
        {"fourCC":"CAS*","id":3,"data":{"memberIDs":[4]}},
        {"fourCC":"CASt","id":4,"data":{"type":1,"info":{"name":"pixel"}}}
    ],"scripts":{},"chunks":[["CASt",4,cast.len()],["ediM",6,png.len()]]});
    let json=serde_json::to_vec(&header).unwrap();let mut dump=b"D64DUMP1".to_vec();
    dump.extend((json.len() as u32).to_le_bytes());dump.extend(json);dump.extend(cast);dump.extend(png);
    let mut fs=Memory::new();fs.mkdir_all("dumps").unwrap();fs.mkdir_all("out/images").unwrap();
    let key=director64_aot::convert::source::sha256(plan::file_key("TEST.DCR").as_bytes());
    fs.write(&format!("dumps/{}.dump",&key[..24]),&dump).unwrap();
    let source=json!({"name":"TEST.DXR","source_path":"TEST.DCR","director_version":850});
    let paths=serde_json::Map::new();let policy=json!({});
    let mut sources=Sources{media:"",dumps:"dumps",source_paths:&paths,palettes:HashMap::new()};
    let job=MovieJob{source:&source,movie_id:1,policy:&policy,recovery:"",output:"out",
        common_palette:None,common_palette_hash:None,d5_stage_palette:None};
    let result=convert_movie(&mut fs,&mut sources,&job).unwrap();
    assert!(result.problems.is_empty(),"{:?}",result.problems);
    let member=&result.movie["members"][0];
    assert_eq!((member["width"].as_u64(),member["height"].as_u64()),(Some(2),Some(1)));
    let bytes=fs.read(&format!("out/images/{}",member["asset"].as_str().unwrap())).unwrap();
    assert_eq!(&bytes[8..12],&[0,2,0,1]);
}

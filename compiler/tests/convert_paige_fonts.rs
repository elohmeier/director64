//! Paige styles, font binding, text reachability and Fmap (was
//! tests/node/paige-fonts.test.mjs).

mod common;

use common::*;
use director64_aot::convert::d5media::font_map;
use director64_aot::convert::paige::{paige_styles, Reader, Records};
use director64_aot::convert::text::{bind_text_fonts, text_reachability};
use serde_json::{json, Value};

fn num(n: i64) -> Vec<u8> {
    latin1(&format!("\x02{}{:X}", if n < 0 { "-" } else { "" }, n.abs()))
}
fn numbers(ns: &[i64]) -> Vec<u8> {
    ns.iter().flat_map(|&n| num(n)).collect()
}
fn name(text: &str) -> Vec<u8> {
    let mut b = vec![0u8; 64];
    b[0] = text.len() as u8;
    put(&mut b, 1, &latin1(text));
    concat(&[&latin1("\x0040,"), &b])
}
fn fixture() -> Records {
    let zeros = numbers(&[0; 17]);
    let font = concat(&[&name("Geneva"), &name(""), &zeros, &name("Pettson *"), &name(""), &zeros]);
    let (mut a, mut b) = (vec![0i64; 77], vec![0i64; 77]);
    a[18] = 12 * 65536;
    b[0] = 1;
    b[3] = 14;
    b[4] = 7;
    b[18] = 15 * 65536;
    b[9] = 0x4c00;
    b[10] = 0x0a00;
    let mut par = vec![0i64; 54];
    par[0] = 1;
    par[30] = 24;
    let styles: Vec<i64> = [vec![1], a, b].concat();
    Records::from([
        (0, (num(0x40001), 1)),
        (8, (font, 2)),
        (6, (numbers(&styles), 2)),
        (7, (numbers(&par), 1)),
        (4, (numbers(&[0, 1, 4, 1]), 2)),
        (5, (numbers(&[0, 0, 4, 0]), 2)),
    ])
}

#[test]
fn repeat_decoding_retains_signed_values_and_numeric_type_transitions() {
    let bytes = concat(&[&num(-19), &[0x81, 0xc2, 3], &num(0xffff)]);
    let mut r = Reader::new(&bytes);
    assert_eq!(r.numbers(6).unwrap(), [-19, -19, -19, -19, -19, 65535]);
    r.end().unwrap();
    for bad in [&[0x81][..], &[0xc2, 2], &[2], &[2, 49, 0xc1, 0]] {
        let mut reader = Reader::new(bad);
        let result = (|| {
            while reader.position < reader.bytes.len() {
                reader.number()?;
            }
            reader.end()
        })();
        assert!(result.is_err(), "{bad:?}");
    }
}

#[test]
fn font_indices_select_authored_face_size_color_and_insertion_metrics() {
    let p = paige_styles(&fixture(), 2).unwrap();
    let s = &p["styles"][p["insertStyle"].as_u64().unwrap() as usize];
    assert_eq!(s["font"], "Pettson *");
    assert_eq!((s["pointSize"].clone(), s["ascent"].clone(), s["descent"].clone()), (15.into(), 14.into(), 7.into()));
    assert_eq!(s["foreground"], json!([0x4c00, 0xa00, 0, 0]));
    assert_eq!(p["paragraphs"][0]["justification"], 1);
    assert_eq!(p["styleRuns"], json!([{"offset": 0, "index": 1}, {"offset": 4, "index": 1}]));
    assert_eq!(p["styles"][0]["face"], json!({}));
}

#[test]
fn record_truncations_fail_instead_of_guessing_font_names_or_metrics() {
    for kind in [4, 5, 6, 7, 8] {
        let bytes = fixture()[&kind].0.clone();
        for n in 0..bytes.len() {
            let mut f = fixture();
            f.get_mut(&kind).unwrap().0 = bytes[..n].to_vec();
            assert!(paige_styles(&f, 2).is_err(), "section {kind} truncated at {n}");
        }
    }
}

#[test]
fn run_sentinels_are_bounded_and_invalid_font_references_fail() {
    let mut f = fixture();
    f.get_mut(&4).unwrap().0 = numbers(&[0, 1, 5, 1]);
    assert!(paige_styles(&f, 2).unwrap_err().contains("style run"));
    let mut g = fixture();
    g.get_mut(&6).unwrap().0 = numbers(&[[3].as_slice(), &[0; 154]].concat());
    assert!(paige_styles(&g, 2).unwrap_err().contains("insertion style"));
    let mut h = fixture();
    h.get_mut(&8).unwrap().1 = 4097;
    assert!(paige_styles(&h, 2).unwrap_err().contains("excessive"));
}

#[test]
fn original_font_binding_keeps_initial_and_insertion_sizes_distinct_and_records_reachable_evidence() {
    let mut t = paige_styles(&fixture(), 2).unwrap();
    t["styles"][0]["font"] = json!("Pettson *");
    t["styleRuns"][0]["index"] = json!(0);
    let mut bytes = vec![0u8; 48];
    bytes[0] = 16;
    be16(&mut bytes, 4, 1);
    be16(&mut bytes, 6, 7);
    let mut movies = vec![json!({"name": "LO.DXR",
        "members": [{"cast": 1, "number": 7, "name": "namn", "text": "Ab", "typography": t}],
        "casts": [{"number": 1, "file": "LO.DXR"}],
        "score": {"frames": [{"number": 3, "channels": [{"channel": 8, "bytes": bytes}]}]}})];
    let mut fonts = vec![json!({"number": 1, "aliases": ["Pettson", "Pettson *"]})];
    assert_eq!(bind_text_fonts(&mut movies, &mut fonts, false).unwrap(), Vec::<Value>::new());
    let member = &movies[0]["members"][0];
    assert_eq!(member["textStyle"]["size"], 12);
    assert_eq!(member["textInsertStyle"]["size"], 15);
    assert_eq!(member["textInsertStyle"]["color"], 0x4c0a00);
    assert_eq!(fonts[0]["variants"], json!([{"size": 12, "asset": "fonts/f1-12.font64"}, {"size": 15, "asset": "fonts/f1-15.font64"}]));
    let scripts = [("LO.DXR.lingo".to_string(), "member(\"namn\").text = \"Ab\"".to_string())];
    let a = text_reachability(&movies, &scripts);
    assert_eq!(a["computedReferencesProvenUnreachable"], false);
    assert_eq!(a["members"][0]["evidence"], "referenced");
    assert_eq!(a["members"][0]["scoreReferences"][0]["channel"], 8);
    assert_eq!(a["members"][0]["literalNameReferences"], json!(["LO.DXR.lingo"]));
}

#[test]
fn missing_source_fonts_remain_explicit_rather_than_aliasing_to_a_similar_embedded_font() {
    let typography = paige_styles(&fixture(), 2).unwrap();
    let mut movies = vec![json!({"name": "NOTES.CXT",
        "members": [{"cast": 1, "number": 1, "name": "notes", "text": "Ab", "typography": typography}]})];
    let mut fonts = vec![json!({"number": 1, "aliases": ["Different Font"]})];
    let limits = bind_text_fonts(&mut movies, &mut fonts, false).unwrap();
    let style = &movies[0]["members"][0]["textStyle"];
    assert_eq!(style["fontId"], 0);
    assert_eq!(style["fontName"], "Pettson *");
    assert_eq!(limits[0]["kind"], "unavailable-source-font");
    assert_eq!(fonts[0]["variants"], json!([]));
}

fn fmap_fixture(entries: &[(i64, i64, &str)]) -> Vec<u8> {
    let (mut names, mut offsets, mut total) = (Vec::new(), Vec::new(), 0);
    for &(_, _, name) in entries {
        offsets.push(total);
        let mut b = vec![0u8; 4];
        be32(&mut b, 0, name.len() as i64);
        b.extend(latin1(name));
        total += b.len() as i64;
        names.extend(b);
    }
    let mut body = vec![0u8; 28 + entries.len() * 8];
    be32(&mut body, 8, entries.len() as i64);
    for (i, &(id, platform, _)) in entries.iter().enumerate() {
        be32(&mut body, 28 + i * 8, offsets[i]);
        be16(&mut body, 32 + i * 8, platform);
        be16(&mut body, 34 + i * 8, id);
    }
    let mut head = vec![0u8; 8];
    be32(&mut head, 0, body.len() as i64);
    be32(&mut head, 4, names.len() as i64);
    concat(&[&head, &body, &names])
}

#[test]
fn fmap_decodes_cast_font_ids_prefers_windows_on_duplicates_and_rejects_truncation() {
    let bytes = fmap_fixture(&[(32769, 2, "Arial"), (32770, 1, "Chicago"), (32770, 2, "GrundSchulGroteskBQ-RNew *")]);
    assert_eq!(Value::Object(font_map(&bytes).unwrap()), json!({"32769": "Arial", "32770": "GrundSchulGroteskBQ-RNew *"}));
    for n in 0..bytes.len() {
        assert!(font_map(&bytes[..n]).is_err(), "truncated at {n}");
    }
}

#[test]
fn field_styles_bind_through_the_cast_font_map_only_when_the_game_policy_opts_in() {
    let make = || vec![json!({"name": "LOGIN.DXR",
        "casts": [{"number": 1, "file": "LOGIN.DXR", "fontMap": {"32770": "Pettson *", "32771": "Arial"}}],
        "members": [
            {"cast": 1, "number": 5, "name": "Line current", "text": "b", "textAlign": 1,
                "sourceTextStyles": [{"offset": 0, "lineHeight": 0, "ascent": 0, "sourceFontId": 32770, "face": 0, "size": 24, "color": 0}]},
            {"cast": 1, "number": 6, "name": "system", "text": "x", "textAlign": -1,
                "sourceTextStyles": [{"offset": 0, "lineHeight": 16, "ascent": 13, "sourceFontId": 32771, "face": 0, "size": 12, "color": 0xff0000}]},
            {"cast": 1, "number": 7, "name": "unmapped", "text": "y", "textAlign": 0,
                "sourceTextStyles": [{"offset": 0, "lineHeight": 0, "ascent": 0, "sourceFontId": 99, "face": 1, "size": 10, "color": 0}]}]})];
    let mut fonts = vec![json!({"number": 1, "aliases": ["Pettson", "Pettson *"]})];
    let mut off = make();
    assert_eq!(bind_text_fonts(&mut off, &mut fonts, false).unwrap(), Vec::<Value>::new());
    assert!(off[0]["members"][0].get("textStyle").is_none());
    assert_eq!(fonts[0]["variants"], json!([]));
    let mut on = make();
    let limits = bind_text_fonts(&mut on, &mut fonts, true).unwrap();
    let bound = &on[0]["members"][0];
    assert_eq!(bound["textStyle"], json!({"fontName": "Pettson *", "fontId": 1, "size": 24, "align": 1,
        "ascent": 0, "descent": 0, "leading": 0, "lineHeight": 0, "color": 0}));
    assert_eq!(bound["textInsertStyle"], bound["textStyle"]);
    assert_eq!(bound["textPresentation"], "recovered-original-font");
    assert_eq!(fonts[0]["variants"], json!([{"size": 24, "asset": "fonts/f1-24.font64"}]));
    assert!(on[0]["members"][1].get("textStyle").is_none());
    assert!(on[0]["members"][2].get("textStyle").is_none());
    let found: Vec<(Value, Value)> = limits.iter().map(|l| (l["kind"].clone(), l["fonts"][0].clone())).collect();
    assert_eq!(found, [(json!("unavailable-source-font"), json!("Arial")), (json!("unavailable-source-font"), json!("font id 99"))]);
}

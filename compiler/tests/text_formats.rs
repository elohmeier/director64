use director64_aot::convert::text::bind_text_fonts;
use serde_json::{Value, json};
#[test]
fn paige_zero_point_selects_twelve_without_rewriting_source_style() {
    let mut movies = vec![json!({"name":"ZERO.DXR","directorVersion":1000,
      "members":[{"type":7,"cast":1,"number":1,"text":"AB","typography":{
       "styles":[{"font":"Alpha","pointSize":0,"ascent":12,"descent":3,
         "leading":0,"foreground":[0,0,0],"face":{}}],
       "styleRuns":[{"offset":0,"index":0}],"paragraphRuns":[{"offset":0,"index":0}],
       "paragraphs":[{"justification":0}],"insertStyle":0}}]})];
    let mut fonts = vec![json!({"number":1,"aliases":["Alpha"]})];
    assert!(
        bind_text_fonts(&mut movies, &mut fonts, false)
            .unwrap()
            .is_empty()
    );
    let member = &movies[0]["members"][0];
    assert_eq!(member["textStyle"]["size"], 12);
    assert_eq!(member["textInsertStyle"]["size"], 12);
    assert_eq!(member["textFormats"][0]["size"], 0);
    assert_eq!(member["typography"]["styles"][0]["pointSize"], 0);
    assert_eq!(fonts[0]["variants"][0]["size"], 12);
}
fn source(offset: u32, font: u32, size: u32, face: u32) -> Value {
    json!({"offset":offset,"sourceFontId":font,"size":size,"face":face,
        "ascent":size-2,"lineHeight":size+3,"color":0x123456})
}
#[test]
fn every_stxt_run_binds_in_scope_and_outline_stays_an_explicit_limit() {
    let mut movies = vec![json!({"name":"TEXT.DXR","directorVersion":600,
  "casts":[{"file":"TEXT.DXR","fontMap":{"1":"Alpha","2":"Beta"}}],
  "members":[{"type":3,"cast":1,"number":1,"text":"AB","textAlign":-1,
   "sourceTextStyles":[source(0,1,12,1),source(1,2,18,10)]}]})];
    let mut fonts = vec![
        json!({"number":1,"aliases":["Alpha"]}),
        json!({"number":2,"aliases":["Beta"]}),
    ];
    let limits = bind_text_fonts(&mut movies, &mut fonts, true).unwrap();
    let runs = &movies[0]["members"][0]["textFormats"];
    assert_eq!(runs[0]["fontId"], 1);
    assert_eq!(runs[0]["face"], 1);
    assert_eq!(runs[1]["fontId"], 2);
    assert_eq!(runs[1]["size"], 18);
    assert_eq!(runs[1]["face"], 2);
    assert_eq!(runs[1]["align"], 2);
    assert_eq!(fonts[1]["variants"][0]["size"], 18);
    assert_eq!(limits.len(), 1);
    assert_eq!(limits[0]["detail"], "unsupported character face effects");
}
#[test]
fn paige_merges_paragraph_boundaries_and_preserves_insertion_face_and_all_sizes() {
    let style = |name: &str, size: u32, face: Value| {
        json!({"font":name,"pointSize":size,
  "ascent":size-2,"descent":2,"leading":0,"foreground":[0x1200,0x3400,0x5600],"face":face})
    };
    let mut movies = vec![json!({"name":"TEXT.DXR","directorVersion":850,
  "members":[{"type":7,"cast":1,"number":1,"text":"AB😀","typography":{
   "styles":[style("Alpha",12,json!({"bold":true})),style("Beta",18,json!({"italic":true}))],
   "styleRuns":[{"offset":0,"index":0},{"offset":2,"index":1}],
   "paragraphs":[{"justification":0},{"justification":2,"leadingFixed":28}],
   "paragraphRuns":[{"offset":0,"index":0},{"offset":1,"index":1}],"insertStyle":0}}]})];
    let mut fonts = vec![
        json!({"number":1,"aliases":["Alpha"]}),
        json!({"number":2,"aliases":["Beta"]}),
    ];
    assert!(
        bind_text_fonts(&mut movies, &mut fonts, false)
            .unwrap()
            .is_empty()
    );
    let runs = movies[0]["members"][0]["textFormats"].as_array().unwrap();
    assert_eq!(
        runs.iter()
            .map(|r| r["offset"].as_u64().unwrap())
            .collect::<Vec<_>>(),
        [0, 1, 2, 4]
    );
    assert_eq!(runs[1]["align"], 2);
    assert_eq!(runs[1]["lineHeight"], 28);
    assert_eq!(runs[2]["size"], 18);
    assert_eq!(runs[2]["face"], 2);
    assert_eq!(runs[3]["size"], 12);
    assert_eq!(runs[3]["face"], 1);
    assert_eq!(fonts[1]["variants"][0]["size"], 18);
}
#[test]
fn later_missing_fonts_are_reported_and_d5_keeps_its_existing_contract() {
    let movie = |version: u32| {
        json!({"name":"TEXT.DXR","directorVersion":version,
  "casts":[{"fontMap":{"1":"Alpha"}}],"members":[{"type":3,"cast":1,"number":1,
   "text":"AB","sourceTextStyles":[source(0,1,12,0),source(1,99,18,1)]}]})
    };
    let mut movies = vec![movie(600), movie(500)];
    let mut fonts = vec![json!({"number":1,"aliases":["Alpha"]})];
    let limits = bind_text_fonts(&mut movies, &mut fonts, true).unwrap();
    assert_eq!(movies[0]["members"][0]["textFormats"][1]["fontId"], 0);
    assert!(limits.iter().any(
        |limit| limit["kind"] == "unavailable-source-font" && limit["fonts"][0] == "font id 99"
    ));
    assert!(movies[1]["members"][0].get("textFormats").is_none());
}

#[test]
fn paige_full_justification_is_a_supported_paragraph_style() {
    let mut movies = vec![json!({"name":"TEXT.DXR","directorVersion":850,"members":[
      {"type":7,"cast":1,"number":1,"text":"AA BB CC","typography":{
       "styles":[{"font":"Alpha","pointSize":12,"ascent":10,"descent":3,"leading":0,
         "foreground":[0,0,0],"face":{}}],"styleRuns":[{"offset":0,"index":0}],
       "paragraphs":[{"justification":3}],"paragraphRuns":[{"offset":0,"index":0}],"insertStyle":0}}]})];
    let mut fonts = vec![json!({"number":1,"aliases":["Alpha"]})];
    assert!(
        bind_text_fonts(&mut movies, &mut fonts, false)
            .unwrap()
            .is_empty()
    );
    assert_eq!(movies[0]["members"][0]["textFormats"][0]["align"], 3);
}

#[test]
fn repeated_stxt_boundaries_use_the_last_authored_style() {
    let mut movies = vec![json!({"name":"TEXT.DXR","directorVersion":600,
  "casts":[{"fontMap":{"1":"Alpha"}}],"members":[{"type":3,"cast":1,"number":1,
   "text":"AB","sourceTextStyles":[source(0,1,12,0),source(0,1,18,1),source(1,1,18,1)]}]})];
    let mut fonts = vec![json!({"number":1,"aliases":["Alpha"]})];
    bind_text_fonts(&mut movies, &mut fonts, true).unwrap();
    let runs = movies[0]["members"][0]["textFormats"].as_array().unwrap();
    assert_eq!(runs.len(), 1);
    assert_eq!(runs[0]["offset"], 0);
    assert_eq!(runs[0]["size"], 18);
}

#[test]
fn paige_character_extra_is_integral_fixed_pixels_and_keeps_spacing_only_boundaries() {
    let style = |extra: Value| {
        json!({"font":"Alpha","pointSize":12,"ascent":10,
        "descent":3,"leading":0,"foreground":[0,0,0],"face":{},"characterExtra":extra})
    };
    let movie = |last: Value| {
        json!({"name":"SPACING.DXR","directorVersion":850,"members":[
        {"type":7,"cast":2,"number":17,"text":"A😀B","typography":{
          "styles":[style(json!(65536)),style(last)],
          "styleRuns":[{"offset":0,"index":0},{"offset":3,"index":1}],
          "paragraphs":[{"justification":0}],"paragraphRuns":[{"offset":0,"index":0}],
          "insertStyle":1}}]})
    };
    let mut movies = vec![movie(json!(-65536000))];
    let mut fonts = vec![json!({"number":1,"aliases":["Alpha"]})];
    bind_text_fonts(&mut movies, &mut fonts, false).unwrap();
    let runs = movies[0]["members"][0]["textFormats"].as_array().unwrap();
    assert_eq!(runs.len(), 2);
    assert_eq!(runs[0]["charSpacing"], 1);
    assert_eq!(runs[1]["offset"], 3);
    assert_eq!(runs[1]["charSpacing"], -1000);
    for invalid in [
        json!(32768),
        json!(2147483648i64),
        json!(-2147549184i64),
        json!(65536.5),
        json!("65536"),
    ] {
        let mut movies = vec![movie(invalid)];
        let error = bind_text_fonts(&mut movies, &mut fonts, false).unwrap_err();
        assert!(
            error.contains("SPACING.DXR")
                && error.contains("cast 2 member 17 UTF16 offset 3")
                && error.contains("characterExtra"),
            "{error}"
        );
    }
}

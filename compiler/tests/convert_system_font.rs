use director64_aot::convert::{
    files::{Files, Memory},
    source::sha256,
    system_font,
};
use serde_json::json;
fn word(b: &mut [u8], p: usize, n: u16) {
    b[p..p + 2].copy_from_slice(&n.to_be_bytes());
}
fn long(b: &mut [u8], p: usize, n: u32) {
    b[p..p + 4].copy_from_slice(&n.to_be_bytes());
}
fn synthetic(format: u16) -> Vec<u8> {
    let mut head = vec![0; 54];
    long(&mut head, 12, 0x5f0f3cf5);
    word(&mut head, 18, 2048);
    let mut hhea = vec![0; 36];
    word(&mut hhea, 4, 1800);
    word(&mut hhea, 6, (-400i16) as u16);
    let mut maxp = vec![0; 6];
    word(&mut maxp, 4, 4);
    let text: Vec<u8> = "Synthetic Exact"
        .encode_utf16()
        .flat_map(u16::to_be_bytes)
        .collect();
    let mut name = vec![0; 18];
    word(&mut name, 2, 1);
    word(&mut name, 4, 18);
    word(&mut name, 6, 3);
    word(&mut name, 8, 1);
    word(&mut name, 10, 0x409);
    word(&mut name, 12, 1);
    word(&mut name, 14, text.len() as u16);
    name.extend(text);
    let map = if format == 12 {
        let mut map = vec![0; 28];
        word(&mut map, 0, 12);
        long(&mut map, 4, 28);
        long(&mut map, 12, 1);
        long(&mut map, 16, 65);
        long(&mut map, 20, 67);
        long(&mut map, 24, 1);
        map
    } else {
        let mut map = vec![0; 32];
        word(&mut map, 0, 4);
        word(&mut map, 2, 32);
        word(&mut map, 6, 4);
        word(&mut map, 14, 67);
        word(&mut map, 16, 65535);
        word(&mut map, 20, 65);
        word(&mut map, 22, 65535);
        word(&mut map, 24, (-64i16) as u16);
        word(&mut map, 26, 1);
        map
    };
    let mut cmap = vec![0; 12];
    word(&mut cmap, 2, 1);
    word(&mut cmap, 4, 3);
    word(&mut cmap, 6, if format == 12 { 10 } else { 1 });
    long(&mut cmap, 8, 12);
    cmap.extend(map);
    let tables = [
        (*b"head", head),
        (*b"hhea", hhea),
        (*b"maxp", maxp),
        (*b"name", name),
        (*b"cmap", cmap),
    ];
    let mut out = vec![0; 12 + tables.len() * 16];
    long(&mut out, 0, 0x10000);
    word(&mut out, 4, tables.len() as u16);
    for (n, (name, table)) in tables.into_iter().enumerate() {
        let p = 12 + n * 16;
        out[p..p + 4].copy_from_slice(&name);
        let offset = out.len();
        long(&mut out, p + 8, offset as u32);
        long(&mut out, p + 12, table.len() as u32);
        out.extend(table);
    }
    out
}
#[test]
fn exact_original_sfnt_identity_metrics_and_unicode_coverage_are_preserved() {
    for format in [4, 12] {
        let raw = synthetic(format);
        let aliases = vec!["Synthetic Exact".into()];
        let m = system_font::metadata(&raw, &aliases).unwrap();
        assert_eq!(m["unitsPerEm"], 2048);
        assert_eq!(m["ascent"], 1800);
        assert_eq!(m["descent"], -400);
        assert_eq!(m["codepoints"], json!([65, 66, 67]));
        assert_eq!(m["sha256"], sha256(&raw));
        assert!(system_font::metadata(&raw, &["Similar Font".into()]).is_err());
        for size in 0..raw.len() {
            assert!(
                system_font::metadata(&raw[..size], &aliases).is_err(),
                "truncated at {size}"
            );
        }
    }
}
#[test]
fn source_resources_are_hash_pinned_and_output_is_byte_identical() {
    let raw = synthetic(12);
    let mut fs = Memory::new();
    fs.mkdir_all("/media/fonts").unwrap();
    fs.write("/media/fonts/original.ttf", &raw).unwrap();
    let mut policy = json!({"system_font_resources":[{"path":"fonts/original.ttf","sha256":sha256(&raw),"aliases":["Synthetic Exact"]}]});
    let mut fonts = Vec::new();
    system_font::bind(&mut fs, "/media", "/output", &policy, &mut fonts).unwrap();
    assert_eq!(fonts.len(), 1);
    assert_eq!(fonts[0]["number"], 1);
    assert_eq!(
        fs.read(&format!("/output/{}", fonts[0]["asset"].as_str().unwrap()))
            .unwrap(),
        raw
    );
    system_font::bind(&mut fs, "/media", "/output", &policy, &mut fonts).unwrap();
    assert_eq!(fonts.len(), 1);
    policy["system_font_resources"][0]["sha256"] = json!("incorrect");
    assert!(
        system_font::bind(&mut fs, "/media", "/output", &policy, &mut fonts)
            .unwrap_err()
            .contains("hash mismatch")
    );
}
#[test]
fn original_installer_entry_is_extracted_and_both_hashes_are_verified() {
    use std::io::{Cursor, Write};
    let raw = synthetic(4);
    let mut builder = cab::CabinetBuilder::new();
    builder
        .add_folder(cab::CompressionType::MsZip)
        .add_file("Synthetic.ttf");
    let mut writer = builder.build(Cursor::new(Vec::new())).unwrap();
    writer
        .next_file()
        .unwrap()
        .unwrap()
        .write_all(&raw)
        .unwrap();
    assert!(writer.next_file().unwrap().is_none());
    let mut archive = b"MZ data-only installer prefix".to_vec();
    archive.extend(writer.finish().unwrap().into_inner());
    let mut fs = Memory::new();
    fs.mkdir_all("/media/fonts").unwrap();
    fs.write("/media/fonts/original.exe", &archive).unwrap();
    let mut policy = json!({"system_font_resources":[{"path":"fonts/original.exe","sha256":sha256(&archive),
        "entry":"Synthetic.ttf","font_sha256":sha256(&raw),"aliases":["Synthetic Exact"]}]});
    let mut fonts = Vec::new();
    system_font::bind(&mut fs, "/media", "/output", &policy, &mut fonts).unwrap();
    assert_eq!(
        fs.read(&format!("/output/{}", fonts[0]["asset"].as_str().unwrap()))
            .unwrap(),
        raw
    );
    assert_eq!(fonts[0]["sources"][0]["sha256"], sha256(&archive));
    assert_eq!(fonts[0]["sources"][0]["entry"], "Synthetic.ttf");
    policy["system_font_resources"][0]["font_sha256"] = json!("wrong");
    assert!(
        system_font::bind(&mut fs, "/media", "/output", &policy, &mut Vec::new())
            .unwrap_err()
            .contains("extracted system font hash mismatch")
    );
}

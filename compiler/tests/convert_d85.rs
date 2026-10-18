//! Director 8.5: text Xtras, damaged score indexes and shared script
//! contexts as the Findus and Löwenzahn 8.5 discs store them.

mod common;

use common::*;
use director64_aot::convert::score::decode;
use director64_aot::convert::source::script_casts;
use director64_aot::convert::xtra::text_xtra_model;
use serde_json::{Value, json};

fn xmed(records: &[(u32, Vec<u8>)], suffix: &str) -> Vec<u8> {
    let mut out = latin1("FFFF0000000600040001\x0177AA");
    for (id, body) in records {
        out.extend(latin1(&format!(
            "\x03{id:04X}{:08X}00000000",
            body.len() + 1
        )));
        out.extend_from_slice(body);
    }
    out.extend(latin1(&format!("\x03{suffix}")));
    out
}
fn payload(length: usize) -> Vec<u8> {
    let mut b = vec![0u8; length];
    be32(&mut b, 36, 20);
    be32(&mut b, 40, 80);
    b
}
/// Paige's special-character table in one character set's codes.
fn table(curly_quote: u8) -> (u32, Vec<u8>) {
    (
        19,
        vec![0x01, curly_quote, 0x00, 0x00, 0x01, curly_quote + 1],
    )
}

#[test]
fn text_xtras_take_the_large_payload_and_their_tables_character_set() {
    let text = (2, latin1("\x005,K\u{fc}che"));
    let windows = xmed(
        &[(0, vec![0x81]), (1, vec![0x82]), text.clone(), table(0x93)],
        "",
    );
    let result = text_xtra_model(&payload(432), &windows, 850).unwrap();
    assert_eq!(
        (result["text"].clone(), result["textEncoding"].clone()),
        (json!("Küche"), json!("windows-1252"))
    );
    assert!(
        text_xtra_model(&payload(76), &windows, 850)
            .unwrap_err()
            .contains("text Xtra layout")
    );
    // A Mac-authored document in the same movie: ü is 0x9F in Mac Roman.
    let mac = xmed(
        &[
            (0, vec![0x81]),
            (1, vec![0x82]),
            (2, latin1("\x005,K\u{9f}che")),
            table(0xd2),
        ],
        "",
    );
    let result = text_xtra_model(&payload(432), &mac, 850).unwrap();
    assert_eq!(
        (result["text"].clone(), result["textEncoding"].clone()),
        (json!("Küche"), json!("macintosh"))
    );
}

#[test]
fn a_document_two_bytes_short_in_its_trailing_style_record_is_read() {
    let mut document = xmed(
        &[
            (0, vec![0x81]),
            (1, vec![0x82]),
            (2, latin1("\x002,Hi")),
            (297, latin1("\x022\u{82}\u{5}")),
        ],
        "",
    );
    document.truncate(document.len() - 2);
    let result = text_xtra_model(&payload(432), &document, 850).unwrap();
    assert_eq!(
        (result["text"].clone(), result["truncatedTail"].clone()),
        (json!("Hi"), json!(true))
    );
    // Any other record cut short still fails.
    let mut other = xmed(
        &[
            (0, vec![0x81]),
            (1, vec![0x82]),
            (2, latin1("\x002,Hi")),
            (296, latin1("\x022\u{82}\u{5}")),
        ],
        "",
    );
    other.truncate(other.len() - 2);
    assert!(
        text_xtra_model(&payload(432), &other, 850)
            .unwrap_err()
            .contains("record range")
    );
}

/// A D7-and-later score: an index of `offsets` over a header with one
/// empty frame, as entry 0.
fn score(offsets: &[u32]) -> Vec<u8> {
    let entries = offsets.len() as u32 - 1;
    let slots = entries + 1;
    let base = 24 + slots as usize * 4;
    let mut b = vec![0u8; base + 22];
    let size = b.len() as u32;
    for (at, value) in [
        (0, size),
        (4, 0xfffffffd),
        (8, 12),
        (12, entries),
        (16, slots),
        (20, 22),
    ] {
        b[at..at + 4].copy_from_slice(&u32::to_be_bytes(value));
    }
    for (i, offset) in offsets.iter().enumerate() {
        b[24 + i * 4..28 + i * 4].copy_from_slice(&offset.to_be_bytes());
    }
    let header = [
        &22u32.to_be_bytes()[..],
        &20u32.to_be_bytes(),
        &1u32.to_be_bytes(),
        &13u16.to_be_bytes(),
        &48u16.to_be_bytes(),
        &6u16.to_be_bytes(),
        &[0, 0],
        &[0, 2],
    ]
    .concat();
    b[base..].copy_from_slice(&header);
    b
}

#[test]
fn one_flipped_index_offset_between_agreeing_neighbours_is_repaired() {
    let intact: Value =
        serde_json::from_str(&decode("VWSC", &score(&[0, 22, 22, 22]), 850).unwrap()).unwrap();
    assert!(intact["fields"].get("repaired_index_entries").is_none());
    let flipped: Value =
        serde_json::from_str(&decode("VWSC", &score(&[0, 22, 22 + 0x2000000, 22]), 850).unwrap())
            .unwrap();
    assert_eq!(flipped["fields"]["repaired_index_entries"], 1);
    assert_eq!(flipped["frames"].as_array().unwrap().len(), 1);
    assert!(
        decode(
            "VWSC",
            &score(&[0, 22 + 0x2000000, 22 + 0x2000000, 22]),
            850
        )
        .unwrap_err()
        .contains("active offset")
    );
    assert!(
        decode("VWSC", &score(&[0, 22, 22, 22]), 900)
            .unwrap_err()
            .contains("8.5 and 10 only")
    );
}

#[test]
fn castlibs_sharing_a_library_id_share_one_script_context() {
    let scripts = json!({"casts": [
        {"name": "LZ_ADV", "scripts": [{"memberId": 1}]},
        {"name": "LZ_ADV", "scripts": [{"memberId": 1}]},
        {"name": "Neu", "scripts": [{"memberId": 2}]},
        {"name": "LZ_ADV", "scripts": [{"memberId": 1}]},
    ]});
    let entries = json!([{"name": "Internal", "id": 1024}, {"name": "LZ3", "id": 1024}, {"name": "Neu", "id": 66562},
        {"name": "LZ_ADV", "id": 1024}]);
    let names: Vec<String> = script_casts(&scripts, &entries)
        .into_iter()
        .map(|(name, _)| name)
        .collect();
    assert_eq!(names, ["Internal", "Neu"]);
    // Distinct ids keep the parser's names and every listing.
    let entries = json!([{"name": "Internal", "id": 1}, {"name": "LZ_ADV", "id": 2}, {"name": "Neu", "id": 3}]);
    assert_eq!(script_casts(&scripts, &entries).len(), 4);
}

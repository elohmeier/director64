//! D10 text Xtras and ALFA planes (was tests/node/director-d10-assets.test.mjs).

mod common;

use common::*;
use director64_aot::convert::bitmap::{verify_alfa_plane, Meta};
use director64_aot::convert::xtra::text_xtra_model;

/// D10 record envelopes reuse the D8 encoding; the count field carries the
/// continuation offset for split text records.
fn xmed(records: &[(u32, Vec<u8>, u32)], suffix: &str) -> Vec<u8> {
    let mut out = latin1("FFFF0000000600040001\x0177AA");
    for (id, body, count) in records {
        out.extend(latin1(&format!("\x03{id:04X}{:08X}{count:08X}", body.len() + 1)));
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
fn segment(text: &str) -> Vec<u8> {
    latin1(&format!("\x00{:X},{text}", text.len()))
}
fn empty(id: u32) -> (u32, Vec<u8>, u32) {
    (id, vec![], 0)
}

#[test]
fn text_xtra_uses_the_432_byte_payload_and_windows_1252_text() {
    let document = xmed(&[(0, vec![0x81], 0), (1, vec![0x82], 0), (2, latin1("\x005,K\u{fc}che"), 0)], "");
    let result = text_xtra_model(&payload(432), &document, 1000).unwrap();
    assert_eq!(result["text"], "Küche");
    assert_eq!(result["textEncoding"], "windows-1252");
    assert_eq!((result["width"].clone(), result["height"].clone()), (80.into(), 20.into()));
    assert!(text_xtra_model(&payload(76), &document, 1000).unwrap_err().contains("text Xtra layout"));
    assert!(text_xtra_model(&payload(432), &document, 800).unwrap_err().contains("text Xtra layout"));
}

#[test]
fn continuation_records_reassemble_long_text_in_authored_order() {
    let head = [(0, vec![0x81], 0), (1, vec![0x82], 0)];
    let document = xmed(&[head[0].clone(), head[1].clone(), (2, segment("Erster"), 0), (2, segment("Teil"), 6)], "");
    assert_eq!(text_xtra_model(&payload(432), &document, 1000).unwrap()["text"], "ErsterTeil");
    let reversed = xmed(&[head[0].clone(), head[1].clone(), (2, segment("Erster"), 4), (2, segment("Teil"), 0)], "");
    assert!(text_xtra_model(&payload(432), &reversed, 1000).unwrap_err().contains("segments out of order"));
    let d8 = xmed(&[empty(0), empty(1), (2, segment("a"), 0), (2, segment("b"), 1)], "FF");
    assert!(text_xtra_model(&payload(76), &d8, 800).unwrap_err().contains("record range"));
}

#[test]
fn fffe_terminator_and_extension_section_are_accepted_and_recorded() {
    let closed = xmed(&[empty(0), empty(1)], "FFFE");
    assert!(text_xtra_model(&payload(432), &closed, 1000).unwrap().get("mediaExtension").is_none());
    let mut extension = xmed(&[empty(0), empty(1), (2, segment("Satz"), 0)], "");
    extension.pop();
    extension.extend(latin1("\x03FFFE0000000600040001\x0177AA\x03TXcl"));
    extension.extend([0u8; 20]);
    let result = text_xtra_model(&payload(432), &extension, 1000).unwrap();
    assert_eq!(result["text"], "Satz");
    assert_eq!(result["mediaExtension"]["bytes"], 51);
    assert!(text_xtra_model(&payload(76), &extension, 800).unwrap_err().contains("record header"));
}

#[test]
fn alfa_planes_verify_against_bitmap_alpha_with_even_width_rows() {
    let meta = Meta {
        width: 3, height: 2, depth: 32, pitch: 0, palette: 0, palette_cast: 0, reg_x: 0, reg_y: 0,
        use_alpha: true, alpha_threshold: 0, update_flags: 0,
    };
    let alpha = [1, 2, 3, 4, 5, 6];
    verify_alfa_plane(&meta, &alpha, &[1, 2, 3, 0, 4, 5, 6, 0]).unwrap();
    assert!(verify_alfa_plane(&meta, &alpha, &[1, 2, 9, 0, 4, 5, 6, 0]).unwrap_err().contains("disagrees"));
    let error = verify_alfa_plane(&meta, &alpha, &[1, 2, 3, 4, 5, 6]).unwrap_err();
    assert!(error.contains("bounds") || error.contains("disagrees"), "{error}");
}

use director64_aot::convert::pict::decode;

fn word(out: &mut Vec<u8>, value: u16) {
    out.extend(value.to_be_bytes());
}
fn rect(out: &mut Vec<u8>, width: u16, height: u16) {
    for value in [0, 0, height, width] {
        word(out, value);
    }
}
fn picture() -> Vec<u8> {
    let mut out = vec![0, 1];
    rect(&mut out, 8, 3);
    out.extend([0, 17, 2, 255, 12, 0]);
    out.extend([0; 24]);
    word(&mut out, 0xa1);
    word(&mut out, 42);
    word(&mut out, 3);
    out.extend([1, 2, 3, 0]); // Ignored comment, aligned to a word.
    word(&mut out, 1);
    word(&mut out, 10);
    rect(&mut out, 8, 3);
    word(&mut out, 0x98);
    word(&mut out, 0x8008);
    rect(&mut out, 8, 3);
    word(&mut out, 0);
    word(&mut out, 0);
    out.extend([0; 12]);
    for value in [0, 8, 1, 8] {
        word(&mut out, value);
    }
    out.extend([0; 12]);
    out.extend([0; 4]);
    word(&mut out, 0);
    word(&mut out, 1);
    word(&mut out, 1);
    out.extend([0; 6]); // Explicit palette indices may be reordered.
    word(&mut out, 0);
    out.extend([255; 6]);
    rect(&mut out, 8, 3);
    rect(&mut out, 8, 3);
    word(&mut out, 0);
    for row in [
        [0, 1, 1, 1, 1, 1, 1, 0],
        [0, 1, 0, 0, 0, 0, 1, 0],
        [0, 1, 1, 1, 1, 1, 1, 0],
    ] {
        out.extend([10, 128, 7]); // A PackBits no-op plus eight literal indices.
        out.extend(row);
    }
    if out.len() % 2 != 0 {
        out.push(0);
    }
    word(&mut out, 0xff);
    out
}

#[test]
fn picture_palette_runs_and_exterior_matte_preserve_enclosed_white() {
    let picture = decode(&picture()).unwrap();
    assert_eq!((picture.width, picture.height), (8, 3));
    assert_eq!(&picture.pixels[..4], &[255, 255, 255, 0]);
    assert_eq!(&picture.pixels[4..8], &[0, 0, 0, 255]);
    assert_eq!(&picture.pixels[10 * 4..11 * 4], &[255; 4]);
}

#[test]
fn packed_repeat_runs_expand_to_exact_row_extent() {
    let mut bytes = picture();
    let first_row = bytes.len() - 36;
    bytes.splice(first_row..first_row + 11, [2, 249, 1]); // Eight palette-index1 pixels.
    let decoded = decode(&bytes).unwrap();
    assert!(
        decoded.pixels[..32]
            .chunks_exact(4)
            .all(|p| p == [0, 0, 0, 255])
    );
}

#[test]
fn unknown_drawing_commands_and_truncation_fail_without_missing_pixels() {
    let original = picture();
    for length in [0, 12, 43, original.len() - 1] {
        assert!(decode(&original[..length]).is_err());
    }
    let mut bad = original.clone();
    bad[40..42].copy_from_slice(&0x0031u16.to_be_bytes());
    assert!(
        decode(&bad)
            .unwrap_err()
            .contains("unsupported PICT opcode")
    );
    let mut trailing = original;
    trailing.push(0);
    assert!(decode(&trailing).is_err());
}

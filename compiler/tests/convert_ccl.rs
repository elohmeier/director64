use director64_aot::convert::ccl::{Reference, decode};

fn table(records: &[(i16, &[u8])]) -> Vec<u8> {
    let mut result = 4u32.to_be_bytes().to_vec();
    result.extend((records.len() as u16).to_be_bytes());
    let mut offset = 0u32;
    result.extend(offset.to_be_bytes());
    for (_, path) in records {
        offset += path.len() as u32 + 4;
        result.extend(offset.to_be_bytes());
    }
    for (cast, path) in records {
        result.extend(cast.to_be_bytes());
        result.push(path.len() as u8);
        result.extend(*path);
        result.push(0);
    }
    result
}

#[test]
fn film_cast_table_preserves_zero_index_external_paths_and_empty_table() {
    let bytes = table(&[
        (3, b""),
        (2, b""),
        (0, b"C:\\Authored\\Sound.cst"),
        (-1, b""),
        (1, b"Unknown-3"),
    ]);
    assert_eq!(
        decode(&bytes).unwrap(),
        vec![
            Reference {
                cast: 3,
                path: String::new()
            },
            Reference {
                cast: 2,
                path: String::new()
            },
            Reference {
                cast: 0,
                path: "C:\\Authored\\Sound.cst".into()
            },
            Reference {
                cast: -1,
                path: String::new()
            },
            Reference {
                cast: 1,
                path: "Unknown-3".into()
            },
        ]
    );
    assert!(decode(&table(&[])).unwrap().is_empty());
    assert_eq!(
        decode(&table(&[(0, b"Disk:L\x9awenzahn:Cursor.cst")])).unwrap()[0].path,
        "Disk:Löwenzahn:Cursor.cst"
    );
}

#[test]
fn film_cast_table_rejects_truncation_bad_offsets_and_invalid_strings() {
    let bytes = table(&[(3, b""), (0, b"Sound.cst")]);
    for length in 0..bytes.len() {
        assert!(
            decode(&bytes[..length]).is_err(),
            "accepted prefix {length}"
        );
    }
    for (at, value) in [
        (3, 8),
        (9, 1),
        (13, 255),
        (17, 0),
        (20, 9),
        (bytes.len() - 1, 1),
    ] {
        let mut corrupt = bytes.clone();
        corrupt[at] = value;
        assert!(decode(&corrupt).is_err(), "accepted mutation at {at}");
    }
    assert!(decode(&table(&[(0, b"Sound\0.cst")])).is_err());
}

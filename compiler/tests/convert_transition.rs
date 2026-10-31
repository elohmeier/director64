//! Synthetic descriptor retains its authored duration/chunk/type/area flags.
use director64_aot::convert::movie::transition_cast_model;
use serde_json::json;
#[test]
fn transition_source_record_retains_all_fields_and_checks_lengths() {
    let mut data = vec![0u8; 22];
    data[4..8].copy_from_slice(&4u32.to_be_bytes());
    data[8..12].copy_from_slice(&6u32.to_be_bytes());
    data[16..22].copy_from_slice(&[0, 12, 37, 2, 0x06, 0xd6]);
    assert_eq!(
        transition_cast_model(&data).unwrap(),
        json!({"time_raw":0,
        "chunk_size":12,"type":37,"flags_raw":2,"duration_ms":1750})
    );
    assert!(transition_cast_model(&data[..21]).is_err());
    data[8..12].copy_from_slice(&5u32.to_be_bytes());
    assert!(transition_cast_model(&data).is_err());
    data[4..8].copy_from_slice(&u32::MAX.to_be_bytes());
    assert!(transition_cast_model(&data).is_err());
}

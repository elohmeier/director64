//! Classic CASt field/button presentation bytes. Format facts corroborated
//! against clean ScummVM41ac2b31847622d0662d22c03fe6979e3b43cfbc,
//! castmember/text.cpp; Lingo defaults require separate original observations.
use serde_json::{Value, json};

/// Native CASt buttons append a BE16 kind to the classic28-byte text data.
pub fn button(data: &[u8]) -> Result<u16, String> {
    let tail = data.get(28..30).ok_or("truncated native button metadata")?;
    let kind = u16::from_be_bytes([tail[0], tail[1]]);
    if !(1..=3).contains(&kind) {
        return Err(format!("unsupported native button type {kind}"));
    }
    Ok(kind)
}
pub fn classic(data: &[u8]) -> Result<Value, String> {
    if data.len() < 28 {
        return Err("truncated classic text box metadata".into());
    }
    let kind = match data[3] {
        0 => 1, // adjust
        1 => 3, // scroll
        2 => 2, // fixed (original8.5 LogfileName field observation)
        3 => 4, // limit, documented in the MX2004 Scripting Reference p671
        _ => return Err("unsupported classic text box type".into()),
    };
    // Every audited selected classic field uses replicated16-bit channels.
    // Reject a different encoding instead of guessing between its two bytes.
    if (6..12).step_by(2).any(|at| data[at] != data[at + 1]) {
        return Err(
            "non-replicated classic text background requires original reference evidence".into(),
        );
    }
    let background = (u32::from(data[7]) << 16) | (u32::from(data[9]) << 8) | u32::from(data[11]);
    Ok(json!({"type":kind,"margin":data[1],"border":data[0],
        "boxDropShadow":data[2],"wordWrap":data[25]&4==0,"background":background}))
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn native_button_kinds_are_big_endian_and_bounded() {
        let mut data = [0; 30];
        for kind in 1..=3 {
            data[28..30].copy_from_slice(&u16::to_be_bytes(kind));
            assert_eq!(button(&data).unwrap(), kind);
        }
        for invalid in [0u16, 4, 256] {
            data[28..30].copy_from_slice(&invalid.to_be_bytes());
            assert!(button(&data).is_err());
        }
        for n in 0..30 {
            assert!(button(&data[..n]).is_err());
        }
    }
    #[test]
    fn source_presentation_bytes_and_wrap_flag_are_independent() {
        let mut data = [0; 30];
        data[0] = 1;
        data[1] = 2;
        data[2] = 3;
        data[3] = 2;
        data[25] = 5;
        data[6..12].copy_from_slice(&[0x12, 0x12, 0x34, 0x34, 0x56, 0x56]);
        assert_eq!(
            classic(&data).unwrap(),
            json!({"type":2,"margin":2,"border":1,
                "boxDropShadow":3,"wordWrap":false,"background":0x123456})
        );
        data[25] = 1;
        data[3] = 0;
        assert_eq!(classic(&data).unwrap()["wordWrap"], true);
        assert_eq!(classic(&data).unwrap()["type"], 1);
        for n in 0..28 {
            assert!(classic(&data[..n]).is_err());
        }
        for (source, kind) in [(0, 1), (1, 3), (2, 2), (3, 4)] {
            data[3] = source;
            assert_eq!(classic(&data).unwrap()["type"], kind);
        }
        data[3] = 4;
        assert!(classic(&data).is_err());
        data[3] = 0;
        data[6] = 0xff;
        assert!(classic(&data).is_err());
    }
}

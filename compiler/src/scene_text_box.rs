use serde_json::Value;
#[derive(Clone, Debug, PartialEq)]
pub struct TextBox {
    pub kind: u8,
    pub margin: u8,
    pub border: u8,
    pub shadow: u8,
    pub wrap: bool,
    pub background: u32,
}
pub fn read(value: &Value, enabled: bool, kind: u16) -> Result<Option<TextBox>, String> {
    if value.is_null() {
        return Ok(None);
    }
    if !enabled || !matches!(kind, 3 | 7) {
        return Err("text box metadata requires a supported text member".into());
    }
    let byte = |name: &str| {
        value[name]
            .as_u64()
            .filter(|v| *v <= 255)
            .map(|v| v as u8)
            .ok_or_else(|| format!("invalid text box {name}"))
    };
    let data = TextBox {
        kind: byte("type")?,
        margin: byte("margin")?,
        border: byte("border")?,
        shadow: byte("boxDropShadow")?,
        wrap: value["wordWrap"]
            .as_bool()
            .ok_or("invalid text box wordWrap")?,
        background: value["background"]
            .as_u64()
            .filter(|v| *v <= 0xffffff)
            .ok_or("invalid text box background")? as u32,
    };
    if !(1..=4).contains(&data.kind) {
        return Err("invalid text box type".into());
    }
    Ok(Some(data))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn explicit_box_metadata_is_typed_and_bounded_without_guessing_defaults() {
        let good = json!({"type":2,"margin":2,"border":1,"boxDropShadow":2,
            "wordWrap":false,"background":0x123456});
        let result = read(&good, true, 3).unwrap().unwrap();
        assert_eq!(result.background, 0x123456);
        assert!(!result.wrap);
        let mut limited = good.clone();
        limited["type"] = json!(4);
        assert_eq!(read(&limited, true, 3).unwrap().unwrap().kind, 4);
        assert!(read(&Value::Null, false, 3).unwrap().is_none());
        assert!(read(&good, false, 3).is_err());
        assert!(read(&good, true, 1).is_err());
        for (key, value) in [
            ("type", json!(0)),
            ("type", json!(5)),
            ("margin", json!(-1)),
            ("border", json!(256)),
            ("boxDropShadow", json!(1.5)),
            ("wordWrap", json!(1)),
            ("background", json!(0x1000000)),
        ] {
            let mut invalid = good.clone();
            invalid[key] = value;
            assert!(read(&invalid, true, 3).is_err());
        }
    }
}

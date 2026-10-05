//! JavaScript semantics the converter's outputs were defined by: JSON as
//! `JSON.stringify` writes it (integer-like keys first, ECMAScript number
//! formatting, minimal escaping), `Math.round`, and the text decoders the
//! JS stages used. The Rust port writes the same bytes the JS stages did.

use serde_json::{Map, Value};

/// `Number.prototype.toString()` for a finite double.
pub fn number(x: f64) -> String {
    if !x.is_finite() {
        return "null".into(); // JSON.stringify's rendering
    }
    if x == 0.0 {
        return "0".into();
    }
    // Safe integers print exactly; larger ones take the shortest digits below.
    if x.fract() == 0.0 && x.abs() < 9_007_199_254_740_992.0 {
        return format!("{}", x as i64);
    }
    let sign = if x < 0.0 { "-" } else { "" };
    let scientific = format!("{:e}", x.abs());
    let (mantissa, exponent) = scientific.split_once('e').unwrap();
    let digits: String = mantissa.chars().filter(|c| *c != '.').collect();
    let k = digits.len() as i32;
    let n = exponent.parse::<i32>().unwrap() + 1;
    let body = if k <= n && n <= 21 {
        format!("{digits}{}", "0".repeat((n - k) as usize))
    } else if 0 < n && n <= 21 {
        format!("{}.{}", &digits[..n as usize], &digits[n as usize..])
    } else if -6 < n && n <= 0 {
        format!("0.{}{digits}", "0".repeat((-n) as usize))
    } else {
        let e = n - 1;
        let sign = if e < 0 { '-' } else { '+' };
        if k == 1 {
            format!("{digits}e{sign}{}", e.abs())
        } else {
            format!("{}.{}e{sign}{}", &digits[..1], &digits[1..], e.abs())
        }
    };
    format!("{sign}{body}")
}

/// `Math.round`: halves round toward +infinity.
pub fn round(x: f64) -> f64 {
    let floor = x.floor();
    if x - floor >= 0.5 { floor + 1.0 } else { floor }
}

fn array_index(key: &str) -> Option<u32> {
    if key == "0" {
        return Some(0);
    }
    if key.is_empty() || key.starts_with('0') || !key.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    key.parse::<u32>().ok().filter(|&n| n != u32::MAX)
}

/// An object's keys in JavaScript property order: array indices ascending,
/// then the rest in insertion order.
pub fn keys(map: &Map<String, Value>) -> Vec<&String> {
    let mut indices: Vec<(u32, &String)> =
        map.keys().filter_map(|k| array_index(k).map(|n| (n, k))).collect();
    indices.sort();
    let mut out: Vec<&String> = indices.into_iter().map(|(_, k)| k).collect();
    out.extend(map.keys().filter(|k| array_index(k).is_none()));
    out
}

pub fn string(text: &str, out: &mut String) {
    out.push('"');
    for c in text.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\u{8}' => out.push_str("\\b"),
            '\u{c}' => out.push_str("\\f"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
}

fn write(value: &Value, indent: &str, level: usize, out: &mut String) {
    let newline = |out: &mut String, level: usize| {
        if !indent.is_empty() {
            out.push('\n');
            for _ in 0..level {
                out.push_str(indent);
            }
        }
    };
    match value {
        Value::Null => out.push_str("null"),
        Value::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Value::Number(n) => {
            if let Some(i) = n.as_i64() {
                out.push_str(&i.to_string());
            } else if let Some(u) = n.as_u64() {
                out.push_str(&u.to_string());
            } else {
                out.push_str(&number(n.as_f64().unwrap_or(f64::NAN)));
            }
        }
        Value::String(s) => string(s, out),
        Value::Array(items) => {
            if items.is_empty() {
                out.push_str("[]");
                return;
            }
            out.push('[');
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                newline(out, level + 1);
                write(item, indent, level + 1, out);
            }
            newline(out, level);
            out.push(']');
        }
        Value::Object(map) => {
            if map.is_empty() {
                out.push_str("{}");
                return;
            }
            out.push('{');
            for (i, key) in keys(map).into_iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                newline(out, level + 1);
                string(key, out);
                out.push(':');
                if !indent.is_empty() {
                    out.push(' ');
                }
                write(&map[key], indent, level + 1, out);
            }
            newline(out, level);
            out.push('}');
        }
    }
}

/// `JSON.stringify(value)`.
pub fn stringify(value: &Value) -> String {
    let mut out = String::new();
    write(value, "", 0, &mut out);
    out
}

/// `JSON.stringify(value, null, 2)`.
pub fn stringify_pretty(value: &Value) -> String {
    let mut out = String::new();
    write(value, "  ", 0, &mut out);
    out
}

/// source-model's `canonical`: every object's keys sorted (UTF-16 order,
/// which is code point order for these keys).
pub fn canonical(value: &Value) -> Value {
    match value {
        Value::Array(items) => Value::Array(items.iter().map(canonical).collect()),
        Value::Object(map) => {
            let mut keys: Vec<&String> = map.keys().collect();
            keys.sort_by(|a, b| a.encode_utf16().cmp(b.encode_utf16()));
            Value::Object(keys.into_iter().map(|k| (k.clone(), canonical(&map[k]))).collect())
        }
        other => other.clone(),
    }
}

/// source-model's `serialize`: canonical, two-space indented, newline.
pub fn serialize(value: &Value) -> String {
    stringify_pretty(&canonical(value)) + "\n"
}

/// A double as a JSON value, with integral values kept integral (a JS
/// number has one representation for both).
pub fn num(x: f64) -> Value {
    if x.fract() == 0.0 && x.abs() < 9.007_199_254_740_992e15 {
        Value::from(x as i64)
    } else {
        serde_json::Number::from_f64(x).map(Value::Number).unwrap_or(Value::Null)
    }
}

/// `Buffer.toString("latin1")`: bytes as U+0000..U+00FF.
pub fn latin1(bytes: &[u8]) -> String {
    bytes.iter().map(|&b| b as char).collect()
}

/// `new TextDecoder("macintosh")`.
pub fn mac_roman(bytes: &[u8]) -> String {
    const HIGH: [char; 128] = [
        'Ä', 'Å', 'Ç', 'É', 'Ñ', 'Ö', 'Ü', 'á', 'à', 'â', 'ä', 'ã', 'å', 'ç', 'é', 'è', 'ê', 'ë', 'í', 'ì', 'î',
        'ï', 'ñ', 'ó', 'ò', 'ô', 'ö', 'õ', 'ú', 'ù', 'û', 'ü', '†', '°', '¢', '£', '§', '•', '¶', 'ß', '®', '©',
        '™', '´', '¨', '≠', 'Æ', 'Ø', '∞', '±', '≤', '≥', '¥', 'µ', '∂', '∑', '∏', 'π', '∫', 'ª', 'º', 'Ω', 'æ',
        'ø', '¿', '¡', '¬', '√', 'ƒ', '≈', '∆', '«', '»', '…', '\u{a0}', 'À', 'Ã', 'Õ', 'Œ', 'œ', '–', '—', '“',
        '”', '‘', '’', '÷', '◊', 'ÿ', 'Ÿ', '⁄', '€', '‹', '›', 'ﬁ', 'ﬂ', '‡', '·', '‚', '„', '‰', 'Â', 'Ê',
        'Á', 'Ë', 'È', 'Í', 'Î', 'Ï', 'Ì', 'Ó', 'Ô', '\u{f8ff}', 'Ò', 'Ú', 'Û', 'Ù', 'ı', 'ˆ', '˜', '¯', '˘',
        '˙', '˚', '¸', '˝', '˛', 'ˇ',
    ];
    bytes.iter().map(|&b| if b < 128 { b as char } else { HIGH[b as usize - 128] }).collect()
}

/// `new TextDecoder("windows-1252")` (also what the label "latin1" selects).
pub fn windows_1252(bytes: &[u8]) -> String {
    const HIGH: [char; 32] = [
        '€', '\u{81}', '‚', 'ƒ', '„', '…', '†', '‡', 'ˆ', '‰', 'Š', '‹', 'Œ', '\u{8d}', 'Ž', '\u{8f}', '\u{90}',
        '‘', '’', '“', '”', '•', '–', '—', '˜', '™', 'š', '›', 'œ', '\u{9d}', 'ž', 'Ÿ',
    ];
    bytes
        .iter()
        .map(|&b| if (0x80..0xa0).contains(&b) { HIGH[b as usize - 0x80] } else { b as char })
        .collect()
}

pub fn hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for &b in bytes {
        out.push(DIGITS[(b >> 4) as usize] as char);
        out.push(DIGITS[(b & 15) as usize] as char);
    }
    out
}

pub fn unhex(text: &str) -> Result<Vec<u8>, String> {
    if text.len() % 2 != 0 {
        return Err("odd hex length".into());
    }
    (0..text.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&text[i..i + 2], 16).map_err(|_| "invalid hex".to_string()))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn numbers_format_as_javascript() {
        for (x, s) in [
            (0.0, "0"), (-0.0, "0"), (1.0, "1"), (1.5, "1.5"), (0.1, "0.1"), (1e21, "1e+21"),
            (123456789012345680000.0, "123456789012345680000"), (1e-7, "1e-7"), (0.000001, "0.000001"),
            (1.23e-7, "1.23e-7"), (-2.5, "-2.5"), (8.233333333333333, "8.233333333333333"), (65536.5, "65536.5"),
        ] {
            assert_eq!(number(x), s, "{x}");
        }
        assert_eq!((round(0.49999999999999994), round(-0.5), round(2.5)), (0.0, -0.0, 3.0));
    }

    #[test]
    fn stringify_orders_index_keys_first() {
        let v = json!({"b": 1, "10": 2, "a": [1.5, "x\n"], "2": null, "01": true});
        assert_eq!(stringify(&v), r#"{"2":null,"10":2,"b":1,"a":[1.5,"x\n"],"01":true}"#);
        assert_eq!(stringify_pretty(&json!({"a": [], "b": {}, "c": [1]})), "{\n  \"a\": [],\n  \"b\": {},\n  \"c\": [\n    1\n  ]\n}");
        assert_eq!(serialize(&json!({"b": 1, "a": {"d": 1, "c": 2}})), "{\n  \"a\": {\n    \"c\": 2,\n    \"d\": 1\n  },\n  \"b\": 1\n}\n");
    }

    #[test]
    fn decoders_match_the_web_platform() {
        assert_eq!(mac_roman(&[0x80, 0x8a, 0xa5, 0x41]), "Ää•A");
        assert_eq!(windows_1252(&[0x80, 0xe4, 0x9f]), "€äŸ");
        assert_eq!(latin1(&[0x80, 0xe4]), "\u{80}ä");
    }
}

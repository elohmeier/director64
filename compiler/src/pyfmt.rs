//! Formatting that reproduces the Python generator's output byte for byte:
//! `repr(float)`, `json.dumps(indent=2)` and the octal-escaped C string.

use serde_json::Value;

/// Python's `repr` of a float: shortest round-trip digits, fixed notation
/// for decimal exponents in [-4, 16), scientific with a signed two-digit
/// exponent otherwise, and a `.0` on integral values.
pub fn float_repr(value: f64) -> String {
    if value.is_nan() {
        return "nan".into();
    }
    if value.is_infinite() {
        return if value > 0.0 { "inf".into() } else { "-inf".into() };
    }
    let sign = if value.is_sign_negative() { "-" } else { "" };
    let scientific = format!("{:e}", value.abs());
    let (mantissa, exponent) = scientific.split_once('e').expect("exponent");
    let digits: String = mantissa.chars().filter(|c| *c != '.').collect();
    let exponent: i32 = exponent.parse().expect("exponent digits");
    let decpt = exponent + 1;
    let n = digits.len() as i32;
    let body = if decpt <= -4 || decpt > 16 {
        let mut s = String::new();
        s.push_str(&digits[..1]);
        if n > 1 {
            s.push('.');
            s.push_str(&digits[1..]);
        }
        let e = decpt - 1;
        s.push_str(&format!("e{}{:02}", if e < 0 { '-' } else { '+' }, e.abs()));
        s
    } else if decpt <= 0 {
        format!("0.{}{}", "0".repeat((-decpt) as usize), digits)
    } else if decpt >= n {
        format!("{}{}.0", digits, "0".repeat((decpt - n) as usize))
    } else {
        format!("{}.{}", &digits[..decpt as usize], &digits[decpt as usize..])
    };
    format!("{sign}{body}")
}

fn escape_json(text: &str, out: &mut String) {
    out.push('"');
    for c in text.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{8}' => out.push_str("\\b"),
            '\u{c}' => out.push_str("\\f"),
            c if (c as u32) < 0x20 || (c as u32) > 0x7e => {
                let mut units = [0u16; 2];
                for unit in c.encode_utf16(&mut units) {
                    out.push_str(&format!("\\u{:04x}", unit));
                }
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

fn dump(value: &Value, level: usize, out: &mut String) {
    let pad = |level: usize| "  ".repeat(level);
    match value {
        Value::Null => out.push_str("null"),
        Value::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Value::Number(n) => {
            if let Some(i) = n.as_i64() {
                out.push_str(&i.to_string());
            } else if let Some(u) = n.as_u64() {
                out.push_str(&u.to_string());
            } else {
                out.push_str(&float_repr(n.as_f64().unwrap_or(0.0)));
            }
        }
        Value::String(s) => escape_json(s, out),
        Value::Array(items) => {
            if items.is_empty() {
                out.push_str("[]");
                return;
            }
            out.push_str("[\n");
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.push_str(",\n");
                }
                out.push_str(&pad(level + 1));
                dump(item, level + 1, out);
            }
            out.push('\n');
            out.push_str(&pad(level));
            out.push(']');
        }
        Value::Object(map) => {
            if map.is_empty() {
                out.push_str("{}");
                return;
            }
            out.push_str("{\n");
            for (i, (key, item)) in map.iter().enumerate() {
                if i > 0 {
                    out.push_str(",\n");
                }
                out.push_str(&pad(level + 1));
                escape_json(key, out);
                out.push_str(": ");
                dump(item, level + 1, out);
            }
            out.push('\n');
            out.push_str(&pad(level));
            out.push('}');
        }
    }
}

/// `json.dumps(value, indent=2)` with Python's default ASCII-only escapes.
pub fn json_dumps(value: &Value) -> String {
    let mut out = String::new();
    dump(value, 0, &mut out);
    out
}

/// A C string literal: printable ASCII verbatim, every other UTF-8 byte as
/// an octal escape (a hex escape could swallow a following hex digit).
pub fn cstring(text: &str) -> String {
    let mut out = String::from("\"");
    for b in text.bytes() {
        if (32..127).contains(&b) && b != b'"' && b != b'\\' {
            out.push(b as char);
        } else {
            out.push_str(&format!("\\{b:03o}"));
        }
    }
    out.push('"');
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn float_repr_matches_python() {
        for (value, expected) in [
            (0.0, "0.0"),
            (1.0, "1.0"),
            (2.5, "2.5"),
            (100.0, "100.0"),
            (0.0001, "0.0001"),
            (0.00001, "1e-05"),
            (1e15, "1000000000000000.0"),
            (1e16, "1e+16"),
            (1.5e-7, "1.5e-07"),
            (3.141592653589793, "3.141592653589793"),
            (-0.5, "-0.5"),
            (123456789012345680.0, "1.2345678901234568e+17"),
        ] {
            assert_eq!(float_repr(value), expected, "{value}");
        }
    }
}

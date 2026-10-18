//! Finite, proven font resource uses in retained AVM1. This is resource
//! discovery, not action execution: unknown dataflow loses facts. Runtime
//! font lookup remains strict for dynamically computed faces and sizes.
use super::*;
use std::collections::BTreeSet;
#[derive(Clone, Default)]
enum Fact {
    #[default]
    Unknown,
    Text(String),
    Number(f64),
    Format(usize),
}
#[derive(Default)]
struct Format {
    font: Option<String>,
    size: Option<u32>,
}
fn string(b: &[u8], p: &mut usize) -> R<String> {
    let end = index_of(b, 0, *p).ok_or("unterminated font-use action string")?;
    let value = js::latin1(&b[*p..end]);
    *p = end + 1;
    Ok(value)
}
fn size(value: &Fact) -> Option<u32> {
    match value {
        Fact::Number(n) if n.is_finite() && *n >= 1.0 && *n <= 255.0 => Some(n.round() as u32),
        _ => None,
    }
}
fn record(format: &Format, uses: &mut BTreeSet<(String, u32)>) {
    if let (Some(font), Some(size)) = (&format.font, format.size) {
        uses.insert((font.clone(), size));
    }
}
fn pop(stack: &mut Vec<Fact>) -> Fact {
    stack.pop().unwrap_or_default()
}
fn arguments(stack: &mut Vec<Fact>) -> Vec<Fact> {
    let Fact::Number(n) = pop(stack) else {
        stack.clear();
        return Vec::new();
    };
    if !n.is_finite() || n < 0.0 || n > 256.0 || n.fract() != 0.0 {
        stack.clear();
        return Vec::new();
    }
    (0..n as usize).map(|_| pop(stack)).collect()
}
fn block(
    b: &[u8],
    initial_pool: &[String],
    uses: &mut BTreeSet<(String, u32)>,
    depth: usize,
) -> R<()> {
    if depth > 16 {
        return Err("excessive SWF font-use function nesting".into());
    }
    let (mut p, mut pool, mut stack) = (0, initial_pool.to_vec(), Vec::new());
    let mut registers = vec![Fact::Unknown; 256];
    let mut variables = HashMap::new();
    let mut formats = Vec::<Format>::new();
    while p < b.len() {
        let op = b[p];
        p += 1;
        if op == 0 {
            break;
        }
        let length = if op >= 128 {
            let length = u16le(b, p)?;
            p += 2;
            length
        } else {
            0
        };
        let data = b.get(p..p + length).ok_or("truncated font-use action")?;
        p += length;
        match op {
            0x88 => {
                let count = u16le(data, 0)?;
                let mut q = 2;
                pool.clear();
                for _ in 0..count {
                    pool.push(string(data, &mut q)?);
                }
            }
            0x96 => {
                let mut q = 0;
                while q < data.len() {
                    let kind = data[q];
                    q += 1;
                    let fact = match kind {
                        0 => Fact::Text(string(data, &mut q)?),
                        1 => {
                            let bytes = data.get(q..q + 4).ok_or("truncated font-use float")?;
                            q += 4;
                            Fact::Number(f32::from_le_bytes(bytes.try_into().unwrap()) as f64)
                        }
                        2 | 3 => Fact::Unknown,
                        4 => {
                            let n = byte(data, q)?;
                            q += 1;
                            registers[n as usize].clone()
                        }
                        5 => {
                            q += 1;
                            Fact::Unknown
                        }
                        6 => {
                            let bytes = data.get(q..q + 8).ok_or("truncated font-use double")?;
                            q += 8;
                            Fact::Number(f64::from_le_bytes([
                                bytes[4], bytes[5], bytes[6], bytes[7], bytes[0], bytes[1],
                                bytes[2], bytes[3],
                            ]))
                        }
                        7 => {
                            let n = u32le(data, q)? as u32 as i32;
                            q += 4;
                            Fact::Number(n as f64)
                        }
                        8 | 9 => {
                            let n = if kind == 8 {
                                let n = byte(data, q)? as usize;
                                q += 1;
                                n
                            } else {
                                let n = u16le(data, q)?;
                                q += 2;
                                n
                            };
                            pool.get(n).cloned().map(Fact::Text).unwrap_or_default()
                        }
                        _ => return Err("unknown font-use push type".into()),
                    };
                    stack.push(fact);
                }
            }
            0x17 => {
                pop(&mut stack);
            }
            0x4c => stack.push(stack.last().cloned().unwrap_or_default()),
            0x4d => {
                let a = pop(&mut stack);
                let b = pop(&mut stack);
                stack.extend([a, b]);
            }
            0x87 => registers[byte(data, 0)? as usize] = stack.last().cloned().unwrap_or_default(),
            0x1c => {
                let value = match pop(&mut stack) {
                    Fact::Text(name) => variables.get(&name).cloned().unwrap_or_default(),
                    _ => Fact::Unknown,
                };
                stack.push(value);
            }
            0x1d | 0x3c => {
                let value = pop(&mut stack);
                if let Fact::Text(name) = pop(&mut stack) {
                    variables.insert(name, value);
                }
            }
            0x40 => {
                let name = pop(&mut stack);
                let args = arguments(&mut stack);
                if matches!(name,Fact::Text(ref n) if n=="TextFormat") {
                    let font = args.first().and_then(|v| match v {
                        Fact::Text(s) => Some(s.clone()),
                        _ => None,
                    });
                    let format = Format {
                        font,
                        size: args.get(1).and_then(size),
                    };
                    record(&format, uses);
                    stack.push(Fact::Format(formats.len()));
                    formats.push(format);
                } else {
                    stack.push(Fact::Unknown);
                }
            }
            0x4e => {
                pop(&mut stack);
                pop(&mut stack);
                stack.push(Fact::Unknown);
            }
            0x4f => {
                let value = pop(&mut stack);
                let property = pop(&mut stack);
                let owner = pop(&mut stack);
                if let (Fact::Format(index), Fact::Text(property)) = (owner, property) {
                    let format = &mut formats[index];
                    match property.as_str() {
                        "font" => {
                            format.font = match value {
                                Fact::Text(s) => Some(s),
                                _ => None,
                            }
                        }
                        "size" => format.size = size(&value),
                        _ => {}
                    }
                    record(format, uses);
                }
            }
            0x52 => {
                let method = pop(&mut stack);
                pop(&mut stack);
                let args = arguments(&mut stack);
                if matches!(method,Fact::Text(ref n) if n=="createTextField") && args.len() >= 6 {
                    uses.insert(("Times New Roman".into(), 12));
                }
                if matches!(method,Fact::Text(ref n) if n=="setTextFormat"||n=="setNewTextFormat") {
                    if let Some(Fact::Format(index)) = args.last() {
                        record(&formats[*index], uses);
                    }
                }
                if matches!(method,Fact::Text(ref n) if n=="getTextFormat"||n=="getNewTextFormat") {
                    stack.push(Fact::Format(formats.len()));
                    formats.push(Format::default());
                } else {
                    stack.push(Fact::Unknown);
                }
            }
            0x8e | 0x9b => {
                let body = u16le(
                    data,
                    data.len().checked_sub(2).ok_or("short font-use function")?,
                )?;
                block(
                    b.get(p..p + body)
                        .ok_or("truncated font-use function body")?,
                    &pool,
                    uses,
                    depth + 1,
                )?;
                p += body;
                stack.push(Fact::Unknown);
            }
            // Branches and unsupported abstract operations invalidate state;
            // never mistake adjacent literals on different paths for a use.
            _ => {
                stack.clear();
                registers.fill(Fact::Unknown);
                variables.clear();
            }
        }
        if stack.len() > 4096 || formats.len() > 4096 {
            return Err("SWF font-use capacity exceeded".into());
        }
    }
    Ok(())
}
pub(super) fn actions(value: &Value) -> R<Vec<Value>> {
    fn walk(value: &Value, uses: &mut BTreeSet<(String, u32)>) -> R<()> {
        match value {
            Value::Object(map) => {
                if let Some(bytes) = map.get("code").and_then(Value::as_array) {
                    let b = bytes
                        .iter()
                        .map(|v| {
                            v.as_u64()
                                .filter(|n| *n <= 255)
                                .map(|n| n as u8)
                                .ok_or("invalid font-use action byte".to_string())
                        })
                        .collect::<R<Vec<_>>>()?;
                    block(&b, &[], uses, 0)?;
                }
                for (k, v) in map {
                    if k != "code" {
                        walk(v, uses)?;
                    }
                }
            }
            Value::Array(all) => {
                for v in all {
                    walk(v, uses)?;
                }
            }
            _ => {}
        }
        Ok(())
    }
    let mut uses = BTreeSet::new();
    walk(value, &mut uses)?;
    Ok(uses.into_iter().map(|(font,size)|json!({"fontName":font,"fontHeight":size,"source":"avm1-static-font-use"})).collect())
}

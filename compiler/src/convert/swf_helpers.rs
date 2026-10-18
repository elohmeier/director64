//! Semantic lowering of straight-line AVM1 TextField helpers. Only complete
//! proven programs are accepted; unknown instructions retain the normal
//! capability failure. Function names and constant-pool positions are arbitrary.
use super::*;

#[derive(Clone, Debug, PartialEq)]
enum Avm {
    Text(String),
    Number(f64),
    Object(String),
    This,
    Argument(usize),
    Format(String),
    Unescaped(usize),
    Function(Helper),
    Undefined,
    Boolean(bool),
}
#[derive(Clone, Debug, PartialEq)]
struct Helper {
    target: Option<String>,
    property: &'static str,
    operation: u8,
    argument: usize,
}

fn string(b: &[u8], p: &mut usize) -> Option<String> {
    let end = index_of(b, 0, *p)?;
    let out = js::latin1(&b[*p..end]);
    *p = end + 1;
    Some(out)
}
fn push(b: &[u8], pool: &[String], registers: &[Avm], stack: &mut Vec<Avm>) -> Option<()> {
    let mut p = 0;
    while p < b.len() {
        let kind = b[p];
        p += 1;
        stack.push(match kind {
            0 => Avm::Text(string(b, &mut p)?),
            3 => Avm::Undefined,
            4 => {
                let r = *b.get(p)? as usize;
                p += 1;
                registers.get(r)?.clone()
            }
            5 => {
                let n = *b.get(p)?;
                p += 1;
                if n > 1 {
                    return None;
                }
                Avm::Boolean(n != 0)
            }
            6 => {
                let bytes = b.get(p..p + 8)?;
                p += 8;
                // SWF DOUBLE stores its two little-endian words high first.
                let raw = [
                    bytes[4], bytes[5], bytes[6], bytes[7], bytes[0], bytes[1], bytes[2], bytes[3],
                ];
                let number = f64::from_le_bytes(raw);
                if !number.is_finite() {
                    return None;
                }
                Avm::Number(number)
            }
            7 => {
                let n = u32le(b, p).ok()? as u32 as i32;
                p += 4;
                Avm::Number(n as f64)
            }
            8 => {
                let i = *b.get(p)? as usize;
                p += 1;
                Avm::Text(pool.get(i)?.clone())
            }
            9 => {
                let i = u16le(b, p).ok()?;
                p += 2;
                Avm::Text(pool.get(i)?.clone())
            }
            _ => return None,
        });
        if stack.len() > 256 {
            return None;
        }
    }
    Some(())
}
fn arguments(stack: &mut Vec<Avm>) -> Option<Vec<Avm>> {
    let Avm::Number(n) = stack.pop()? else {
        return None;
    };
    if n < 0.0 || n > 16.0 || n.fract() != 0.0 {
        return None;
    }
    (0..n as usize).map(|_| stack.pop()).collect()
}
fn action<'a>(b: &'a [u8], p: &mut usize) -> Option<(u8, &'a [u8])> {
    let code = *b.get(*p)?;
    *p += 1;
    let length = if code >= 128 {
        let n = u16le(b, *p).ok()?;
        *p += 2;
        n
    } else {
        0
    };
    let end = p.checked_add(length)?;
    let data = b.get(*p..end)?;
    *p = end;
    Some((code, data))
}

/// Prove the complete once-only program `if (guard == undefined) {
/// gotoAndPlay(random(N)+1); guard=true; }`. The guard name is arbitrary.
/// Its lifetime is validated against the containing display list separately.
pub(super) fn random_initial_phase(payload: &[u8]) -> Option<u8> {
    let mut p = 0;
    let (code, data) = action(payload, &mut p)?;
    if code != 0x88 {
        return None;
    }
    let count = u16le(data, 0).ok()?;
    let mut q = 2;
    let mut pool = Vec::new();
    for _ in 0..count {
        pool.push(string(data, &mut q)?);
    }
    if q != data.len() {
        return None;
    }
    let mut records = Vec::new();
    while p < payload.len() {
        let (code, data) = action(payload, &mut p)?;
        records.push((code, data, p));
    }
    if records.iter().map(|r| r.0).collect::<Vec<_>>()
        != [
            0x96, 0x1c, 0x96, 0x49, 0x12, 0x9d, 0x96, 0x30, 0x96, 0x47, 0x9f, 0x96, 0x1d, 0,
        ]
    {
        return None;
    }
    let values = |index: usize| -> Option<Vec<Avm>> {
        let mut stack = Vec::new();
        push(records[index].1, &pool, &[], &mut stack)?;
        Some(stack)
    };
    let first = values(0)?;
    let [Avm::Text(guard)] = first.as_slice() else {
        return None;
    };
    if guard.is_empty()
        || values(2)? != [Avm::Undefined]
        || values(8)? != [Avm::Number(1.0)]
        || values(11)? != [Avm::Text(guard.clone()), Avm::Boolean(true)]
        || records[10].1 != [1]
    {
        return None;
    }
    let jump = records[5].1;
    if jump.len() != 2
        || (records[5].2 as i64 + i16::from_le_bytes([jump[0], jump[1]]) as i64)
            != (records[13].2 - 1) as i64
    {
        return None;
    }
    let n = values(6)?;
    let [Avm::Number(n)] = n.as_slice() else {
        return None;
    };
    if !(2.0..=255.0).contains(n) || n.fract() != 0.0 {
        return None;
    }
    Some(*n as u8)
}
fn function(header: &[u8], body: &[u8], pool: &[String]) -> Option<Helper> {
    let mut p = 0;
    if !string(header, &mut p)?.is_empty() {
        return None;
    }
    let count = u16le(header, p).ok()?;
    p += 2;
    let register_count = *header.get(p)? as usize;
    p += 1;
    let flags = u16le(header, p).ok()?;
    p += 2;
    // The supported bodies use explicit argument registers and optionally
    // preload this. Other implicit objects are not silently substituted.
    if flags & !0x2b != 0 || flags & 3 == 3 {
        return None;
    }
    let mut registers = vec![Avm::Undefined; register_count + 1];
    if flags & 1 != 0 {
        *registers.get_mut(1)? = Avm::This;
    }
    for argument in 0..count {
        let r = *header.get(p)? as usize;
        p += 1;
        string(header, &mut p)?;
        if r == 0 || registers.get(r)? != &Avm::Undefined {
            return None;
        }
        registers[r] = Avm::Argument(argument);
    }
    if p + 2 != header.len() || u16le(header, p).ok()? != body.len() {
        return None;
    }
    let mut stack = Vec::new();
    let mut format_size: Option<(String, usize)> = None;
    let mut result = None;
    p = 0;
    while p < body.len() {
        let (code, data) = action(body, &mut p)?;
        match code {
            0 => {
                if p != body.len() {
                    return None;
                }
                break;
            }
            0x96 => push(data, pool, &registers, &mut stack)?,
            0x1c => {
                let Avm::Text(name) = stack.pop()? else {
                    return None;
                };
                stack.push(Avm::Object(name));
            }
            0x87 => {
                if data.len() != 1 {
                    return None;
                }
                *registers.get_mut(data[0] as usize)? = stack.last()?.clone();
            }
            0x17 => {
                stack.pop()?;
            }
            0x52 => {
                let Avm::Text(method) = stack.pop()? else {
                    return None;
                };
                let Avm::Object(target) = stack.pop()? else {
                    return None;
                };
                let args = arguments(&mut stack)?;
                if method == "getTextFormat" && args.is_empty() {
                    stack.push(Avm::Format(target));
                } else if method == "setTextFormat" && args == [Avm::Format(target.clone())] {
                    let (changed, argument) = format_size.take()?;
                    if changed != target || result.is_some() {
                        return None;
                    }
                    result = Some(Helper {
                        target: Some(target),
                        property: "size",
                        operation: 3,
                        argument,
                    });
                    stack.push(Avm::Undefined);
                } else {
                    return None;
                }
            }
            0x3d => {
                let Avm::Text(name) = stack.pop()? else {
                    return None;
                };
                let args = arguments(&mut stack)?;
                if name != "unescape" {
                    return None;
                }
                let [Avm::Argument(argument)] = args.as_slice() else {
                    return None;
                };
                stack.push(Avm::Unescaped(*argument));
            }
            0x4f => {
                let value = stack.pop()?;
                let Avm::Text(property) = stack.pop()? else {
                    return None;
                };
                match (stack.pop()?, property.as_str(), value) {
                    (Avm::Format(target), "size", Avm::Argument(argument)) => {
                        if format_size.is_some() {
                            return None;
                        }
                        format_size = Some((target, argument));
                    }
                    (owner, "text", Avm::Unescaped(argument)) => {
                        let target = match owner {
                            Avm::This => None,
                            Avm::Object(name) => Some(name),
                            _ => return None,
                        };
                        if result.is_some() {
                            return None;
                        }
                        result = Some(Helper {
                            target,
                            property: "text",
                            operation: 4,
                            argument,
                        });
                    }
                    _ => return None,
                }
            }
            _ => return None,
        }
    }
    if stack.is_empty() && format_size.is_none() {
        result
    } else {
        None
    }
}

pub(super) fn lower(payload: &[u8]) -> Option<(Vec<Value>, bool)> {
    let mut p = 0;
    let mut pool = Vec::new();
    let mut stack = Vec::new();
    let mut helpers = Vec::new();
    let mut stop = false;
    while p < payload.len() {
        let (code, data) = action(payload, &mut p)?;
        match code {
            0 => {
                if p != payload.len() {
                    return None;
                }
                break;
            }
            6 => stop = false,
            7 => stop = true,
            0x88 => {
                let count = u16le(data, 0).ok()?;
                let mut q = 2;
                pool.clear();
                for _ in 0..count {
                    pool.push(string(data, &mut q)?);
                }
                if q != data.len() {
                    return None;
                }
            }
            0x96 => push(data, &pool, &[], &mut stack)?,
            0x1c => {
                let Avm::Text(name) = stack.pop()? else {
                    return None;
                };
                stack.push(Avm::Object(name));
            }
            0x8e => {
                let size = u16le(data, data.len().checked_sub(2)?).ok()?;
                let end = p.checked_add(size)?;
                stack.push(Avm::Function(function(data, payload.get(p..end)?, &pool)?));
                p = end;
            }
            0x1d | 0x4f => {
                let Avm::Function(helper) = stack.pop()? else {
                    return None;
                };
                let Avm::Text(name) = stack.pop()? else {
                    return None;
                };
                let method = code == 0x4f;
                let target = if method {
                    let Avm::Object(target) = stack.pop()? else {
                        return None;
                    };
                    if helper.target.as_ref().is_some_and(|t| t != &target) {
                        return None;
                    }
                    target
                } else {
                    helper.target?
                };
                if name.is_empty() || target.is_empty() || helper.argument > 255 {
                    return None;
                }
                helpers.push(
                    json!({"name":name,"target":target,"property":helper.property,
                    "operation":helper.operation,"argument":helper.argument,"method":method}),
                );
            }
            // A constant trace has no visual or game-state side effect.
            0x26 => {
                if !matches!(stack.pop()?, Avm::Text(_)) {
                    return None;
                }
            }
            _ => return None,
        }
    }
    if stack.is_empty() && !helpers.is_empty() {
        Some((helpers, stop))
    } else {
        None
    }
}

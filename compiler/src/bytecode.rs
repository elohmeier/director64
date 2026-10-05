//! The bytecode contract of runtime/lingo/lingo_bytecode.h: opcodes, operand
//! kinds, the assembler that lays states out back to back, and the listing.

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    U8,
    I8,
    U16,
    I32,
    Pc,
}

impl Kind {
    fn width(self) -> usize {
        match self {
            Kind::U8 | Kind::I8 => 1,
            Kind::U16 | Kind::Pc => 2,
            Kind::I32 => 4,
        }
    }
    fn label(self) -> &'static str {
        match self {
            Kind::U8 => "u8",
            Kind::I8 => "i8",
            Kind::U16 => "u16",
            Kind::I32 => "i32",
            Kind::Pc => "pc",
        }
    }
}

#[derive(Debug)]
pub struct Opcode {
    pub name: &'static str,
    pub number: u8,
    pub kinds: &'static [Kind],
}

use Kind::{Pc, I32, I8, U16, U8};
pub const OPCODES: &[Opcode] = &[
    Opcode { name: "line", number: 0, kinds: &[U16] },
    Opcode { name: "num8", number: 1, kinds: &[I8] },
    Opcode { name: "num32", number: 2, kinds: &[I32] },
    Opcode { name: "numd", number: 3, kinds: &[U16] },
    Opcode { name: "dbl", number: 4, kinds: &[U16] },
    Opcode { name: "text", number: 5, kinds: &[U16] },
    Opcode { name: "sym", number: 6, kinds: &[U16] },
    Opcode { name: "void", number: 7, kinds: &[] },
    Opcode { name: "local", number: 8, kinds: &[U8] },
    Opcode { name: "global", number: 9, kinds: &[U16] },
    Opcode { name: "self", number: 10, kinds: &[U16] },
    Opcode { name: "the", number: 11, kinds: &[U16] },
    Opcode { name: "get", number: 12, kinds: &[U16] },
    Opcode { name: "unary", number: 13, kinds: &[U8] },
    Opcode { name: "binary", number: 14, kinds: &[U8] },
    Opcode { name: "index", number: 15, kinds: &[] },
    Opcode { name: "chunk", number: 16, kinds: &[U16] },
    Opcode { name: "chunk_count", number: 17, kinds: &[U16] },
    Opcode { name: "chunk_set", number: 18, kinds: &[U16] },
    Opcode { name: "chunk_delete", number: 19, kinds: &[U16] },
    Opcode { name: "last_chunk", number: 20, kinds: &[U16] },
    Opcode { name: "list", number: 21, kinds: &[U8, U8] },
    Opcode { name: "list_extend", number: 22, kinds: &[U8] },
    Opcode { name: "reference", number: 23, kinds: &[U16] },
    Opcode { name: "call", number: 24, kinds: &[U16, U8] },
    Opcode { name: "result", number: 25, kinds: &[] },
    Opcode { name: "trace", number: 26, kinds: &[] },
    Opcode { name: "set_local", number: 27, kinds: &[U8] },
    Opcode { name: "set_global", number: 28, kinds: &[U16] },
    Opcode { name: "set_self", number: 29, kinds: &[U16] },
    Opcode { name: "set_the", number: 30, kinds: &[U16] },
    Opcode { name: "set", number: 31, kinds: &[U16] },
    Opcode { name: "set_index", number: 32, kinds: &[] },
    Opcode { name: "declare", number: 33, kinds: &[U16] },
    Opcode { name: "call_builtin_expr", number: 34, kinds: &[U16, U8] },
    Opcode { name: "jump", number: 40, kinds: &[Pc] },
    Opcode { name: "yield", number: 41, kinds: &[Pc] },
    Opcode { name: "branch", number: 42, kinds: &[Pc, Pc] },
    Opcode { name: "return", number: 43, kinds: &[] },
    Opcode { name: "return_void", number: 44, kinds: &[] },
    Opcode { name: "invoke", number: 45, kinds: &[U16, U8, Pc] },
    Opcode { name: "invoke_method", number: 46, kinds: &[U16, U8, Pc] },
    Opcode { name: "invoke_expr", number: 47, kinds: &[U16, U8, Pc] },
    Opcode { name: "invoke_method_expr", number: 48, kinds: &[U16, U8, Pc] },
    Opcode { name: "invoke_local", number: 49, kinds: &[U16, U8, U8, Pc] },
    Opcode { name: "self_slot", number: 52, kinds: &[U16, U8] },
    Opcode { name: "set_self_slot", number: 53, kinds: &[U16, U8] },
    Opcode { name: "invoke_local_expr", number: 50, kinds: &[U16, U8, U8, Pc] },
    Opcode { name: "call_builtin", number: 51, kinds: &[U16, U8, Pc] },
];

pub const BINARY_OPERATORS: &[&str] = &[
    "=", "<>", "and", "or", "&", "&&", "contains", "starts", "<", ">", "<=", ">=", "within",
    "intersects", "+", "-", "*", "/", "mod", "^",
];
pub const UNARY_OPERATORS: &[&str] = &["-", "not"];

pub fn opcode(name: &str) -> &'static Opcode {
    OPCODES.iter().find(|op| op.name == name).unwrap_or_else(|| panic!("unknown opcode {name}"))
}

fn is_terminator(op: &Opcode) -> bool {
    op.number >= 40
}

/// One op of a state: pc operands are state indices until assembly.
#[derive(Clone, Debug)]
pub struct Op {
    pub code: &'static Opcode,
    pub args: Vec<i64>,
}

pub fn op(name: &str, args: Vec<i64>) -> Op {
    Op { code: opcode(name), args }
}

fn encode_op(code: &Opcode, args: &[i64], out: &mut Vec<u8>) -> Result<(), String> {
    if code.kinds.len() != args.len() {
        return Err(format!("{} takes {} operands", code.name, code.kinds.len()));
    }
    out.push(code.number);
    for (kind, &value) in code.kinds.iter().zip(args) {
        let fits = match kind {
            Kind::U8 => (0..=255).contains(&value),
            Kind::I8 => (-128..=127).contains(&value),
            Kind::U16 | Kind::Pc => (0..=65535).contains(&value),
            Kind::I32 => (i32::MIN as i64..=i32::MAX as i64).contains(&value),
        };
        if !fits {
            return Err(format!("{} operand {value} exceeds its {} field", code.name, kind.label()));
        }
        match kind {
            Kind::U8 => out.push(value as u8),
            Kind::I8 => out.push(value as i8 as u8),
            Kind::U16 | Kind::Pc => out.extend_from_slice(&(value as u16).to_be_bytes()),
            Kind::I32 => out.extend_from_slice(&(value as i32).to_be_bytes()),
        }
    }
    Ok(())
}

/// Lay the states out back to back; each begins with its line and ends with
/// a terminator. Returns the code and every state's byte offset.
pub fn assemble(states: &[(i64, Vec<Op>)]) -> Result<(Vec<u8>, Vec<usize>), String> {
    let line = opcode("line");
    let mut offsets = Vec::with_capacity(states.len());
    let mut position = 0usize;
    for (_, ops) in states {
        if ops.last().map_or(true, |last| !is_terminator(last.code)) {
            return Err("bytecode state without a terminator".into());
        }
        offsets.push(position);
        position += 1 + line.kinds.iter().map(|k| k.width()).sum::<usize>();
        for op in ops {
            position += 1 + op.code.kinds.iter().map(|k| k.width()).sum::<usize>();
        }
    }
    if position > 65535 {
        return Err("handler bytecode exceeds 64 KiB".into());
    }
    let mut code = Vec::with_capacity(position);
    for (line_number, ops) in states {
        encode_op(line, &[*line_number], &mut code)?;
        for op in ops {
            let resolved: Vec<i64> = op
                .code
                .kinds
                .iter()
                .zip(&op.args)
                .map(|(kind, &value)| if *kind == Kind::Pc { offsets[value as usize] as i64 } else { value })
                .collect();
            encode_op(op.code, &resolved, &mut code)?;
        }
    }
    Ok((code, offsets))
}

/// (offset, opcode, operands) per op, pc operands as byte offsets.
pub fn disassemble(code: &[u8]) -> Result<Vec<(usize, &'static Opcode, Vec<i64>)>, String> {
    let mut ops = Vec::new();
    let mut pc = 0usize;
    while pc < code.len() {
        let start = pc;
        let op = OPCODES
            .iter()
            .find(|op| op.number == code[pc])
            .ok_or_else(|| format!("invalid opcode {} at {pc}", code[pc]))?;
        pc += 1;
        let mut operands = Vec::new();
        for kind in op.kinds {
            let width = kind.width();
            if pc + width > code.len() {
                return Err(format!("truncated {} at {start}", op.name));
            }
            let bytes = &code[pc..pc + width];
            operands.push(match kind {
                Kind::U8 => bytes[0] as i64,
                Kind::I8 => bytes[0] as i8 as i64,
                Kind::U16 | Kind::Pc => u16::from_be_bytes([bytes[0], bytes[1]]) as i64,
                Kind::I32 => i32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]) as i64,
            });
            pc += width;
        }
        ops.push((start, op, operands));
    }
    Ok(ops)
}

pub fn listing(code: &[u8]) -> Result<String, String> {
    let mut out = String::new();
    for (offset, op, operands) in disassemble(code)? {
        out.push_str(&format!("{offset:6}: {}", op.name.to_uppercase()));
        match op.name {
            "binary" => out.push_str(&format!(" {}", BINARY_OPERATORS[operands[0] as usize])),
            "unary" => out.push_str(&format!(" {}", UNARY_OPERATORS[operands[0] as usize])),
            _ => {
                for v in &operands {
                    out.push_str(&format!(" {v}"));
                }
            }
        }
        out.push('\n');
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn header() -> String {
        let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../runtime/lingo/lingo_bytecode.h");
        std::fs::read_to_string(path).expect("runtime/lingo/lingo_bytecode.h")
    }

    /// One table on each side of the contract: every opcode number and every
    /// operator id here is the runtime's.
    #[test]
    fn opcode_table_matches_the_runtime_header() {
        let text = header();
        let ops = text.split("} lb_op_t;").next().unwrap();
        let mut found = 0;
        for line in ops.lines() {
            let Some(rest) = line.trim().strip_prefix("LB_") else { continue };
            let Some((name, value)) = rest.split_once(" = ") else { continue };
            let value = value.split("//").next().unwrap().trim().trim_end_matches(',');
            let number: u8 = value.trim().parse().unwrap();
            let name = name.to_lowercase();
            let op = OPCODES.iter().find(|op| op.name == name).unwrap_or_else(|| panic!("header opcode {name}"));
            assert_eq!(op.number, number, "{name}");
            found += 1;
        }
        assert_eq!(found, OPCODES.len());
        assert!(OPCODES.iter().all(|op| is_terminator(op) == (op.number >= 40)));
    }

    #[test]
    fn operator_tables_match_the_runtime_header() {
        let text = header();
        for (prefix, table) in [("LB_BIN_", BINARY_OPERATORS), ("LB_UN_", UNARY_OPERATORS)] {
            let mut count = 0;
            for line in text.lines() {
                let Some(rest) = line.trim().strip_prefix(prefix) else { continue };
                let Some((_, rest)) = rest.split_once(" = ") else { continue };
                let Some((value, comment)) = rest.split_once("//") else { continue };
                let id: usize = value.trim().trim_end_matches(',').parse().unwrap();
                assert_eq!(table[id], comment.trim(), "{prefix}{id}");
                count += 1;
            }
            assert_eq!(count, table.len());
        }
    }

    /// Every encoded op reads back as itself, and a wide operand is refused.
    #[test]
    fn encoding_round_trips_and_ranges_are_enforced() {
        let states = vec![
            (1, vec![op("num8", vec![-5]), op("num32", vec![70000]), op("local", vec![255]), op("jump", vec![1])]),
            (2, vec![op("text", vec![65535]), op("list", vec![3, 1]), op("invoke", vec![7, 2, 0])]),
        ];
        let (code, offsets) = assemble(&states).unwrap();
        assert_eq!(offsets.len(), 2);
        let text = listing(&code).unwrap();
        for expected in ["NUM8 -5", "NUM32 70000", "LOCAL 255", "TEXT 65535", "LIST 3 1", "INVOKE 7 2"] {
            assert!(text.contains(expected), "{expected} in {text}");
        }
        assert!(assemble(&[(1, vec![op("local", vec![256]), op("return", vec![])])]).unwrap_err().contains("u8"));
        assert!(assemble(&[(1, vec![op("void", vec![])])]).unwrap_err().contains("terminator"));
    }
}

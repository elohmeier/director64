//! Strict source AST of recovered Lingo scripts: the Rust port of
//! src/director64/lingo.py. It writes the same program.json byte for byte
//! (tests/test_lingo_rust.py compares the two over the synthetic suite and
//! `director64-aot lingo` over every recovered corpus), so the browser can
//! parse scripts without Python.
//!
//! Unsupported syntax is an error with movie/member/line identity, never an
//! ignored statement.

use std::collections::BTreeMap;

use serde_json::{json, Map, Value};
use sha2::{Digest, Sha256};

use crate::pyfmt::float_repr;

pub type Result<T> = std::result::Result<T, String>;

// ---- Python string semantics the reference relies on ----

/// `str.isspace` for the characters recovered text can contain.
fn is_space(c: char) -> bool {
    c.is_whitespace() || matches!(c, '\u{1c}'..='\u{1f}')
}
/// `\w` in a Python str pattern: Unicode letters and digits, and underscore.
fn is_word(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}
fn is_latin1_high(c: char) -> bool {
    ('\u{80}'..='\u{ff}').contains(&c)
}
/// `\d`: a Unicode decimal digit.
fn is_digit(c: char) -> bool {
    char_decimal(c).is_some()
}
/// Decimal value of a Unicode decimal digit (Python's `int()` accepts them).
fn char_decimal(c: char) -> Option<u32> {
    if let Some(d) = c.to_digit(10) {
        return Some(d);
    }
    // Unicode Nd blocks are runs of ten starting at a code point ending in 0.
    const ZEROS: &[u32] = &[
        0x660, 0x6F0, 0x7C0, 0x966, 0x9E6, 0xA66, 0xAE6, 0xB66, 0xBE6, 0xC66, 0xCE6, 0xD66, 0xDE6, 0xE50,
        0xED0, 0xF20, 0x1040, 0x1090, 0x17E0, 0x1810, 0xFF10,
    ];
    let code = c as u32;
    ZEROS.iter().find(|&&z| code >= z && code < z + 10).map(|&z| code - z)
}
fn rstrip(s: &str) -> &str {
    s.trim_end_matches(is_space)
}
fn strip(s: &str) -> &str {
    s.trim_matches(is_space)
}
fn lower(s: &str) -> String {
    s.to_lowercase()
}
/// `str.splitlines()`.
pub fn splitlines(text: &str) -> Vec<&str> {
    let mut lines = Vec::new();
    let mut start = 0;
    let chars: Vec<(usize, char)> = text.char_indices().collect();
    let mut i = 0;
    while i < chars.len() {
        let (at, c) = chars[i];
        let breaks = matches!(c, '\n' | '\r' | '\u{b}' | '\u{c}' | '\u{1c}' | '\u{1d}' | '\u{1e}' | '\u{85}' | '\u{2028}' | '\u{2029}');
        if breaks {
            lines.push(&text[start..at]);
            let mut next = at + c.len_utf8();
            if c == '\r' && i + 1 < chars.len() && chars[i + 1].1 == '\n' {
                next += 1;
                i += 1;
            }
            start = next;
        }
        i += 1;
    }
    if start < text.len() {
        lines.push(&text[start..]);
    }
    lines
}
/// `re.fullmatch(r"[a-z_\x80-\xff][\w\x80-\xff]*", token)`.
fn is_name(token: &str) -> bool {
    let mut chars = token.chars();
    match chars.next() {
        Some(c) if c.is_ascii_lowercase() || c == '_' || is_latin1_high(c) => {}
        _ => return false,
    }
    chars.all(|c| is_word(c) || is_latin1_high(c))
}

// ---- Tokens ----

/// The reference's TOKEN pattern at one offset, after its leading `\s*`.
fn match_token(chars: &[char], mut at: usize) -> Option<(usize, usize)> {
    while at < chars.len() && is_space(chars[at]) {
        at += 1;
    }
    let start = at;
    let c = *chars.get(at)?;
    // "..." without newlines
    if c == '"' {
        let mut end = at + 1;
        while end < chars.len() && chars[end] != '"' && chars[end] != '\n' {
            end += 1;
        }
        if end < chars.len() && chars[end] == '"' {
            return Some((start, end + 1));
        }
    }
    // \d+(?:\.\d+)?
    if is_digit(c) {
        let mut end = at;
        while end < chars.len() && is_digit(chars[end]) {
            end += 1;
        }
        if end + 1 < chars.len() && chars[end] == '.' && is_digit(chars[end + 1]) {
            end += 1;
            while end < chars.len() && is_digit(chars[end]) {
                end += 1;
            }
        }
        return Some((start, end));
    }
    // \#(?:[\w\x80-\xff][\w.\x80-\xff]*|[\\:])
    if c == '#' {
        if let Some(&n) = chars.get(at + 1) {
            if is_word(n) || is_latin1_high(n) {
                let mut end = at + 2;
                while end < chars.len() && (is_word(chars[end]) || chars[end] == '.' || is_latin1_high(chars[end])) {
                    end += 1;
                }
                return Some((start, end));
            }
            if n == '\\' || n == ':' {
                return Some((start, at + 2));
            }
        }
    }
    // [A-Za-z_\x80-\xff][\w\x80-\xff]*
    if c.is_ascii_alphabetic() || c == '_' || is_latin1_high(c) {
        let mut end = at + 1;
        while end < chars.len() && (is_word(chars[end]) || is_latin1_high(chars[end])) {
            end += 1;
        }
        return Some((start, end));
    }
    let two: String = chars[at..(at + 2).min(chars.len())].iter().collect();
    if matches!(two.as_str(), ".." | "<>" | "<=" | ">=" | "&&") {
        return Some((start, at + 2));
    }
    if "+*/%^=&<>,()[]:.-".contains(c) {
        return Some((start, at + 1));
    }
    None
}

pub fn tokens(text: &str) -> Result<Vec<String>> {
    let chars: Vec<char> = text.chars().collect();
    let limit = rstrip(text).chars().count();
    let mut result = Vec::new();
    let mut offset = 0;
    while offset < limit {
        let Some((start, end)) = match_token(&chars, offset) else {
            let near: String = chars[offset..(offset + 50).min(chars.len())].iter().collect();
            return Err(format!("unrecognized token near {}", py_repr(&near)));
        };
        let token: String = chars[start..end].iter().collect();
        let rest: String = chars[end..].iter().collect();
        // ProjectorRays CallNode writes the zero-argument pi() function as PI.
        if token == "PI" && !rest.trim_start_matches(is_space).starts_with('(') {
            result.extend(["pi".to_string(), "(".to_string(), ")".to_string()]);
        } else if token.starts_with('"') || token.starts_with('#') {
            result.push(token);
        } else {
            result.push(lower(&token));
        }
        offset = end;
    }
    Ok(result)
}

/// Python's `repr` of a str, for messages.
fn py_repr(s: &str) -> String {
    let quote = if s.contains('\'') && !s.contains('"') { '"' } else { '\'' };
    let mut out = String::from(quote);
    for c in s.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c == quote => {
                out.push('\\');
                out.push(c);
            }
            c if (c as u32) < 0x20 || c as u32 == 0x7f => out.push_str(&format!("\\x{:02x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push(quote);
    out
}

fn precedence(op: &str) -> Option<u32> {
    Some(match op {
        "or" => 1,
        "and" => 2,
        "=" | "<>" | "<" | ">" | "<=" | ">=" | "contains" | "starts" | "within" | "intersects" => 3,
        "&" | "&&" => 4,
        "+" | "-" => 5,
        "*" | "/" | "mod" => 6,
        "^" => 7,
        _ => return None,
    })
}

struct Expression {
    tokens: Vec<String>,
    pos: usize,
}

fn number(token: &str) -> Result<Value> {
    if token.contains('.') {
        let ascii: String = token.chars().map(|c| if c == '.' { '.' } else { char::from_digit(char_decimal(c).unwrap_or(0), 10).unwrap() }).collect();
        let value: f64 = ascii.parse().map_err(|_| format!("invalid number {token}"))?;
        Ok(json!(value))
    } else {
        let ascii: String = token.chars().map(|c| char::from_digit(char_decimal(c).unwrap_or(0), 10).unwrap()).collect();
        let value: i64 = ascii.parse().map_err(|_| format!("number {token} exceeds 64 bits"))?;
        Ok(json!(value))
    }
}

impl Expression {
    fn new(text: &str) -> Result<Self> {
        Ok(Self { tokens: tokens(text)?, pos: 0 })
    }
    fn peek(&self) -> &str {
        self.tokens.get(self.pos).map(String::as_str).unwrap_or("")
    }
    fn take(&mut self, expected: Option<&str>) -> Result<String> {
        let value = self.peek().to_string();
        if value.is_empty() || expected.is_some_and(|e| value != e) {
            return Err(format!(
                "expected {}, got {} at token {}",
                expected.map(py_repr).unwrap_or("None".into()),
                py_repr(&value),
                self.pos
            ));
        }
        self.pos += 1;
        Ok(value)
    }
    fn args(&mut self, end: &str) -> Result<Vec<Value>> {
        let mut result = Vec::new();
        if self.peek() != end {
            loop {
                result.push(self.parse(0)?);
                if self.peek() != "," {
                    break;
                }
                self.take(Some(","))?;
            }
        }
        self.take(Some(end))?;
        Ok(result)
    }
    fn parse(&mut self, minimum: u32) -> Result<Value> {
        let token = self.take(None)?;
        let mut value: Value;
        let first = token.chars().next().unwrap_or(' ');
        if token == "-" || token == "+" || token == "not" {
            let operand = self.parse(if token == "not" { 3 } else { 7 })?;
            value = json!(["unary", token, operand]);
        } else if token.starts_with('"') {
            value = json!(["string", &token[1..token.len() - 1]]);
        } else if is_digit(first) {
            value = json!(["number", number(&token)?]);
        } else if let Some(symbol) = token.strip_prefix('#') {
            value = json!(["symbol", symbol]);
        } else if token == "(" {
            value = self.parse(0)?;
            self.take(Some(")"))?;
        } else if token == "[" {
            if self.peek() == ":" {
                self.take(Some(":"))?;
                self.take(Some("]"))?;
                value = json!(["proplist", []]);
            } else if self.peek() == "]" {
                self.take(Some("]"))?;
                value = json!(["list", []]);
            } else {
                let mut first = self.parse(0)?;
                if self.peek() == ":" {
                    let mut pairs = Vec::new();
                    loop {
                        self.take(Some(":"))?;
                        let second = self.parse(0)?;
                        pairs.push(json!([first, second]));
                        if self.peek() != "," {
                            break;
                        }
                        self.take(Some(","))?;
                        first = self.parse(0)?;
                    }
                    self.take(Some("]"))?;
                    value = json!(["proplist", pairs]);
                } else {
                    let mut items = vec![first];
                    while self.peek() == "," {
                        self.take(Some(","))?;
                        items.push(self.parse(0)?);
                    }
                    self.take(Some("]"))?;
                    value = json!(["list", items]);
                }
            }
        } else if token == "the" {
            let prop = self.take(None)?;
            if matches!(prop.as_str(), "long" | "short" | "abbreviated" | "abbrev" | "abbr")
                && matches!(self.peek(), "date" | "time")
            {
                let unit = self.take(None)?;
                value = json!(["the", prop + &unit]);
            } else if prop == "number" {
                self.take(Some("of"))?;
                let unit = self.take(None)?;
                if self.peek() == "in" {
                    self.take(Some("in"))?;
                    value = json!(["call", format!("count_{unit}"), [self.parse(7)?]]);
                } else if matches!(unit.as_str(), "member" | "cast" | "field" | "sprite" | "sound") {
                    let owner = self.parse(7)?;
                    value = json!(["property", "number", unit, owner]);
                    if self.peek() == "of" {
                        self.take(Some("of"))?;
                        self.take(Some("castlib"))?;
                        let lib = self.parse(7)?;
                        value = json!(["get", "number", ["reference", unit, value[3].clone(), lib]]);
                    }
                } else if unit == "castmembers" && self.peek() == "of" {
                    self.take(Some("of"))?;
                    value = json!(["get", "numberofmembers", self.parse(7)?]);
                } else if matches!(unit.as_str(), "castmembers" | "castlibs" | "xtras" | "windows" | "sprites") {
                    value = json!(["the", format!("numberof{unit}")]);
                } else {
                    self.pos -= 1;
                    value = json!(["get", "number", self.parse(7)?]);
                }
            } else if prop == "last" {
                let unit = self.take(None)?;
                if !matches!(unit.as_str(), "char" | "word" | "item" | "line") {
                    return Err("invalid last chunk unit".into());
                }
                if !matches!(self.take(None)?.as_str(), "in" | "of") {
                    return Err("last chunk requires in/of".into());
                }
                value = json!(["last_chunk", unit, self.parse(7)?]);
            } else if self.peek() == "of" {
                self.take(Some("of"))?;
                let owner = self.peek().to_string();
                if matches!(owner.as_str(), "sprite" | "cast" | "member" | "field" | "sound") {
                    self.take(None)?;
                    let target = self.parse(7)?;
                    value = json!(["property", prop, owner, target]);
                    if self.peek() == "of" {
                        self.take(Some("of"))?;
                        self.take(Some("castlib"))?;
                        let lib = self.parse(7)?;
                        value = json!(["get", prop, ["reference", owner, value[3].clone(), lib]]);
                    }
                } else {
                    value = json!(["get", prop, self.parse(7)?]);
                }
            } else {
                value = json!(["the", prop]);
            }
        } else if matches!(token.as_str(), "script" | "window" | "castlib") && !self.peek().is_empty() && self.peek() != "(" {
            value = json!(["call", token, [self.parse(7)?]]);
        } else if matches!(token.as_str(), "member" | "cast" | "field" | "sprite") && self.peek() == "(" {
            self.take(Some("("))?;
            let args = self.args(")")?;
            if !(args.len() == 1 || args.len() == 2) || (token == "sprite" && args.len() != 1) {
                return Err("invalid reference arguments".into());
            }
            let second = if args.len() == 2 { args[1].clone() } else { Value::Null };
            value = json!(["reference", token, args[0].clone(), second]);
            if self.peek() == "of" {
                if args.len() == 2 {
                    return Err("duplicate cast library argument".into());
                }
                self.take(Some("of"))?;
                self.take(Some("castlib"))?;
                value[3] = self.parse(7)?;
            }
        } else if matches!(token.as_str(), "member" | "cast" | "field" | "sprite")
            && !self.peek().is_empty()
            && precedence(self.peek()).is_none()
            && !matches!(self.peek(), "(" | "," | ")" | "]" | ":" | ".")
        {
            let target = self.parse(7)?;
            value = json!(["reference", token, target, null]);
            if self.peek() == "of" {
                self.take(Some("of"))?;
                self.take(Some("castlib"))?;
                value[3] = self.parse(7)?;
            }
        } else if matches!(token.as_str(), "char" | "word" | "item" | "line")
            && !self.peek().is_empty()
            && !matches!(self.peek(), "." | "[" | "," | ")" | "]" | ":" | "=")
        {
            let first = self.parse(7)?;
            let mut last = first.clone();
            if self.peek() == "to" {
                self.take(Some("to"))?;
                last = self.parse(7)?;
            }
            self.take(Some("of"))?;
            let target = self.parse(7)?;
            value = json!(["chunk", token, first, last, target]);
        } else if is_name(&token) {
            if self.peek() == "(" {
                self.take(Some("("))?;
                value = json!(["call", token, self.args(")")?]);
            } else {
                value = json!(["variable", token]);
            }
        } else {
            return Err(format!("unexpected expression token {}", py_repr(&token)));
        }
        while self.peek() == "." || self.peek() == "[" {
            if self.take(None)? == "[" {
                let index = self.parse(0)?;
                value = json!(["index", value, index]);
                self.take(Some("]"))?;
                continue;
            }
            let prop = self.take(None)?;
            if !is_name(&prop) {
                return Err("invalid dot property".into());
            }
            if self.peek() == "(" {
                self.take(Some("("))?;
                value = json!(["method", prop, value, self.args(")")?]);
            } else if matches!(prop.as_str(), "char" | "word" | "item" | "line") {
                if self.peek() == "." {
                    self.take(Some("."))?;
                    self.take(Some("count"))?;
                    value = json!(["call", format!("count_{prop}s"), [value]]);
                } else {
                    self.take(Some("["))?;
                    let first = self.parse(0)?;
                    let mut last = first.clone();
                    if self.peek() == ".." {
                        self.take(Some(".."))?;
                        last = self.parse(0)?;
                    }
                    self.take(Some("]"))?;
                    value = json!(["chunk", prop, first, last, value]);
                }
            } else {
                value = json!(["get", prop, value]);
            }
        }
        while let Some(p) = precedence(self.peek()) {
            if p < minimum {
                break;
            }
            let op = self.take(None)?;
            if op == "starts" && self.peek() == "with" {
                self.take(Some("with"))?;
            }
            let right = self.parse(p + u32::from(op != "^"))?;
            value = json!(["binary", op, value, right]);
        }
        Ok(value)
    }
    fn complete(mut self) -> Result<Value> {
        let result = self.parse(0)?;
        if !self.peek().is_empty() {
            let rest: Vec<String> = self.tokens[self.pos..].iter().map(|t| py_repr(t)).collect();
            return Err(format!("unexpected trailing expression tokens [{}]", rest.join(", ")));
        }
        Ok(result)
    }
}

pub fn expression(text: &str) -> Result<Value> {
    Expression::new(text)?.complete()
}

/// One statement block from (line, text) pairs, as the reference's
/// `Statements(lines).block()`.
pub fn block(lines: &[(usize, String)]) -> Result<Vec<Value>> {
    Statements { lines, pos: 0 }.block(&[])
}

/// Splits at a top-level keyword outside strings, brackets and parentheses.
fn split_keyword<'a>(text: &'a str, keyword: &str) -> Result<(&'a str, &'a str)> {
    let mut depth: i64 = 0;
    let mut quoted = false;
    let marker = format!(" {keyword} ");
    let marker_chars = marker.chars().count();
    let chars: Vec<(usize, char)> = text.char_indices().collect();
    for (i, &(at, c)) in chars.iter().enumerate() {
        if c == '"' {
            quoted = !quoted;
        }
        if quoted {
            continue;
        }
        if c == '(' || c == '[' {
            depth += 1;
        }
        if c == ')' || c == ']' {
            depth -= 1;
        }
        if depth == 0 {
            let end = chars.get(i + marker_chars).map(|&(e, _)| e).unwrap_or(text.len());
            if i + marker_chars <= chars.len() && lower(&text[at..end]) == marker {
                return Ok((&text[..at], &text[end..]));
            }
        }
    }
    Err(format!("missing top-level {} in {}", py_repr(keyword), py_repr(text)))
}

// ---- Statements ----

/// A handler's shared script context: Python's handlers alias the context's
/// lists, so declarations after a handler still show up in it.
struct Context {
    cast: String,
    member: i64,
    script_type: String,
    properties: Vec<String>,
    script_globals: Vec<String>,
}

struct Statements<'a> {
    lines: &'a [(usize, String)],
    pos: usize,
}

fn node(line: usize) -> Map<String, Value> {
    let mut map = Map::new();
    map.insert("line".into(), json!(line));
    map
}

impl<'a> Statements<'a> {
    fn current(&self) -> String {
        self.lines.get(self.pos).map(|(_, t)| lower(t)).unwrap_or_default()
    }
    fn end(&mut self, expected: &str) -> Result<()> {
        if self.current() != expected {
            return Err(format!("missing {expected}"));
        }
        self.pos += 1;
        Ok(())
    }
    fn parse_list(text: &str, what: &str) -> Result<Vec<Value>> {
        let mut parser = Expression::new(text)?;
        let mut values = vec![parser.parse(0)?];
        while parser.peek() == "," {
            parser.take(Some(","))?;
            values.push(parser.parse(0)?);
        }
        if !parser.peek().is_empty() {
            return Err(what.into());
        }
        Ok(values)
    }
    fn block(&mut self, stops: &[&str]) -> Result<Vec<Value>> {
        let mut result = Vec::new();
        while self.pos < self.lines.len() {
            let (line, text) = self.lines[self.pos].clone();
            let low = lower(&text);
            if stops.contains(&low.as_str()) || (stops.contains(&"case-label") && low.ends_with(':')) {
                break;
            }
            self.pos += 1;
            let mut n = node(line);
            let outcome: Result<bool> = (|| {
                if low.starts_with("global ") {
                    let names: Vec<String> = text[7..].split(',').map(|v| lower(strip(v))).collect();
                    if !names.iter().all(|v| is_name(v)) {
                        return Err("malformed globals".into());
                    }
                    n.insert("op".into(), json!("global"));
                    n.insert("names".into(), json!(names));
                } else if low.starts_with("tell ") {
                    n.insert("op".into(), json!("tell"));
                    n.insert("target".into(), expression(&text[5..])?);
                    let body = self.block(&["end tell"])?;
                    n.insert("body".into(), Value::Array(body));
                    self.end("end tell")?;
                } else if low.starts_with("if ") && low.ends_with(" then") {
                    n.insert("op".into(), json!("if"));
                    n.insert("condition".into(), expression(&text[3..text.len() - 5])?);
                    let yes = self.block(&["else", "end if"])?;
                    n.insert("yes".into(), Value::Array(yes));
                    n.insert("no".into(), json!([]));
                    if self.current() == "else" {
                        self.pos += 1;
                        let no = self.block(&["end if"])?;
                        n.insert("no".into(), Value::Array(no));
                    }
                    self.end("end if")?;
                } else if low.starts_with("repeat while ") {
                    n.insert("op".into(), json!("while"));
                    n.insert("condition".into(), expression(&text[13..])?);
                    let body = self.block(&["end repeat"])?;
                    n.insert("body".into(), Value::Array(body));
                    self.end("end repeat")?;
                } else if low.starts_with("repeat with ") {
                    if repeat_in(&low) {
                        let (var, sequence) = split_keyword(&text[12..], "in")?;
                        n.insert("op".into(), json!("foreach"));
                        n.insert("variable".into(), json!(lower(var)));
                        n.insert("sequence".into(), expression(sequence)?);
                        let body = self.block(&["end repeat"])?;
                        n.insert("body".into(), Value::Array(body));
                        self.end("end repeat")?;
                        return Ok(true);
                    }
                    let replaced = text[12..].replacen(" = ", " to ", 1);
                    let (var, bounds) = split_keyword(&replaced, "to")?;
                    let descending = lower(bounds).contains(" down to ");
                    let (start, end) = split_keyword(bounds, if descending { "down to" } else { "to" })?;
                    let mut chars = var.chars();
                    let valid = chars.next().is_some_and(|c| c.is_ascii_alphabetic() || c == '_') && chars.all(is_word);
                    if !valid {
                        return Err("invalid loop variable".into());
                    }
                    n.insert("op".into(), json!("for"));
                    n.insert("variable".into(), json!(lower(var)));
                    n.insert("first".into(), expression(start)?);
                    n.insert("last".into(), expression(end)?);
                    n.insert("step".into(), json!(if descending { -1 } else { 1 }));
                    let body = self.block(&["end repeat"])?;
                    n.insert("body".into(), Value::Array(body));
                    self.end("end repeat")?;
                } else if low.starts_with("case ") && low.ends_with(" of") {
                    let mut branches = Vec::new();
                    while self.current() != "end case" {
                        if !self.current().ends_with(':') {
                            return Err("expected case label".into());
                        }
                        let raw = &self.lines[self.pos].1;
                        let label = &raw[..raw.len() - 1];
                        self.pos += 1;
                        let values = if lower(label) == "otherwise" {
                            Value::Null
                        } else {
                            Value::Array(Self::parse_list(label, "invalid case label")?)
                        };
                        let body = self.block(&["case-label", "end case"])?;
                        branches.push(json!({"values": values, "body": body}));
                    }
                    self.end("end case")?;
                    n.insert("op".into(), json!("case"));
                    n.insert("selector".into(), expression(&text[5..text.len() - 3])?);
                    n.insert("branches".into(), Value::Array(branches));
                } else if matches!(low.as_str(), "exit" | "return" | "exit repeat" | "next repeat") {
                    let op = match low.as_str() {
                        "exit" | "return" => "return",
                        "exit repeat" => "break",
                        _ => "continue",
                    };
                    n.insert("op".into(), json!(op));
                } else if low.starts_with("return ") && !low.starts_with("return =") {
                    n.insert("op".into(), json!("return"));
                    n.insert("value".into(), expression(&text[7..])?);
                } else if low.starts_with("set ") {
                    let (target, value) = split_keyword(&text[4..], "to")?;
                    n.insert("op".into(), json!("set"));
                    n.insert("target".into(), expression(target)?);
                    n.insert("value".into(), expression(value)?);
                } else if low.starts_with("delete ") {
                    n.insert("op".into(), json!("delete"));
                    n.insert("target".into(), expression(&text[7..])?);
                } else if low.starts_with("put ") {
                    let mut append = None;
                    for direction in ["after", "before"] {
                        let Ok((value, target)) = split_keyword(&text[4..], direction) else { continue };
                        append = Some((direction, expression(value)?, expression(target)?));
                        break;
                    }
                    if let Some((direction, value, target)) = append {
                        let (left, right) = if direction == "after" {
                            (target.clone(), value)
                        } else {
                            (value, target.clone())
                        };
                        n.insert("op".into(), json!("set"));
                        n.insert("target".into(), target);
                        n.insert("value".into(), json!(["binary", "&", left, right]));
                        return Ok(true);
                    }
                    match split_keyword(&text[4..], "into") {
                        Ok((value, target)) => {
                            n.insert("op".into(), json!("set"));
                            n.insert("target".into(), expression(target)?);
                            n.insert("value".into(), expression(value)?);
                        }
                        Err(_) => {
                            let values = Self::parse_list(&text[4..], "invalid trace expression")?;
                            n.insert("op".into(), json!("trace"));
                            n.insert("value".into(), json!(["list", values]));
                        }
                    }
                } else if low.starts_with("play movie ") {
                    n.insert("op".into(), json!("call"));
                    n.insert("value".into(), json!(["call", "play_movie", [expression(&text[11..])?]]));
                } else if low == "play done" {
                    n.insert("op".into(), json!("call"));
                    n.insert("value".into(), json!(["call", "play_done", []]));
                } else if low.starts_with("sound ") {
                    let (command, operands) = text[6..].split_once(' ').unwrap_or((&text[6..], ""));
                    let args = Self::parse_list(operands, "invalid sound command")?;
                    n.insert("op".into(), json!("call"));
                    n.insert("value".into(), json!(["call", format!("sound_{}", lower(command)), args]));
                } else if low.starts_with("when mouseup then ") {
                    n.insert("op".into(), json!("set"));
                    n.insert("target".into(), json!(["the", "mouseupscript"]));
                    n.insert("value".into(), json!(["string", &text[18..]]));
                } else if low.starts_with("hilite ") {
                    n.insert("op".into(), json!("call"));
                    n.insert("value".into(), json!(["call", "hilite_chunk", [expression(&text[7..])?]]));
                } else if let Some(opcode) = unrecovered(&low) {
                    n.insert("op".into(), json!("unrecovered"));
                    n.insert("opcode".into(), json!(opcode));
                } else {
                    match split_keyword(&text, "=") {
                        Err(_) => {
                            let value = expression(&text)?;
                            if !matches!(value[0].as_str(), Some("call" | "method")) {
                                return Err("statement is not a call".into());
                            }
                            n.insert("op".into(), json!("call"));
                            n.insert("value".into(), value);
                        }
                        Ok((target, value)) => {
                            n.insert("op".into(), json!("set"));
                            n.insert("target".into(), expression(target)?);
                            n.insert("value".into(), expression(value)?);
                        }
                    }
                }
                Ok(false)
            })();
            let appended = outcome.map_err(|e| format!("line {line}: {text}: {e}"))?;
            if !appended
                && n["op"] == "set"
                && !matches!(
                    n["target"][0].as_str(),
                    Some("variable" | "the" | "property" | "reference" | "chunk" | "get" | "index")
                )
            {
                return Err(format!("line {line}: invalid assignment target"));
            }
            result.push(Value::Object(n));
        }
        Ok(result)
    }
}

/// `re.match(r"repeat with \w+ in ", low)`.
fn repeat_in(low: &str) -> bool {
    let Some(rest) = low.strip_prefix("repeat with ") else { return false };
    let word: usize = rest.chars().take_while(|&c| is_word(c)).map(char::len_utf8).sum();
    word > 0 && rest[word..].starts_with(" in ")
}
/// `re.fullmatch(r"-- (unk[0-9a-f]+)", low)`.
fn unrecovered(low: &str) -> Option<&str> {
    let opcode = low.strip_prefix("-- ")?;
    let hex = opcode.strip_prefix("unk")?;
    (!hex.is_empty() && hex.chars().all(|c| c.is_ascii_digit() || ('a'..='f').contains(&c))).then_some(opcode)
}
/// `re.fullmatch(r"-- cast: (.*); member: (\d+); type: (\w+); name: (.*)", text)`.
fn script_header(text: &str) -> Option<(String, i64, String)> {
    let rest = text.strip_prefix("-- cast: ")?;
    if rest.contains('\n') {
        return None;
    }
    // Greedy `(.*)`: the last "; member: " that lets the rest match.
    let mut candidates: Vec<usize> = rest.match_indices("; member: ").map(|(i, _)| i).collect();
    candidates.reverse();
    for at in candidates {
        let cast = &rest[..at];
        let after = &rest[at + "; member: ".len()..];
        let digits: usize = after.chars().take_while(|c| is_digit(*c)).map(char::len_utf8).sum();
        if digits == 0 {
            continue;
        }
        let Some(after) = after[digits..].strip_prefix("; type: ") else { continue };
        let word: usize = after.chars().take_while(|&c| is_word(c)).map(char::len_utf8).sum();
        if word == 0 || !after[word..].starts_with("; name: ") {
            continue;
        }
        let member: String = rest[at + "; member: ".len()..][..digits]
            .chars()
            .map(|c| char::from_digit(char_decimal(c).unwrap_or(0), 10).unwrap())
            .collect();
        return Some((cast.to_string(), member.parse().ok()?, after[..word].to_string()));
    }
    None
}

struct Parsed {
    context: usize,
    fields: Map<String, Value>,
    body: Vec<Value>,
    globals_first: bool,
}

/// Parses one recovered movie's scripts.
pub fn parse_movie(source: &str, movie: &str) -> Result<Vec<Value>> {
    let mut contexts: Vec<Context> = Vec::new();
    let mut handlers: Vec<Parsed> = Vec::new();
    let mut current: Option<(usize, Map<String, Value>)> = None;
    let mut body: Vec<(usize, String)> = Vec::new();
    let lines = splitlines(source);
    for (index, text) in lines.iter().enumerate() {
        let lineno = index + 1;
        if let Some((cast, member, script_type)) = script_header(text) {
            if current.is_some() {
                return Err(format!("{movie}:{lineno}: unterminated handler"));
            }
            contexts.push(Context { cast, member, script_type, properties: Vec::new(), script_globals: Vec::new() });
        } else if current.is_none() && (lower(text).starts_with("global ") || lower(text).starts_with("property ")) {
            let Some(context) = contexts.last_mut() else {
                return Err(format!("{movie}:{lineno}: declaration outside script"));
            };
            let (declaration, names) = text.split_once(' ').unwrap();
            let property = lower(declaration) == "property";
            let names: Vec<String> = names
                .split(',')
                .map(|name| if property { strip(name).to_string() } else { lower(strip(name)) })
                .collect();
            let valid = names.iter().all(|name| {
                let mut chars = name.chars();
                chars.next().is_some_and(|c| c.is_ascii_alphabetic() || c == '_' || is_latin1_high(c))
                    && chars.all(|c| is_word(c) || is_latin1_high(c))
            });
            if !valid {
                return Err(format!("{movie}:{lineno}: invalid script declaration"));
            }
            if property {
                context.properties.extend(names);
            } else {
                context.script_globals.extend(names);
            }
        } else if let Some(rest) = text.strip_prefix("on ") {
            if current.is_some() || contexts.is_empty() {
                return Err(format!("{movie}:{lineno}: invalid handler"));
            }
            let (name, params) = rest.split_once(' ').unwrap_or((rest, ""));
            let mut fields = Map::new();
            fields.insert("movie".into(), json!(movie));
            fields.insert("name".into(), json!(lower(name)));
            fields.insert("line".into(), json!(lineno));
            let parameters: Vec<String> =
                params.split(',').filter(|p| !strip(p).is_empty()).map(|p| lower(strip(p))).collect();
            fields.insert("parameters".into(), json!(parameters));
            current = Some((contexts.len() - 1, fields));
            body.clear();
        } else if *text == "end" {
            let Some((context, mut fields)) = current.take() else {
                return Err(format!("{movie}:{lineno}: stray end"));
            };
            let member = contexts[context].member;
            let handler_name = fields["name"].as_str().unwrap_or("").to_string();
            let parsed = Statements { lines: &body, pos: 0 }
                .block(&[])
                .map_err(|e| format!("{movie} member {member} {handler_name}: {e}"))?;
            let globals_first = !contexts[context].script_globals.is_empty();
            let start = fields["line"].as_u64().unwrap() as usize - 1;
            let text = lines[start..lineno].join("\n");
            fields.insert("source_sha256".into(), json!(hex(&Sha256::digest(text.as_bytes()))));
            handlers.push(Parsed { context, fields, body: parsed, globals_first });
        } else if !strip(text).is_empty() && !text.starts_with("--") {
            if current.is_none() {
                return Err(format!("{movie}:{lineno}: text outside handler"));
            }
            body.push((lineno, strip(text).to_string()));
        }
    }
    if current.is_some() {
        return Err(format!("{movie}: truncated handler"));
    }
    // Materialize with each script's final declaration lists (the reference's
    // handlers alias them).
    Ok(handlers
        .into_iter()
        .map(|h| {
            let c = &contexts[h.context];
            let mut out = Map::new();
            out.insert("cast".into(), json!(c.cast));
            out.insert("member".into(), json!(c.member));
            out.insert("script_type".into(), json!(c.script_type));
            out.insert("properties".into(), json!(c.properties));
            out.insert("script_globals".into(), json!(c.script_globals));
            out.insert("movie".into(), h.fields["movie"].clone());
            out.insert("name".into(), h.fields["name"].clone());
            out.insert("line".into(), h.fields["line"].clone());
            out.insert("parameters".into(), h.fields["parameters"].clone());
            let mut body = h.body;
            if h.globals_first {
                // The reference writes this node's keys as op, line, names.
                let mut global = Map::new();
                global.insert("op".into(), json!("global"));
                global.insert("line".into(), h.fields["line"].clone());
                global.insert("names".into(), json!(c.script_globals));
                body.insert(0, Value::Object(global));
            }
            out.insert("body".into(), Value::Array(body));
            out.insert("source_sha256".into(), h.fields["source_sha256"].clone());
            Value::Object(out)
        })
        .collect())
}

fn hex(digest: &[u8]) -> String {
    digest.iter().map(|b| format!("{b:02x}")).collect()
}

fn walk_calls(value: &Value, calls: &mut BTreeMap<String, u64>) {
    match value {
        Value::Array(items) => {
            if items.len() == 3 && items[0] == "call" {
                if let Some(name) = items[1].as_str() {
                    *calls.entry(name.to_string()).or_default() += 1;
                }
            }
            for item in items {
                walk_calls(item, calls);
            }
        }
        Value::Object(map) => {
            for item in map.values() {
                walk_calls(item, calls);
            }
        }
        _ => {}
    }
}

/// A recovered script file as read from disk: universal newlines, as
/// Python's `read_text` gives them.
pub fn normalize_newlines(raw: &str) -> String {
    raw.replace("\r\n", "\n").replace('\r', "\n")
}

/// The whole program from (file name, text) pairs sorted by name, as
/// `python -m director64.lingo` writes it.
pub fn program(files: &[(String, String)]) -> Result<Value> {
    let mut handlers = Vec::new();
    let mut entries = Vec::new();
    for (name, source) in files {
        let parsed = parse_movie(source, name.strip_suffix(".lingo").unwrap_or(name))?;
        entries.push(json!({
            "name": name,
            "sha256": hex(&Sha256::digest(source.as_bytes())),
            "handlers": parsed.len(),
        }));
        handlers.extend(parsed);
    }
    if entries.is_empty() {
        return Err("no recovered scripts".into());
    }
    let mut calls = BTreeMap::new();
    for handler in &handlers {
        walk_calls(handler, &mut calls);
    }
    let calls: Map<String, Value> = calls.into_iter().map(|(k, v)| (k, json!(v))).collect();
    Ok(json!({
        "schema_version": 1,
        "files": entries,
        "handlers": handlers,
        "calls": calls,
        "native_execution_verified": false,
    }))
}

/// `json.dumps(value, separators=(",", ":"), ensure_ascii=True)`.
pub fn dumps_compact(value: &Value) -> String {
    let mut out = String::new();
    compact(value, &mut out);
    out
}

fn compact(value: &Value, out: &mut String) {
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
        Value::String(s) => {
            out.push('"');
            for c in s.chars() {
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
                            out.push_str(&format!("\\u{unit:04x}"));
                        }
                    }
                    c => out.push(c),
                }
            }
            out.push('"');
        }
        Value::Array(items) => {
            out.push('[');
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                compact(item, out);
            }
            out.push(']');
        }
        Value::Object(map) => {
            out.push('{');
            for (i, (key, item)) in map.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                compact(&Value::String(key.clone()), out);
                out.push(':');
                compact(item, out);
            }
            out.push('}');
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn splitlines_matches_python() {
        assert_eq!(splitlines("a\nb\r\nc\rd"), ["a", "b", "c", "d"]);
        assert_eq!(splitlines("a\n"), ["a"]);
        assert_eq!(splitlines("a\n\nb"), ["a", "", "b"]);
        assert!(splitlines("").is_empty());
    }

    #[test]
    fn tokens_fold_names_and_keep_literals() {
        assert_eq!(tokens("Put #Sym & \"Text\" into X").unwrap(), ["put", "#Sym", "&", "\"Text\"", "into", "x"]);
        assert_eq!(tokens("set x to PI").unwrap(), ["set", "x", "to", "pi", "(", ")"]);
        assert_eq!(tokens("1.5 .. 2").unwrap(), ["1.5", "..", "2"]);
        assert!(tokens("a @ b").is_err());
    }

    #[test]
    fn expressions_follow_the_reference_grammar() {
        assert_eq!(expression("1 + 2 * 3").unwrap(), json!(["binary", "+", ["number", 1], ["binary", "*", ["number", 2], ["number", 3]]]));
        assert_eq!(expression("the loch of sprite 5").unwrap(), json!(["property", "loch", "sprite", ["number", 5]]));
        assert_eq!(expression("member(\"a\", 2)").unwrap(), json!(["reference", "member", ["string", "a"], ["number", 2]]));
        assert_eq!(expression("char 1 to 3 of x").unwrap(), json!(["chunk", "char", ["number", 1], ["number", 3], ["variable", "x"]]));
        assert_eq!(expression("[#a: 1.5]").unwrap(), json!(["proplist", [[["symbol", "a"], ["number", 1.5]]]]));
        // `^` is right-associative: its right operand parses at its own level.
        assert_eq!(expression("2 ^ 3 ^ 2").unwrap(), json!(["binary", "^", ["number", 2], ["binary", "^", ["number", 3], ["number", 2]]]));
    }

    #[test]
    fn handlers_share_their_script_declarations() {
        let source = "-- cast: Internal; member: 3; type: MovieScript; name: x\nglobal gA\non go a, B\n  put a into gA\nend\n";
        let handlers = parse_movie(source, "X.DXR").unwrap();
        assert_eq!(handlers[0]["parameters"], json!(["a", "b"]));
        assert_eq!(handlers[0]["body"][0], json!({"line": 3, "op": "global", "names": ["ga"]}));
        assert_eq!(handlers[0]["body"][1]["op"], "set");
        assert!(parse_movie("on x\nend\n", "X").is_err());
    }
}

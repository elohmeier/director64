//! The recovered source AST as the parser writes it to program.json, typed.
//!
//! Expressions are JSON lists headed by their kind; statements are objects
//! with an `op` and a `line`. Anything the compiler does not lower keeps its
//! kind so the error it raises names it, as the Python generator did.

use serde_json::Value;

#[derive(Clone, Debug, PartialEq)]
pub enum Num {
    Int(i64),
    Float(f64),
}

#[derive(Clone, Debug, PartialEq)]
pub enum Expr {
    Number(Num),
    Str(String),
    Sym(String),
    Var(String),
    Unary(String, Box<Expr>),
    Binary(String, Box<Expr>, Box<Expr>),
    List(Vec<Expr>),
    PropList(Vec<(Expr, Expr)>),
    The(String),
    /// `["property", name, kind, value]`: a property of the reference kind(value).
    Property { name: String, kind: String, value: Box<Expr> },
    Get(String, Box<Expr>),
    Index(Box<Expr>, Box<Expr>),
    LastChunk(String, Box<Expr>),
    Method { name: String, receiver: Box<Expr>, args: Vec<Expr> },
    Reference { kind: String, number: Box<Expr>, library: Option<Box<Expr>> },
    /// `["chunk", kind, first, last, source]`.
    Chunk { kind: String, first: Box<Expr>, last: Box<Expr>, source: Box<Expr> },
    Call(String, Vec<Expr>),
    /// A list the parser emitted that the compiler has no lowering for.
    Unknown(String),
}

#[derive(Clone, Debug)]
pub struct Branch {
    pub values: Option<Vec<Expr>>,
    pub body: Vec<Stmt>,
}

#[derive(Clone, Debug)]
pub enum StmtKind {
    Global(Vec<String>),
    If { condition: Expr, yes: Vec<Stmt>, no: Vec<Stmt> },
    While { condition: Expr, body: Vec<Stmt> },
    For { variable: String, first: Expr, last: Expr, step: i64, body: Vec<Stmt> },
    Break,
    Continue,
    Return(Option<Expr>),
    Call(Expr),
    Delete(Expr),
    Set { target: Expr, value: Expr },
    Trace(Expr),
    Tell { target: Expr, body: Vec<Stmt> },
    Unrecovered { opcode: String },
    ForEach { variable: String, sequence: Expr, body: Vec<Stmt> },
    Case { selector: Expr, branches: Vec<Branch> },
    Unknown(String),
}

#[derive(Clone, Debug)]
pub struct Stmt {
    pub line: i64,
    pub kind: StmtKind,
}

#[derive(Clone, Debug)]
pub struct Handler {
    pub name: String,
    pub member: i64,
    pub cast: String,
    pub script_type: String,
    pub parameters: Vec<String>,
    pub properties: Vec<String>,
    pub movie: String,
    pub line: i64,
    pub body: Vec<Stmt>,
}

pub struct Program {
    pub handlers: Vec<Handler>,
    pub files: Vec<String>,
    pub compatibility_fixes: Option<Value>,
}

fn field<'a>(object: &'a serde_json::Map<String, Value>, key: &str) -> Result<&'a Value, String> {
    object.get(key).ok_or_else(|| format!("statement without {key:?}"))
}

fn text(value: &Value, what: &str) -> Result<String, String> {
    value.as_str().map(str::to_string).ok_or_else(|| format!("{what} is not a string"))
}

fn strings(value: &Value, what: &str) -> Result<Vec<String>, String> {
    value
        .as_array()
        .ok_or_else(|| format!("{what} is not a list"))?
        .iter()
        .map(|v| text(v, what))
        .collect()
}

fn exprs(value: &Value) -> Result<Vec<Expr>, String> {
    value.as_array().ok_or("expected a list of expressions")?.iter().map(Expr::from_json).collect()
}

fn stmts(value: &Value) -> Result<Vec<Stmt>, String> {
    value.as_array().ok_or("expected a block")?.iter().map(Stmt::from_json).collect()
}

impl Expr {
    pub fn from_json(value: &Value) -> Result<Expr, String> {
        let items = value.as_array().ok_or_else(|| format!("expression is not a list: {value}"))?;
        let kind = match items.first().and_then(Value::as_str) {
            Some(kind) => kind,
            None => return Ok(Expr::Unknown(String::new())),
        };
        let arg = |i: usize| items.get(i).ok_or_else(|| format!("{kind} expression is short"));
        Ok(match kind {
            "number" => match arg(1)? {
                Value::Number(n) => {
                    if let Some(i) = n.as_i64() {
                        Expr::Number(Num::Int(i))
                    } else if let Some(u) = n.as_u64() {
                        Expr::Number(Num::Float(u as f64))
                    } else {
                        Expr::Number(Num::Float(n.as_f64().ok_or("bad number")?))
                    }
                }
                other => return Err(format!("number literal {other} is not numeric")),
            },
            "string" => Expr::Str(text(arg(1)?, "string literal")?),
            "symbol" => Expr::Sym(text(arg(1)?, "symbol")?),
            "variable" => Expr::Var(text(arg(1)?, "variable")?),
            "unary" => Expr::Unary(text(arg(1)?, "operator")?, Box::new(Expr::from_json(arg(2)?)?)),
            "binary" => Expr::Binary(
                text(arg(1)?, "operator")?,
                Box::new(Expr::from_json(arg(2)?)?),
                Box::new(Expr::from_json(arg(3)?)?),
            ),
            "list" => Expr::List(exprs(arg(1)?)?),
            "proplist" => Expr::PropList(
                arg(1)?
                    .as_array()
                    .ok_or("proplist pairs")?
                    .iter()
                    .map(|pair| {
                        let pair = pair.as_array().ok_or("proplist pair")?;
                        if pair.len() != 2 {
                            return Err("proplist pair".to_string());
                        }
                        Ok((Expr::from_json(&pair[0])?, Expr::from_json(&pair[1])?))
                    })
                    .collect::<Result<_, String>>()?,
            ),
            "the" => Expr::The(text(arg(1)?, "the")?),
            "property" => Expr::Property {
                name: text(arg(1)?, "property")?,
                kind: text(arg(2)?, "property kind")?,
                value: Box::new(Expr::from_json(arg(3)?)?),
            },
            "get" => Expr::Get(text(arg(1)?, "get")?, Box::new(Expr::from_json(arg(2)?)?)),
            "index" => Expr::Index(Box::new(Expr::from_json(arg(1)?)?), Box::new(Expr::from_json(arg(2)?)?)),
            "last_chunk" => Expr::LastChunk(text(arg(1)?, "chunk kind")?, Box::new(Expr::from_json(arg(2)?)?)),
            "method" => Expr::Method {
                name: text(arg(1)?, "method")?,
                receiver: Box::new(Expr::from_json(arg(2)?)?),
                args: exprs(arg(3)?)?,
            },
            "reference" => Expr::Reference {
                kind: text(arg(1)?, "reference kind")?,
                number: Box::new(Expr::from_json(arg(2)?)?),
                library: match items.get(3) {
                    None | Some(Value::Null) => None,
                    Some(Value::Array(a)) if a.is_empty() => None,
                    Some(v) => Some(Box::new(Expr::from_json(v)?)),
                },
            },
            "chunk" => Expr::Chunk {
                kind: text(arg(1)?, "chunk kind")?,
                first: Box::new(Expr::from_json(arg(2)?)?),
                last: Box::new(Expr::from_json(arg(3)?)?),
                source: Box::new(Expr::from_json(arg(4)?)?),
            },
            "call" => Expr::Call(text(arg(1)?, "call")?, exprs(arg(2)?)?),
            other => Expr::Unknown(other.to_string()),
        })
    }
}

impl Stmt {
    pub fn from_json(value: &Value) -> Result<Stmt, String> {
        let object = value.as_object().ok_or_else(|| format!("statement is not an object: {value}"))?;
        let op = text(field(object, "op")?, "op")?;
        let line = field(object, "line")?.as_i64().ok_or("line is not an integer")?;
        let expr = |key: &str| Expr::from_json(field(object, key)?);
        let block = |key: &str| stmts(field(object, key)?);
        let kind = match op.as_str() {
            "global" => StmtKind::Global(strings(field(object, "names")?, "global names")?),
            "if" => StmtKind::If { condition: expr("condition")?, yes: block("yes")?, no: block("no")? },
            "while" => StmtKind::While { condition: expr("condition")?, body: block("body")? },
            "for" => StmtKind::For {
                variable: text(field(object, "variable")?, "loop variable")?,
                first: expr("first")?,
                last: expr("last")?,
                step: field(object, "step")?.as_i64().ok_or("step is not an integer")?,
                body: block("body")?,
            },
            "break" => StmtKind::Break,
            "continue" => StmtKind::Continue,
            "return" => StmtKind::Return(match object.get("value") {
                Some(v) => Some(Expr::from_json(v)?),
                None => None,
            }),
            "call" => StmtKind::Call(expr("value")?),
            "delete" => StmtKind::Delete(expr("target")?),
            "set" => StmtKind::Set { target: expr("target")?, value: expr("value")? },
            "trace" => StmtKind::Trace(expr("value")?),
            "tell" => StmtKind::Tell { target: expr("target")?, body: block("body")? },
            "unrecovered" => StmtKind::Unrecovered { opcode: text(field(object, "opcode")?, "opcode")? },
            "foreach" => StmtKind::ForEach {
                variable: text(field(object, "variable")?, "loop variable")?,
                sequence: expr("sequence")?,
                body: block("body")?,
            },
            "case" => StmtKind::Case {
                selector: expr("selector")?,
                branches: field(object, "branches")?
                    .as_array()
                    .ok_or("case branches")?
                    .iter()
                    .map(|branch| {
                        let branch = branch.as_object().ok_or("case branch")?;
                        Ok(Branch {
                            values: match branch.get("values") {
                                None | Some(Value::Null) => None,
                                Some(v) => Some(exprs(v)?),
                            },
                            body: stmts(field(branch, "body")?)?,
                        })
                    })
                    .collect::<Result<_, String>>()?,
            },
            other => StmtKind::Unknown(other.to_string()),
        };
        Ok(Stmt { line, kind })
    }
}

impl Handler {
    pub fn from_json(value: &Value) -> Result<Handler, String> {
        let object = value.as_object().ok_or("handler is not an object")?;
        Ok(Handler {
            name: text(field(object, "name")?, "handler name")?,
            member: field(object, "member")?.as_i64().ok_or("member is not an integer")?,
            cast: text(field(object, "cast")?, "cast")?,
            script_type: text(field(object, "script_type")?, "script_type")?,
            parameters: strings(field(object, "parameters")?, "parameters")?,
            properties: match object.get("properties") {
                Some(v) => strings(v, "properties")?,
                None => Vec::new(),
            },
            movie: text(field(object, "movie")?, "movie")?,
            line: field(object, "line")?.as_i64().ok_or("handler line")?,
            body: stmts(field(object, "body")?)?,
        })
    }
}

impl Program {
    pub fn from_json(value: &Value) -> Result<Program, String> {
        let object = value.as_object().ok_or("program is not an object")?;
        let handlers = field(object, "handlers")?
            .as_array()
            .ok_or("handlers is not a list")?
            .iter()
            .map(Handler::from_json)
            .collect::<Result<_, String>>()?;
        let files = match object.get("files") {
            Some(Value::Array(files)) => files
                .iter()
                .map(|f| text(field(f.as_object().ok_or("file entry")?, "name")?, "file name"))
                .collect::<Result<_, String>>()?,
            _ => Vec::new(),
        };
        // Python's truth test: only a non-empty value is carried across.
        let compatibility_fixes = match object.get("compatibility_fixes") {
            Some(Value::Array(a)) if !a.is_empty() => Some(Value::Array(a.clone())),
            Some(Value::Object(o)) if !o.is_empty() => Some(Value::Object(o.clone())),
            Some(Value::String(s)) if !s.is_empty() => Some(Value::String(s.clone())),
            Some(Value::Number(n)) if n.as_f64() != Some(0.0) => Some(Value::Number(n.clone())),
            Some(Value::Bool(true)) => Some(Value::Bool(true)),
            _ => None,
        };
        Ok(Program { handlers, files, compatibility_fixes })
    }
}

/// Every expression node of a tree in pre-order, the way the Python
/// generator's `walk` visited the lists of an expression.
pub fn expression_nodes<'a>(expr: &'a Expr, out: &mut Vec<&'a Expr>) {
    out.push(expr);
    match expr {
        Expr::Unary(_, e) | Expr::Get(_, e) | Expr::LastChunk(_, e) => expression_nodes(e, out),
        Expr::Binary(_, a, b) | Expr::Index(a, b) => {
            expression_nodes(a, out);
            expression_nodes(b, out);
        }
        Expr::List(items) => items.iter().for_each(|e| expression_nodes(e, out)),
        Expr::PropList(pairs) => pairs.iter().for_each(|(k, v)| {
            expression_nodes(k, out);
            expression_nodes(v, out);
        }),
        Expr::Property { value, .. } => expression_nodes(value, out),
        Expr::Method { receiver, args, .. } => {
            expression_nodes(receiver, out);
            args.iter().for_each(|e| expression_nodes(e, out));
        }
        Expr::Reference { number, library, .. } => {
            expression_nodes(number, out);
            if let Some(l) = library {
                expression_nodes(l, out);
            }
        }
        Expr::Chunk { first, last, source, .. } => {
            expression_nodes(first, out);
            expression_nodes(last, out);
            expression_nodes(source, out);
        }
        Expr::Call(_, args) => args.iter().for_each(|e| expression_nodes(e, out)),
        _ => {}
    }
}

/// Every expression of a block, statements included, in the order the
/// Python generator's `walk` met them (statement fields in their JSON order).
pub fn block_expressions<'a>(block: &'a [Stmt], out: &mut Vec<&'a Expr>) {
    for stmt in block {
        match &stmt.kind {
            StmtKind::Global(_) | StmtKind::Break | StmtKind::Continue | StmtKind::Unknown(_) => {}
            StmtKind::If { condition, yes, no } => {
                expression_nodes(condition, out);
                block_expressions(yes, out);
                block_expressions(no, out);
            }
            StmtKind::While { condition, body } => {
                expression_nodes(condition, out);
                block_expressions(body, out);
            }
            StmtKind::For { first, last, body, .. } => {
                expression_nodes(first, out);
                expression_nodes(last, out);
                block_expressions(body, out);
            }
            StmtKind::Return(value) => {
                if let Some(v) = value {
                    expression_nodes(v, out);
                }
            }
            StmtKind::Call(e) | StmtKind::Delete(e) | StmtKind::Trace(e) => expression_nodes(e, out),
            StmtKind::Set { target, value } => {
                expression_nodes(target, out);
                expression_nodes(value, out);
            }
            StmtKind::Tell { target, body } => {
                expression_nodes(target, out);
                block_expressions(body, out);
            }
            StmtKind::Unrecovered { .. } => {}
            StmtKind::ForEach { sequence, body, .. } => {
                expression_nodes(sequence, out);
                block_expressions(body, out);
            }
            StmtKind::Case { selector, branches } => {
                expression_nodes(selector, out);
                for branch in branches {
                    if let Some(values) = &branch.values {
                        values.iter().for_each(|v| expression_nodes(v, out));
                    }
                    block_expressions(&branch.body, out);
                }
            }
        }
    }
}

/// Visit every statement of a block, nested blocks included.
pub fn for_each_stmt<'a>(block: &'a [Stmt], visit: &mut dyn FnMut(&'a Stmt)) {
    for stmt in block {
        visit(stmt);
        match &stmt.kind {
            StmtKind::If { yes, no, .. } => {
                for_each_stmt(yes, visit);
                for_each_stmt(no, visit);
            }
            StmtKind::While { body, .. }
            | StmtKind::For { body, .. }
            | StmtKind::Tell { body, .. }
            | StmtKind::ForEach { body, .. } => for_each_stmt(body, visit),
            StmtKind::Case { branches, .. } => branches.iter().for_each(|b| for_each_stmt(&b.body, visit)),
            _ => {}
        }
    }
}

//! Lower one handler to bytecode states: the expression stack discipline,
//! the state splits around expression calls, the polling yields, and the
//! per-movie name and double pools, in the exact order the Python generator
//! interned them.

use std::collections::{HashMap, HashSet};

use crate::ast::{block_expressions, expression_nodes, for_each_stmt, Expr, Handler, Num, Stmt, StmtKind};
use crate::bytecode::{assemble, op, Op, BINARY_OPERATORS, UNARY_OPERATORS};
use crate::emit::fold;
use crate::normalize::normalize;

/// Per-movie constant pools so ops carry small indices, not values.
/// Insertion order is emission order, keeping output deterministic.
#[derive(Default)]
pub struct Interner {
    pub names: Vec<String>,
    /// Whether an op reads the pooled name as a symbol (a literal, a
    /// property name, a declaration): those names get corpus-wide symbol
    /// ids; text literals and handler names do not.
    pub symbol_use: Vec<bool>,
    name_index: HashMap<String, usize>,
    pub doubles: Vec<f64>,
    double_index: HashMap<u64, usize>,
}

impl Interner {
    pub fn name(&mut self, text: &str) -> usize {
        if let Some(&i) = self.name_index.get(text) {
            return i;
        }
        self.names.push(text.to_string());
        self.symbol_use.push(false);
        self.name_index.insert(text.to_string(), self.names.len() - 1);
        self.names.len() - 1
    }
    pub fn double(&mut self, value: f64) -> usize {
        // Python keys the pool by value, where 0.0 and -0.0 are one key.
        let key = if value == 0.0 { 0.0f64 } else { value }.to_bits();
        if let Some(&i) = self.double_index.get(&key) {
            return i;
        }
        self.doubles.push(value);
        self.double_index.insert(key, self.doubles.len() - 1);
        self.doubles.len() - 1
    }
}

/// Whether a condition reads something only a later service tick changes.
fn polls_input(nodes: &[&Expr]) -> bool {
    nodes.iter().any(|node| match node {
        Expr::The(name) => matches!(
            name.as_str(),
            "timer" | "ticks" | "milliseconds" | "mousedown" | "stilldown" | "mouseup" | "mouseh" | "mousev"
        ),
        Expr::Call(name, _) => name == "soundbusy",
        // A linked movie advances on the service tick that decodes it, so
        // `repeat while the movieRate of sprite n` can only observe one
        // value per tick however fast it spins.
        Expr::Property { name, kind, .. } => (name == "movierate" || name == "movietime") && kind == "sprite",
        _ => false,
    })
}

/// Each movie handler's call cycle, where recursion can stand in for a loop.
/// Only cycles whose every call to a member is a statement qualify, since a
/// yield under an expression call is an error rather than a pause.
pub fn polling_cycles(handlers: &[&Handler]) -> HashMap<String, HashSet<String>> {
    let mut calls: HashMap<String, HashSet<String>> = HashMap::new();
    let mut expression_callees: HashSet<String> = HashSet::new();
    for handler in handlers {
        let name = handler.name.to_lowercase();
        let mut statement_calls: HashSet<*const Expr> = HashSet::new();
        for_each_stmt(&handler.body, &mut |stmt| {
            if let StmtKind::Call(value @ Expr::Call(..)) = &stmt.kind {
                statement_calls.insert(value as *const Expr);
            }
        });
        let mut nodes = Vec::new();
        block_expressions(&handler.body, &mut nodes);
        let callees = calls.entry(name).or_default();
        for node in nodes {
            if let Expr::Call(callee, _) = node {
                callees.insert(callee.to_lowercase());
                if !statement_calls.contains(&(node as *const Expr)) {
                    expression_callees.insert(callee.to_lowercase());
                }
            }
        }
    }
    let defined: HashSet<String> = calls.keys().cloned().collect();
    for callees in calls.values_mut() {
        callees.retain(|c| defined.contains(c));
    }
    let reachable = |start: &str| -> HashSet<String> {
        let mut seen = HashSet::new();
        let mut stack = vec![start.to_string()];
        while let Some(current) = stack.pop() {
            for callee in &calls[&current] {
                if seen.insert(callee.clone()) {
                    stack.push(callee.clone());
                }
            }
        }
        seen
    };
    let reach: HashMap<String, HashSet<String>> = calls.keys().map(|n| (n.clone(), reachable(n))).collect();
    let mut cycles = HashMap::new();
    for name in calls.keys() {
        if !reach[name].contains(name) {
            continue;
        }
        let cycle: HashSet<String> = reach[name].iter().filter(|o| reach[*o].contains(name)).cloned().collect();
        if cycle.is_disjoint(&expression_callees) {
            cycles.insert(name.clone(), cycle);
        }
    }
    cycles
}

enum Item {
    Op(Op),
    /// A handler call inside an expression: the state splits here. `target`
    /// is the movie handler the call binds to, where one is known.
    ExprCall { name: String, argc: usize, owner: bool, target: Option<(usize, bool)> },
}

/// What a movie's handler table says about each entry, in table order: the
/// facts a call site needs to bind to an entry at convert time.
pub struct Sibling {
    pub name: String,
    pub member: i64,
    pub cast: String,
    pub movie_script: bool,
}

/// What the whole corpus lets a call site assume.
pub struct Corpus<'a> {
    /// Every handler name any movie defines.
    pub handler_names: &'a HashSet<String>,
    /// Whether any script could give an instance an ancestor: if no handler
    /// so much as spells the word, no ancestor chain can shadow a movie
    /// script's handler from a behavior.
    pub ancestors: bool,
}

/// The names invoke treats specially before any lookup; a call to one of
/// them is never bound.
fn special_call(name: &str) -> bool {
    matches!(name, "do" | "new" | "call" | "save")
}

pub struct Compiler<'a> {
    handler: Handler,
    globals: &'a HashMap<String, usize>,
    custom_calls: &'a HashSet<String>,
    siblings: &'a [Sibling],
    ancestors: bool,
    cycle: HashSet<String>,
    interner: &'a mut Interner,
    /// Call sites bound to a movie handler, answered by a builtin directly,
    /// and those resolved by name at run time.
    pub local_calls: usize,
    pub builtin_calls: usize,
    pub dynamic_calls: usize,
    global_names: HashSet<String>,
    properties: HashSet<String>,
    property_slots: HashMap<String, usize>,
    assigned: HashSet<String>,
    pub locals: Vec<String>,
    local_index: HashMap<String, usize>,
    states: Vec<(i64, Vec<Op>)>,
    lines: Vec<Item>,
    depth: i32,
    peak: i32,
}

fn number_op(value: i64) -> Op {
    if (-128..128).contains(&value) {
        op("num8", vec![value])
    } else {
        op("num32", vec![value])
    }
}

impl<'a> Compiler<'a> {
    pub fn new(
        handler: &Handler,
        globals: &'a HashMap<String, usize>,
        corpus: &Corpus<'a>,
        siblings: &'a [Sibling],
        interner: &'a mut Interner,
        cycle: Option<HashSet<String>>,
    ) -> Result<Compiler<'a>, String> {
        let custom_calls = corpus.handler_names;
        let mut handler = handler.clone();
        handler.body = normalize(&handler.body);
        let mut global_names: HashSet<String> = HashSet::new();
        let mut names: HashSet<String> = HashSet::new();
        let mut assigned: HashSet<String> = handler.parameters.iter().cloned().collect();
        for_each_stmt(&handler.body, &mut |stmt| match &stmt.kind {
            StmtKind::Global(declared) => global_names.extend(declared.iter().cloned()),
            StmtKind::For { variable, .. } => {
                names.insert(variable.clone());
                assigned.insert(variable.clone());
            }
            StmtKind::Set { target: Expr::Var(name), .. } => {
                assigned.insert(name.clone());
            }
            _ => {}
        });
        let mut nodes = Vec::new();
        block_expressions(&handler.body, &mut nodes);
        for node in nodes {
            if let Expr::Var(name) = node {
                names.insert(name.clone());
            }
        }
        // Director's object-call notation can read an undeclared global by
        // symbol (e.g. getAt(globalList, i)). ProjectorRays prints the symbol
        // as a variable, without adding a global declaration. An actual local
        // or parameter still shadows it.
        // A declaration keeps the property's authored spelling (the parser
        // folds every other identifier), and pooling it before the body
        // makes that spelling the symbol's text; every comparison folds.
        for property in &handler.properties {
            interner.name(property);
        }
        let declared: HashSet<String> = handler.properties.iter().map(|p| fold(p)).collect();
        for name in &names {
            if globals.contains_key(name) && !assigned.contains(name) && !declared.contains(name) {
                global_names.insert(name.clone());
            }
        }
        let parameters: HashSet<&String> = handler.parameters.iter().collect();
        let properties: HashSet<String> = handler
            .properties
            .iter()
            .map(|p| fold(p))
            .filter(|p| !parameters.contains(p) && !global_names.contains(p))
            .collect();
        // Where each declared property's pair sits in an instance built by
        // this script: `new`/`birth` materialize the declarations in order,
        // so the i-th declaration is the pair at index 2i. A read or write
        // carries that as a hint the runtime checks by one word compare.
        let property_slots: HashMap<String, usize> = handler
            .properties
            .iter()
            .enumerate()
            .map(|(i, p)| (fold(p), 2 * i))
            .collect();
        let mut rest: Vec<String> = names
            .iter()
            .filter(|n| !parameters.contains(n) && !global_names.contains(*n) && !properties.contains(*n))
            .cloned()
            .collect();
        rest.sort();
        let mut locals: Vec<String> = handler.parameters.clone();
        locals.extend(rest);
        if locals.len() > 96 {
            return Err("handler local-variable budget exceeded".into());
        }
        let local_index = locals.iter().enumerate().map(|(i, n)| (n.clone(), i)).collect();
        Ok(Compiler {
            handler,
            globals,
            custom_calls,
            siblings,
            ancestors: corpus.ancestors,
            cycle: cycle.unwrap_or_default(),
            interner,
            local_calls: 0,
            builtin_calls: 0,
            dynamic_calls: 0,
            global_names,
            properties,
            property_slots,
            assigned,
            locals,
            local_index,
            states: Vec::new(),
            lines: Vec::new(),
            depth: 0,
            peak: 0,
        })
    }

    fn emit(&mut self, op: Op, delta: i32) -> Result<(), String> {
        self.lines.push(Item::Op(op));
        self.depth += delta;
        if self.depth > self.peak {
            self.peak = self.depth;
        }
        if self.peak > 96 {
            return Err("native expression stack budget exceeded".into());
        }
        if self.depth < 0 {
            return Err("native expression stack underflow at compile".into());
        }
        Ok(())
    }

    fn name(&mut self, text: &str) -> i64 {
        self.interner.name(text) as i64
    }

    /// The movie handler an unqualified call binds to, if the runtime's
    /// lookup would end there whatever the frame holds: the calling
    /// script's own handler of that name (own: the callee keeps the
    /// caller's receiver), or the first movie-script handler of that name
    /// (not own: it runs without one). The names the runtime treats
    /// specially before any lookup are never bound.
    fn local_target(&mut self, name: &str) -> Option<(usize, bool)> {
        let target = if special_call(name) {
            None
        } else {
            let h = &self.handler;
            self.siblings
                .iter()
                .position(|s| s.name.eq_ignore_ascii_case(name) && s.member == h.member && s.cast.eq_ignore_ascii_case(&h.cast))
                .map(|i| (i, true))
                .or_else(|| {
                    // A movie script's frame never has a receiver, and a
                    // behavior's receiver can only shadow the movie script
                    // through an ancestor.
                    if h.script_type == "MovieScript" || !self.ancestors {
                        self.siblings.iter().position(|s| s.movie_script && s.name.eq_ignore_ascii_case(name)).map(|i| (i, false))
                    } else {
                        None
                    }
                })
        };
        if target.is_some() {
            self.local_calls += 1;
        } else {
            self.dynamic_calls += 1;
        }
        target
    }

    /// Whether a call to `name` can go straight to the runtime's builtin: no
    /// handler anywhere defines it, and invoke does not treat it specially.
    fn builtin_call(&mut self, name: &str) -> bool {
        let builtin = !special_call(name) && !self.custom_calls.contains(name);
        if builtin {
            self.builtin_calls += 1;
        }
        builtin
    }

    fn operator(table: &[&str], text: &str) -> Result<i64, String> {
        table
            .iter()
            .position(|t| *t == text)
            .map(|i| i as i64)
            .ok_or_else(|| format!("unsupported operator {text}"))
    }

    fn push_variable(&mut self, name: &str) -> Result<(), String> {
        if self.properties.contains(name) {
            let index = self.name(name);
            match self.property_slots.get(name) {
                Some(&slot) if slot <= 255 => self.emit(op("self_slot", vec![index, slot as i64]), 1),
                _ => self.emit(op("self", vec![index]), 1),
            }
        } else if self.global_names.contains(name) {
            self.emit(op("global", vec![self.globals[name] as i64]), 1)
        } else {
            let index = *self.local_index.get(name).ok_or_else(|| format!("unknown local {name}"))?;
            self.emit(op("local", vec![index as i64]), 1)
        }
    }

    fn store_variable(&mut self, name: &str) -> Result<(), String> {
        if self.properties.contains(name) {
            let index = self.name(name);
            match self.property_slots.get(name) {
                Some(&slot) if slot <= 255 => {
                    self.emit(op("set_self_slot", vec![index, slot as i64]), -1)
                }
                _ => self.emit(op("set_self", vec![index]), -1),
            }
        } else if self.global_names.contains(name) {
            self.emit(op("set_global", vec![self.globals[name] as i64]), -1)
        } else {
            let index = *self.local_index.get(name).ok_or_else(|| format!("unknown local {name}"))?;
            self.emit(op("set_local", vec![index as i64]), -1)
        }
    }

    /// Emit ops that push exactly one value, in source evaluation order.
    fn expr(&mut self, node: &Expr) -> Result<(), String> {
        match node {
            Expr::Number(Num::Float(value)) => {
                // Authored float literals stay floats even when integral.
                let index = self.interner.double(*value) as i64;
                self.emit(op("dbl", vec![index]), 1)
            }
            Expr::Number(Num::Int(value)) => {
                if (i32::MIN as i64..=i32::MAX as i64).contains(value) {
                    self.emit(number_op(*value), 1)
                } else {
                    let index = self.interner.double(*value as f64) as i64;
                    self.emit(op("numd", vec![index]), 1)
                }
            }
            Expr::Str(text) => {
                let index = self.name(text);
                self.emit(op("text", vec![index]), 1)
            }
            Expr::Sym(text) => {
                let index = self.name(text);
                self.emit(op("sym", vec![index]), 1)
            }
            Expr::Var(name) => {
                let constant = match name.as_str() {
                    "empty" => Some(("text", "")),
                    "return" => Some(("text", "\r")),
                    "true" => Some(("num", "1")),
                    "false" => Some(("num", "0")),
                    "void" => Some(("void", "")),
                    "mnew" => Some(("sym", "mnew")),
                    "mreadline" => Some(("sym", "mreadline")),
                    "mreadfile" => Some(("sym", "mreadfile")),
                    "mwritestring" => Some(("sym", "mwritestring")),
                    "mdispose" => Some(("sym", "mdispose")),
                    "pi" => Some(("dbl", "")),
                    "tab" => Some(("text", "\t")),
                    "quote" => Some(("text", "\"")),
                    "backspace" => Some(("text", "\u{8}")),
                    _ => None,
                };
                let shadowed = self.assigned.contains(name)
                    || self.properties.contains(name)
                    || self.global_names.contains(name);
                match constant {
                    Some((kind, argument)) if !shadowed => match kind {
                        "void" => self.emit(op("void", vec![]), 1),
                        "num" => self.emit(number_op(argument.parse().unwrap()), 1),
                        "dbl" => {
                            let index = self.interner.double(std::f64::consts::PI) as i64;
                            self.emit(op("dbl", vec![index]), 1)
                        }
                        _ => {
                            let index = self.name(argument);
                            self.emit(op(kind, vec![index]), 1)
                        }
                    },
                    _ => self.push_variable(name),
                }
            }
            Expr::Unary(operator, value) => {
                self.expr(value)?;
                let id = Self::operator(UNARY_OPERATORS, operator)?;
                self.emit(op("unary", vec![id]), 0)
            }
            Expr::Binary(operator, left, right) => {
                self.expr(left)?;
                self.expr(right)?;
                let id = Self::operator(BINARY_OPERATORS, operator)?;
                self.emit(op("binary", vec![id]), -1)
            }
            Expr::List(_) | Expr::PropList(_) => {
                let (items, props): (Vec<&Expr>, i64) = match node {
                    Expr::List(items) => (items.iter().collect(), 0),
                    Expr::PropList(pairs) => (pairs.iter().flat_map(|(k, v)| [k, v]).collect(), 1),
                    _ => unreachable!(),
                };
                // Long literal lists build in segments so construction fits
                // the frame's expression stack regardless of the list's length.
                let head = items.len().min(48);
                for item in &items[..head] {
                    self.expr(item)?;
                }
                self.emit(op("list", vec![head as i64, props]), 1 - head as i32)?;
                let mut rest = &items[head..];
                while !rest.is_empty() {
                    let segment = rest.len().min(48);
                    for item in &rest[..segment] {
                        self.expr(item)?;
                    }
                    self.emit(op("list_extend", vec![segment as i64]), -(segment as i32))?;
                    rest = &rest[segment..];
                }
                Ok(())
            }
            Expr::The(name) => {
                let index = self.name(name);
                self.emit(op("the", vec![index]), 1)
            }
            Expr::Property { name, kind, value } => {
                self.expr(&Expr::Reference { kind: kind.clone(), number: value.clone(), library: None })?;
                let index = self.name(name);
                self.emit(op("get", vec![index]), 0)
            }
            Expr::Get(name, value) => {
                if name == "count"
                    && let Expr::Get(inner, source) = &**value
                    && matches!(inner.as_str(), "chars" | "words" | "items" | "lines")
                {
                    self.expr(source)?;
                    let index = self.name(&inner[..inner.len() - 1]);
                    return self.emit(op("chunk_count", vec![index]), 0);
                }
                self.expr(value)?;
                let index = self.name(name);
                self.emit(op("get", vec![index]), 0)
            }
            Expr::Index(owner, index) => {
                self.expr(owner)?;
                self.expr(index)?;
                self.emit(op("index", vec![]), -1)
            }
            Expr::LastChunk(kind, value) => {
                self.expr(value)?;
                let index = self.name(kind);
                self.emit(op("last_chunk", vec![index]), 0)
            }
            Expr::Method { name, receiver, args } => {
                self.expr(receiver)?;
                for argument in args {
                    self.expr(argument)?;
                }
                // The state splits here; the continuation pushes the result.
                self.lines.push(Item::ExprCall { name: name.clone(), argc: args.len(), owner: true, target: None });
                self.depth -= args.len() as i32;
                Ok(())
            }
            Expr::Reference { kind, number, library } => {
                self.expr(number)?;
                match library {
                    Some(library) => self.expr(library)?,
                    None => self.emit(op("void", vec![]), 1)?,
                }
                let index = self.name(kind);
                self.emit(op("reference", vec![index]), -1)
            }
            Expr::Chunk { kind, first, last, source } => {
                self.expr(first)?;
                self.expr(last)?;
                self.expr(source)?;
                let index = self.name(kind);
                self.emit(op("chunk", vec![index]), -2)
            }
            Expr::Call(name, args) => {
                for argument in args {
                    self.expr(argument)?;
                }
                if self.custom_calls.contains(name) {
                    // An expression handler can show a dialogue and wait for
                    // input. The state splits at the call; the values beneath
                    // the arguments simply stay on the rooted stack across
                    // it, and the continuation pushes the eventual result.
                    let target = self.local_target(name);
                    self.lines.push(Item::ExprCall { name: name.clone(), argc: args.len(), owner: false, target });
                    self.depth += 1 - args.len() as i32;
                    self.peak = self.peak.max(self.depth);
                    return Ok(());
                }
                let index = self.name(name);
                let opcode = if self.builtin_call(name) { "call_builtin_expr" } else { "call" };
                self.emit(op(opcode, vec![index, args.len() as i64]), 1 - args.len() as i32)
            }
            Expr::Unknown(kind) => Err(format!("unknown expression {kind}")),
        }
    }

    /// Consume the value on the stack top into the assignment target.
    fn assignment(&mut self, target: &Expr) -> Result<(), String> {
        match target {
            Expr::Var(name) => self.store_variable(name),
            Expr::The(name) => {
                let index = self.name(name);
                self.emit(op("set_the", vec![index]), -1)
            }
            Expr::Property { name, kind, value } => {
                self.expr(&Expr::Reference { kind: kind.clone(), number: value.clone(), library: None })?;
                let index = self.name(name);
                self.emit(op("set", vec![index]), -2)
            }
            Expr::Get(name, value) => {
                self.expr(value)?;
                let index = self.name(name);
                self.emit(op("set", vec![index]), -2)
            }
            Expr::Index(owner, index) => {
                self.expr(owner)?;
                self.expr(index)?;
                self.emit(op("set_index", vec![]), -3)
            }
            Expr::Reference { kind, .. } if kind == "field" || kind == "member" => {
                self.expr(target)?;
                let index = self.name("text");
                self.emit(op("set", vec![index]), -2)
            }
            Expr::Chunk { kind, first, last, source } => {
                self.expr(source)?;
                self.expr(first)?;
                self.expr(last)?;
                let index = self.name(kind);
                self.emit(op("chunk_set", vec![index]), -3)?;
                self.assignment(source)
            }
            other => Err(format!("unimplemented assignment target {other:?}")),
        }
    }

    fn state(&mut self, mut statements: Vec<Item>, line: i64) -> Result<usize, String> {
        let index = self.states.len();
        self.states.push((line, Vec::new()));
        if let Some(i) = statements.iter().position(|item| matches!(item, Item::ExprCall { .. })) {
            // A handler call inside an expression suspends this state. Its
            // operands are already on the frame's rooted temp stack, so the
            // split needs no spill code: the invocation pops its arguments
            // and the continuation pushes the eventual result.
            let after: Vec<Item> = statements.drain(i + 1..).collect();
            let Some(Item::ExprCall { name, argc, owner, target }) = statements.pop() else { unreachable!() };
            let mut tail = vec![Item::Op(op("result", vec![]))];
            tail.extend(after);
            let continuation = self.state(tail, line)?;
            let mut before = ops_of(statements)?;
            if let Some((entry, own)) = target {
                before.push(op("invoke_local_expr", vec![entry as i64, argc as i64, own as i64, continuation as i64]));
            } else {
                let invocation = if owner { "invoke_method_expr" } else { "invoke_expr" };
                let name_index = self.name(&name);
                before.push(op(invocation, vec![name_index, argc as i64, continuation as i64]));
            }
            self.states[index] = (line, before);
            return Ok(index);
        }
        self.states[index] = (line, ops_of(statements)?);
        Ok(index)
    }

    fn take_lines(&mut self) -> Vec<Item> {
        std::mem::take(&mut self.lines)
    }

    fn lower(
        &mut self,
        block: &[Stmt],
        after: usize,
        break_to: Option<usize>,
        continue_to: Option<usize>,
    ) -> Result<usize, String> {
        let mut next_pc = after;
        for node in block.iter().rev() {
            self.lines = Vec::new();
            self.depth = 0;
            let line = node.line;
            match &node.kind {
                StmtKind::Global(_) => continue,
                StmtKind::If { condition, yes, no } => {
                    let yes_pc = self.lower(yes, next_pc, break_to, continue_to)?;
                    let no_pc = self.lower(no, next_pc, break_to, continue_to)?;
                    self.lines = Vec::new();
                    self.depth = 0;
                    self.expr(condition)?;
                    self.emit(op("branch", vec![yes_pc as i64, no_pc as i64]), -1)?;
                    let mut condition_nodes = Vec::new();
                    expression_nodes(condition, &mut condition_nodes);
                    let mut yes_nodes = Vec::new();
                    block_expressions(yes, &mut yes_nodes);
                    let recurses = yes_nodes
                        .iter()
                        .any(|n| matches!(n, Expr::Call(name, _) if self.cycle.contains(&name.to_lowercase())));
                    if polls_input(&condition_nodes) && recurses {
                        // Recursion guarded by a poll is a polling loop: each
                        // pass waits a tick, as a loop does at its back edge,
                        // so the guard sees input change instead of recursing
                        // on one frozen sample until the call stack runs out.
                        let lines = self.take_lines();
                        let test = self.state(lines, line)?;
                        self.lines = vec![Item::Op(op("yield", vec![test as i64]))];
                    }
                }
                StmtKind::While { condition, body } => {
                    let test = self.state(Vec::new(), line)?;
                    let mut increment = test;
                    let mut condition_nodes = Vec::new();
                    expression_nodes(condition, &mut condition_nodes);
                    let polls = polls_input(&condition_nodes);
                    let waits = body.iter().any(|v| match &v.kind {
                        StmtKind::Call(Expr::Call(name, _)) | StmtKind::Call(Expr::Method { name, .. }) => name == "delay",
                        _ => false,
                    });
                    // updateStage commits visuals without advancing time.
                    // Polling loops still need to let input/audio/timers run.
                    if polls && !waits {
                        increment = self.state(vec![Item::Op(op("yield", vec![test as i64]))], line)?;
                    }
                    let body_pc = self.lower(body, increment, Some(next_pc), Some(increment))?;
                    self.lines = Vec::new();
                    self.depth = 0;
                    self.expr(condition)?;
                    self.emit(op("branch", vec![body_pc as i64, next_pc as i64]), -1)?;
                    let lines = self.take_lines();
                    let test_expression = self.state(lines, line)?;
                    self.states[test] = (line, vec![op("jump", vec![test_expression as i64])]);
                    next_pc = test;
                    continue;
                }
                StmtKind::For { variable, first, last, step, body } => {
                    let test = self.state(Vec::new(), line)?;
                    self.expr(&Expr::Binary(
                        "+".into(),
                        Box::new(Expr::Var(variable.clone())),
                        Box::new(Expr::Number(Num::Int(*step))),
                    ))?;
                    self.assignment(&Expr::Var(variable.clone()))?;
                    self.lines.push(Item::Op(op("jump", vec![test as i64])));
                    let lines = self.take_lines();
                    let increment = self.state(lines, line)?;
                    let body_pc = self.lower(body, increment, Some(next_pc), Some(increment))?;
                    self.lines = Vec::new();
                    self.depth = 0;
                    self.expr(&Expr::Binary(
                        if *step == 1 { "<=" } else { ">=" }.into(),
                        Box::new(Expr::Var(variable.clone())),
                        Box::new(last.clone()),
                    ))?;
                    self.emit(op("branch", vec![body_pc as i64, next_pc as i64]), -1)?;
                    let lines = self.take_lines();
                    let test_expression = self.state(lines, line)?;
                    self.states[test] = (line, vec![op("jump", vec![test_expression as i64])]);
                    self.lines = Vec::new();
                    self.depth = 0;
                    self.expr(first)?;
                    self.assignment(&Expr::Var(variable.clone()))?;
                    self.lines.push(Item::Op(op("jump", vec![test as i64])));
                }
                StmtKind::Break | StmtKind::Continue => {
                    let (name, destination) = match node.kind {
                        StmtKind::Break => ("break", break_to),
                        _ => ("continue", continue_to),
                    };
                    let destination = destination.ok_or_else(|| format!("{name} outside loop"))?;
                    self.lines.push(Item::Op(op("jump", vec![destination as i64])));
                }
                StmtKind::Return(value) => match value {
                    Some(value) => {
                        self.expr(value)?;
                        self.emit(op("return", vec![]), -1)?;
                    }
                    None => self.lines.push(Item::Op(op("return_void", vec![]))),
                },
                StmtKind::Call(value) => match value {
                    Expr::Call(name, args) => {
                        for argument in args {
                            self.expr(argument)?;
                        }
                        if self.builtin_call(name) {
                            let index = self.name(name);
                            self.emit(op("call_builtin", vec![index, args.len() as i64, next_pc as i64]), -(args.len() as i32))?;
                        } else if let Some((entry, own)) = self.local_target(name) {
                            self.emit(op("invoke_local", vec![entry as i64, args.len() as i64, own as i64, next_pc as i64]), -(args.len() as i32))?;
                        } else {
                            let index = self.name(name);
                            self.emit(op("invoke", vec![index, args.len() as i64, next_pc as i64]), -(args.len() as i32))?;
                        }
                    }
                    Expr::Method { name, receiver, args } => {
                        self.expr(receiver)?;
                        for argument in args {
                            self.expr(argument)?;
                        }
                        let index = self.name(name);
                        self.emit(
                            op("invoke_method", vec![index, args.len() as i64, next_pc as i64]),
                            -(args.len() as i32) - 1,
                        )?;
                    }
                    other => return Err(format!("unsupported call statement {other:?}")),
                },
                StmtKind::Delete(target) => {
                    let Expr::Chunk { kind, first, last, source } = target else {
                        return Err(format!("unsupported delete target {target:?}"));
                    };
                    self.expr(source)?;
                    self.expr(first)?;
                    self.expr(last)?;
                    let index = self.name(kind);
                    self.emit(op("chunk_delete", vec![index]), -2)?;
                    self.assignment(source)?;
                    self.lines.push(Item::Op(op("jump", vec![next_pc as i64])));
                }
                StmtKind::Set { target, value } => {
                    self.expr(value)?;
                    self.assignment(target)?;
                    self.lines.push(Item::Op(op("jump", vec![next_pc as i64])));
                }
                StmtKind::Trace(value) => {
                    self.expr(value)?;
                    self.emit(op("trace", vec![]), -1)?;
                    self.lines.push(Item::Op(op("jump", vec![next_pc as i64])));
                }
                StmtKind::Tell { .. } | StmtKind::Unrecovered { .. } | StmtKind::ForEach { .. } | StmtKind::Case { .. } => {
                    return Err("statement survived normalization".into());
                }
                StmtKind::Unknown(name) => return Err(format!("unknown statement {name}")),
            }
            let lines = self.take_lines();
            next_pc = self.state(lines, line)?;
        }
        Ok(next_pc)
    }

    /// The handler's bytecode and the byte offset of its entry state.
    pub fn compile(mut self) -> Result<(usize, Vec<u8>, (usize, usize, usize)), String> {
        let line = self.handler.line;
        let end = self.state(vec![Item::Op(op("return_void", vec![]))], line)?;
        let body = std::mem::take(&mut self.handler.body);
        let mut entry = self.lower(&body, end, None, None)?;
        if !self.handler.properties.is_empty() && (self.handler.name == "new" || self.handler.name == "birth") {
            // Director materializes declared properties at construction; the
            // instance protocol then counts declarations before first writes.
            let properties = self.handler.properties.clone();
            let mut declares = Vec::new();
            for property in &properties {
                let index = self.name(property);
                declares.push(Item::Op(op("declare", vec![index])));
            }
            declares.push(Item::Op(op("jump", vec![entry as i64])));
            entry = self.state(declares, line)?;
        }
        for (_, ops) in &self.states {
            for op in ops {
                if matches!(
                    op.code.name,
                    "sym" | "self" | "self_slot" | "set_self" | "set_self_slot" | "get" | "set" | "declare"
                ) {
                    self.interner.symbol_use[op.args[0] as usize] = true;
                }
            }
        }
        let (code, offsets) = assemble(&self.states)?;
        Ok((offsets[entry], code, (self.local_calls, self.builtin_calls, self.dynamic_calls)))
    }
}

fn ops_of(items: Vec<Item>) -> Result<Vec<Op>, String> {
    items
        .into_iter()
        .map(|item| match item {
            Item::Op(op) => Ok(op),
            Item::ExprCall { .. } => Err("expression call left in a state".to_string()),
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ast::Program;

    fn program(handlers: &[(&str, &[(&str, bool)])]) -> Program {
        let handlers: Vec<serde_json::Value> = handlers
            .iter()
            .map(|(name, calls)| {
                let body: Vec<serde_json::Value> = calls
                    .iter()
                    .map(|(callee, expression)| {
                        let call = serde_json::json!(["call", callee, [["number", 1]]]);
                        if *expression {
                            serde_json::json!({"op": "set", "target": ["variable", "x"], "value": call, "line": 1})
                        } else {
                            serde_json::json!({"op": "call", "value": call, "line": 1})
                        }
                    })
                    .collect();
                serde_json::json!({
                    "name": name, "member": 1, "cast": "Internal", "script_type": "MovieScript",
                    "parameters": [], "properties": [], "movie": "T.DXR", "line": 1, "body": body,
                })
            })
            .collect();
        Program::from_json(&serde_json::json!({"handlers": handlers, "files": []})).unwrap()
    }

    /// A cycle closed only by statement calls qualifies, whatever the
    /// spelling; a builtin callee or an expression call never closes one.
    #[test]
    fn polling_cycles_follow_statement_calls_only() {
        let p = program(&[("a", &[("B", false), ("updatestage", false)]), ("b", &[("a", false)]), ("c", &[("c", true)])]);
        let handlers: Vec<&Handler> = p.handlers.iter().collect();
        let cycles = polling_cycles(&handlers);
        assert!(cycles["a"].contains("b") && cycles["b"].contains("a"));
        assert!(!cycles.contains_key("updatestage"));
        assert!(!cycles.contains_key("c") || cycles["c"].is_empty());
    }
}

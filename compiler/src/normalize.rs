//! Lower collection loops, cases, tells, deletes and undecompiled markers to
//! the core statements the compiler knows, exactly as the Python generator's
//! `normalize_handler` did: children first, hidden locals numbered in the
//! same order, so the bytecode and the name pools come out identical.

use crate::ast::{Expr, Num, Stmt, StmtKind};

pub fn normalize(body: &[Stmt]) -> Vec<Stmt> {
    let mut serial = 0u32;
    block(body, &mut serial)
}

fn temporary(kind: &str, serial: &mut u32) -> String {
    *serial += 1;
    format!("${kind}{serial}")
}

fn block(nodes: &[Stmt], serial: &mut u32) -> Vec<Stmt> {
    let mut output = Vec::new();
    for node in nodes {
        let line = node.line;
        // Nested blocks lower first, as the Python did for yes/no/body.
        let kind = match &node.kind {
            StmtKind::If { condition, yes, no } => StmtKind::If {
                condition: condition.clone(),
                yes: block(yes, serial),
                no: block(no, serial),
            },
            StmtKind::While { condition, body } => {
                StmtKind::While { condition: condition.clone(), body: block(body, serial) }
            }
            StmtKind::For { variable, first, last, step, body } => StmtKind::For {
                variable: variable.clone(),
                first: first.clone(),
                last: last.clone(),
                step: *step,
                body: block(body, serial),
            },
            StmtKind::Tell { target, body } => {
                StmtKind::Tell { target: target.clone(), body: block(body, serial) }
            }
            StmtKind::ForEach { variable, sequence, body } => StmtKind::ForEach {
                variable: variable.clone(),
                sequence: sequence.clone(),
                body: block(body, serial),
            },
            other => other.clone(),
        };
        let call = |name: &str, args: Vec<Expr>| Stmt {
            line,
            kind: StmtKind::Call(Expr::Call(name.to_string(), args)),
        };
        match kind {
            StmtKind::Tell { target, body } => {
                if target == Expr::The("stage".into()) {
                    output.push(call("tell_stage", vec![]));
                    output.extend(body);
                    output.push(call("tell_end", vec![]));
                } else if let Expr::Call(name, args) = &target
                    && name == "window"
                {
                    output.push(call("tell_window", args.clone()));
                    output.extend(body);
                    output.push(call("tell_end", vec![]));
                } else {
                    // Remaining tells target sprites, as sprite(n) references
                    // or sprite values; they drive that sprite's flattened
                    // film timeline, and handler sends retarget to its
                    // behaviors.
                    let channel = temporary("tellsprite", serial);
                    let target = match &target {
                        Expr::Reference { kind, number, .. } if kind == "sprite" => (**number).clone(),
                        other => other.clone(),
                    };
                    output.push(Stmt {
                        line,
                        kind: StmtKind::Set { target: Expr::Var(channel.clone()), value: retarget(&target, &channel) },
                    });
                    output.extend(resprite(&body, &channel));
                }
            }
            StmtKind::Delete(Expr::Var(name)) => {
                // Deleting a variable disposes its instance: assign VOID.
                output.push(Stmt {
                    line,
                    kind: StmtKind::Set { target: Expr::Var(name), value: Expr::Var("void".into()) },
                });
            }
            StmtKind::Unrecovered { opcode } => {
                // The pinned decompiler marked untranslatable bytecode; lower
                // to the explicit alert-and-continue runtime failure.
                output.push(call("undecompiled_bytecode", vec![Expr::Str(opcode)]));
            }
            StmtKind::ForEach { variable, sequence, body } => {
                let seq = temporary("sequence", serial);
                let bound = temporary("count", serial);
                let index = temporary("index", serial);
                output.push(Stmt { line, kind: StmtKind::Set { target: Expr::Var(seq.clone()), value: sequence } });
                output.push(Stmt {
                    line,
                    kind: StmtKind::Set {
                        target: Expr::Var(bound.clone()),
                        value: Expr::Call("count".into(), vec![Expr::Var(seq.clone())]),
                    },
                });
                let mut inner = vec![Stmt {
                    line,
                    kind: StmtKind::Set {
                        target: Expr::Var(variable),
                        value: Expr::Call("getat".into(), vec![Expr::Var(seq), Expr::Var(index.clone())]),
                    },
                }];
                inner.extend(body);
                output.push(Stmt {
                    line,
                    kind: StmtKind::For {
                        variable: index,
                        first: Expr::Number(Num::Int(1)),
                        last: Expr::Var(bound),
                        step: 1,
                        body: inner,
                    },
                });
            }
            StmtKind::Case { selector, branches } => {
                let name = temporary("case", serial);
                let mut tail: Vec<Stmt> = Vec::new();
                for branch in branches.iter().rev() {
                    let body = block(&branch.body, serial);
                    let Some(values) = &branch.values else {
                        tail = body;
                        continue;
                    };
                    let mut conditions = values
                        .iter()
                        .map(|v| Expr::Binary("=".into(), Box::new(Expr::Var(name.clone())), Box::new(v.clone())));
                    let mut condition = conditions.next().expect("case branch without values");
                    for extra in conditions {
                        condition = Expr::Binary("or".into(), Box::new(condition), Box::new(extra));
                    }
                    tail = vec![Stmt { line, kind: StmtKind::If { condition, yes: body, no: tail } }];
                }
                output.push(Stmt { line, kind: StmtKind::Set { target: Expr::Var(name), value: selector } });
                output.extend(tail);
            }
            other => output.push(Stmt { line, kind: other }),
        }
    }
    output
}

/// `the frame` and `the lastFrame` inside a sprite tell read that sprite's
/// timeline.
fn retarget(value: &Expr, channel: &str) -> Expr {
    let chan = || vec![Expr::Var(channel.to_string())];
    match value {
        Expr::The(name) if name == "frame" => Expr::Call("sprite_frame".into(), chan()),
        Expr::The(name) if name == "lastframe" => Expr::Call("sprite_lastframe".into(), chan()),
        Expr::Unary(op, e) => Expr::Unary(op.clone(), Box::new(retarget(e, channel))),
        Expr::Binary(op, a, b) => {
            Expr::Binary(op.clone(), Box::new(retarget(a, channel)), Box::new(retarget(b, channel)))
        }
        Expr::List(items) => Expr::List(items.iter().map(|e| retarget(e, channel)).collect()),
        Expr::PropList(pairs) => Expr::PropList(
            pairs.iter().map(|(k, v)| (retarget(k, channel), retarget(v, channel))).collect(),
        ),
        Expr::Property { name, kind, value } => Expr::Property {
            name: name.clone(),
            kind: kind.clone(),
            value: Box::new(retarget(value, channel)),
        },
        Expr::Get(name, e) => Expr::Get(name.clone(), Box::new(retarget(e, channel))),
        Expr::Index(a, b) => Expr::Index(Box::new(retarget(a, channel)), Box::new(retarget(b, channel))),
        Expr::LastChunk(kind, e) => Expr::LastChunk(kind.clone(), Box::new(retarget(e, channel))),
        Expr::Method { name, receiver, args } => Expr::Method {
            name: name.clone(),
            receiver: Box::new(retarget(receiver, channel)),
            args: args.iter().map(|e| retarget(e, channel)).collect(),
        },
        Expr::Reference { kind, number, library } => Expr::Reference {
            kind: kind.clone(),
            number: Box::new(retarget(number, channel)),
            library: library.as_ref().map(|l| Box::new(retarget(l, channel))),
        },
        Expr::Chunk { kind, first, last, source } => Expr::Chunk {
            kind: kind.clone(),
            first: Box::new(retarget(first, channel)),
            last: Box::new(retarget(last, channel)),
            source: Box::new(retarget(source, channel)),
        },
        Expr::Call(name, args) => Expr::Call(name.clone(), args.iter().map(|e| retarget(e, channel)).collect()),
        other => other.clone(),
    }
}

/// Handler sends inside a sprite tell go to the sprite's behaviors.
fn resprite(statements: &[Stmt], channel: &str) -> Vec<Stmt> {
    let rt = |e: &Expr| retarget(e, channel);
    statements
        .iter()
        .map(|inner| {
            let kind = match &inner.kind {
                StmtKind::If { condition, yes, no } => StmtKind::If {
                    condition: rt(condition),
                    yes: resprite(yes, channel),
                    no: resprite(no, channel),
                },
                StmtKind::While { condition, body } => {
                    StmtKind::While { condition: rt(condition), body: resprite(body, channel) }
                }
                StmtKind::For { variable, first, last, step, body } => StmtKind::For {
                    variable: variable.clone(),
                    first: rt(first),
                    last: rt(last),
                    step: *step,
                    body: resprite(body, channel),
                },
                StmtKind::Return(value) => StmtKind::Return(value.as_ref().map(rt)),
                StmtKind::Call(value) => match rt(value) {
                    Expr::Call(name, args) if name == "go" && args.len() == 1 => StmtKind::Call(Expr::Call(
                        "sprite_go".into(),
                        vec![Expr::Var(channel.to_string()), args[0].clone()],
                    )),
                    Expr::Call(name, args)
                        if !matches!(name.as_str(), "sendsprite" | "sprite_go" | "sprite_frame" | "sprite_lastframe") =>
                    {
                        let mut sent = vec![Expr::Var(channel.to_string()), Expr::Sym(name)];
                        sent.extend(args);
                        StmtKind::Call(Expr::Call("sendsprite".into(), sent))
                    }
                    other => StmtKind::Call(other),
                },
                StmtKind::Delete(target) => StmtKind::Delete(rt(target)),
                StmtKind::Set { target, value } => StmtKind::Set { target: rt(target), value: rt(value) },
                StmtKind::Trace(value) => StmtKind::Trace(rt(value)),
                StmtKind::Tell { target, body } => {
                    StmtKind::Tell { target: rt(target), body: resprite(body, channel) }
                }
                StmtKind::ForEach { variable, sequence, body } => StmtKind::ForEach {
                    variable: variable.clone(),
                    sequence: rt(sequence),
                    body: resprite(body, channel),
                },
                StmtKind::Case { selector, branches } => StmtKind::Case {
                    selector: rt(selector),
                    branches: branches.clone(),
                },
                other => other.clone(),
            };
            Stmt { line: inner.line, kind }
        })
        .collect()
}

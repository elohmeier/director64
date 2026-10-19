//! Compile a program into per-movie bytecode units, handler tables and the
//! symbol layout the game package carries.

use std::collections::{BTreeSet, HashMap, HashSet};

use sha2::{Digest, Sha256};


use crate::ast::{Handler, Program, StmtKind, for_each_stmt};
use crate::compiler::{Compiler, Corpus, Interner, Sibling, polling_cycles};
use crate::names::Names;
use crate::normalize::normalize;

/// FNV-1a over the ASCII-folded name, matching `lv_text_hash` in
/// runtime/lingo/lingo_runtime.c: the key of every generated name index.
pub fn text_hash(name: &str) -> u32 {
    let mut digest: u32 = 2166136261;
    for byte in name.bytes() {
        let folded = if byte.is_ascii_uppercase() {
            byte + 32
        } else {
            byte
        };
        digest = (digest ^ folded as u32).wrapping_mul(16777619);
    }
    digest
}

/// The runtime's handler-table index: entry indices grouped by name bucket
/// in table order, and the bucket start offsets (one more than the bucket
/// count). Buckets are the smallest power of two holding the table at a
/// load of one.
pub fn handler_index(names: &[&str]) -> (Vec<usize>, Vec<usize>) {
    let mut buckets = 1usize;
    while buckets < names.len() {
        buckets *= 2;
    }
    let mut chains: Vec<Vec<usize>> = vec![Vec::new(); buckets];
    for (index, name) in names.iter().enumerate() {
        chains[(text_hash(name) as usize) & (buckets - 1)].push(index);
    }
    let mut order = Vec::new();
    let mut starts = Vec::with_capacity(buckets + 1);
    for chain in &chains {
        starts.push(order.len());
        order.extend_from_slice(chain);
    }
    starts.push(order.len());
    (order, starts)
}

/// The corpus symbol table (runtime/lingo/lingo_runtime.h, lb_symbols):
/// ids in bucket order, so bucket b of the power-of-two bucket count is the
/// id range [buckets[b], buckets[b + 1]) and a lookup by text reads one
/// bucket. Returns each name's id and the generated unit.
/// A symbol's identity folds case: `#shopFloor` and `#shopfloor` are one
/// symbol, spelled the way it was first interned.
pub fn fold(name: &str) -> String {
    name.to_ascii_lowercase()
}

/// One handler's table entry, as the runtime's lv_handler_t holds it.
pub struct HandlerEntry {
    pub name: String,
    pub member: i64,
    pub cast: String,
    pub kind: String,
    pub arguments: usize,
    pub locals: Vec<String>,
    pub entry: usize,
    pub properties: Vec<String>,
    /// Index into the unit's deduplicated code blocks.
    pub block: usize,
}

/// One movie's compiled bytecode: what a generated C unit or a package
/// section holds.
pub struct Unit {
    pub stem: String,
    pub symbol: String,
    pub movie: String,
    /// Deduplicated code blocks: generated name, bytes and owning handlers.
    pub blocks: Vec<(String, Vec<u8>, Vec<String>)>,
    pub entries: Vec<HandlerEntry>,
    pub doubles: Vec<f64>,
    pub order: Vec<usize>,
    pub buckets: Vec<usize>,
    pub names: Vec<String>,
    pub symbol_use: Vec<bool>,
    pub bytes: usize,
    pub local_calls: usize,
    pub builtin_calls: usize,
    pub dynamic_calls: usize,
}

impl Unit {
    /// The bucket count of the handler index (0 without handlers).
    pub fn bucket_count(&self) -> usize {
        if self.entries.is_empty() {
            0
        } else {
            self.buckets.len() - 1
        }
    }
}

/// A whole program compiled: every movie's unit, the corpus symbol table
/// and the global slots.
pub struct Compiled {
    pub units: Vec<Unit>,
    /// Symbols in first-spelled order (the table's input).
    pub symbol_names: Vec<String>,
    pub symbol_ids: HashMap<String, usize>,
    /// The table itself: texts in id order and bucket starts.
    pub symbols: Vec<String>,
    pub symbol_buckets: Vec<usize>,
    pub symbol_bucket_count: usize,
    /// Declared globals in slot order.
    pub globals: Vec<String>,
    pub ancestors: bool,
    pub bytecode_bytes: usize,
}

/// The symbol table's layout: texts in id order, bucket starts, bucket
/// count, and each folded name's id.
pub fn symbol_layout(names: &[String]) -> (Vec<String>, Vec<usize>, usize, HashMap<String, usize>) {
    let mut bucket_count = 1usize;
    while bucket_count < names.len() {
        bucket_count *= 2;
    }
    let mut chains: Vec<Vec<usize>> = vec![Vec::new(); bucket_count];
    for (index, name) in names.iter().enumerate() {
        chains[(text_hash(name) as usize) & (bucket_count - 1)].push(index);
    }
    let mut ids = HashMap::new();
    let mut ordered: Vec<String> = Vec::with_capacity(names.len());
    let mut starts = Vec::with_capacity(bucket_count + 1);
    for chain in &chains {
        starts.push(ordered.len());
        for &index in chain {
            ids.insert(fold(&names[index]), ordered.len());
            ordered.push(names[index].clone());
        }
    }
    starts.push(ordered.len());
    (ordered, starts, bucket_count, ids)
}

pub fn compile_program(program: &Program, names: &Names) -> Result<Compiled, String> {
    let handlers: Vec<Handler> = program
        .handlers
        .iter()
        .map(|h| Handler {
            body: normalize(&h.body),
            ..h.clone()
        })
        .collect();
    let mut declared: BTreeSet<String> = BTreeSet::new();
    for handler in &handlers {
        for_each_stmt(&handler.body, &mut |stmt| {
            if let StmtKind::Global(names) = &stmt.kind {
                declared.extend(names.iter().cloned());
            }
        });
    }
    let globals: HashMap<String, usize> = declared
        .iter()
        .enumerate()
        .map(|(i, n)| (n.clone(), i))
        .collect();
    if globals.len() > 1024 {
        return Err("global budget exceeded".into());
    }
    let custom_calls: HashSet<String> = handlers.iter().map(|h| h.name.clone()).collect();
    let corpus = Corpus {
        handler_names: &custom_calls,
        ancestors: handlers
            .iter()
            .any(|h| format!("{:?}", h.body).to_lowercase().contains("ancestor")),
    };
    let mut movies: BTreeSet<String> = handlers.iter().map(|h| h.movie.clone()).collect();
    movies.extend(
        program
            .files
            .iter()
            .map(|f| f.strip_suffix(".lingo").unwrap_or(f).to_string()),
    );
    let mut total_bytes = 0usize;
    // Every symbol the corpus spells gets one id, so the runtime compares
    // symbols and property keys as integers and never interns a literal.
    // The runtime's own vocabulary comes first: the names its services look
    // up are symbols too. Ids are assigned after every movie is compiled,
    // in bucket order (see symbol_table), so each movie's unit is finished
    // in a second pass.
    let mut symbol_names: Vec<String> = Vec::new();
    let mut symbol_seen: HashSet<String> = HashSet::new();
    for name in names.all() {
        if symbol_seen.insert(fold(name)) {
            symbol_names.push(name.clone());
        }
    }
    let mut units: Vec<Unit> = Vec::new();
    for movie in &movies {
        let mut blocks: Vec<(String, Vec<u8>, Vec<String>)> = Vec::new();
        let mut unique: HashMap<[u8; 32], usize> = HashMap::new();
        let mut entries: Vec<HandlerEntry> = Vec::new();
        let mut interner = Interner::default();
        let movie_handlers: Vec<&Handler> = handlers.iter().filter(|h| &h.movie == movie).collect();
        let cycles = polling_cycles(&movie_handlers);
        let siblings: Vec<Sibling> = movie_handlers
            .iter()
            .map(|h| Sibling {
                name: h.name.clone(),
                member: h.member,
                cast: h.cast.clone(),
                movie_script: h.script_type == "MovieScript",
            })
            .collect();
        let (mut local_calls, mut builtin_calls, mut dynamic_calls) = (0usize, 0usize, 0usize);
        for handler in &movie_handlers {
            let compiler = Compiler::new(
                handler,
                &globals,
                &corpus,
                &siblings,
                &mut interner,
                cycles.get(&handler.name.to_lowercase()).cloned(),
            )?;
            let locals = compiler.locals.clone();
            let (entry, code, calls) = compiler.compile()?;
            local_calls += calls.0;
            builtin_calls += calls.1;
            dynamic_calls += calls.2;
            let mut hasher = Sha256::new();
            hasher.update(&code);
            hasher.update((entry as u32).to_be_bytes());
            let digest: [u8; 32] = hasher.finalize().into();
            let block = *unique.entry(digest).or_insert_with(|| {
                blocks.push((format!("code_{}", blocks.len()), code.clone(), Vec::new()));
                blocks.len() - 1
            });
            blocks[block].2.push(format!(
                "{} ({} member {})",
                handler.name, handler.cast, handler.member
            ));
            entries.push(HandlerEntry {
                name: handler.name.clone(),
                member: handler.member,
                cast: handler.cast.clone(),
                kind: handler.script_type.clone(),
                arguments: handler.parameters.len(),
                locals,
                entry,
                properties: handler.properties.clone(),
                block,
            });
        }
        let stem = movie.replace('.', "_").to_lowercase();
        let symbol = format!("aot_{stem}");
        // The handler table's index by name (lv_movie_t.handler_order and
        // handler_buckets): a lookup reads one bucket instead of the table.
        let handler_names: Vec<&str> = movie_handlers.iter().map(|h| h.name.as_str()).collect();
        let (order, buckets) = handler_index(&handler_names);
        for (name, used) in interner.names.iter().zip(&interner.symbol_use) {
            if *used && symbol_seen.insert(fold(name)) {
                symbol_names.push(name.clone());
            }
        }
        let bytes: usize = blocks.iter().map(|(_, code, _)| code.len()).sum();
        total_bytes += bytes;
        units.push(Unit {
            stem,
            symbol,
            movie: movie.clone(),
            blocks,
            entries,
            doubles: interner.doubles.clone(),
            order,
            buckets,
            names: interner.names.clone(),
            symbol_use: interner.symbol_use.clone(),
            bytes,
            local_calls,
            builtin_calls,
            dynamic_calls,
        });
    }
    let (symbols, symbol_buckets, symbol_bucket_count, symbol_ids) = symbol_layout(&symbol_names);
    Ok(Compiled {
        units,
        symbol_names,
        symbol_ids,
        symbols,
        symbol_buckets,
        symbol_bucket_count,
        globals: declared.into_iter().collect(),
        ancestors: corpus.ancestors,
        bytecode_bytes: total_bytes,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The runtime's lv_text_hash and the Python member-name hash agree on
    /// these (tests/native/test_lingo.c asserts the same constants).
    #[test]
    fn text_hash_folds_case_like_the_runtime() {
        assert_eq!(text_hash("exitframe"), 945048422);
        assert_eq!(text_hash("ExitFrame"), 945048422);
        assert_eq!(text_hash(""), 2166136261);
        assert_ne!(text_hash("a"), text_hash("b"));
    }

    #[test]
    fn handler_index_groups_entries_by_name_bucket_in_table_order() {
        let names = [
            "exitframe",
            "ExitFrame",
            "enterframe",
            "helper",
            "helper",
            "new",
        ];
        let (order, buckets) = handler_index(&names);
        assert_eq!(buckets.len(), 9);
        assert_eq!((buckets[0], buckets[8]), (0, names.len()));
        let mut seen = order.clone();
        seen.sort();
        assert_eq!(seen, (0..names.len()).collect::<Vec<_>>());
        for bucket in 0..8 {
            let chain = &order[buckets[bucket]..buckets[bucket + 1]];
            assert!(chain.windows(2).all(|w| w[0] < w[1]));
            for &index in chain {
                assert_eq!((text_hash(names[index]) as usize) & 7, bucket);
            }
        }
        assert_eq!(handler_index(&[]), (vec![], vec![0, 0]));
        assert_eq!(handler_index(&["one"]), (vec![0], vec![0, 1]));
    }
}

//! Write a program's bytecode units: one C file and one listing per movie,
//! the globals table and the manifest, byte for byte what the Python
//! generator wrote.

use std::collections::{BTreeSet, HashMap, HashSet};
use std::path::Path;

use serde_json::{json, Map, Value};
use sha2::{Digest, Sha256};

use crate::ast::{for_each_stmt, Handler, Program, StmtKind};
use crate::bytecode::listing;
use crate::compiler::{polling_cycles, Compiler, Corpus, Interner, Sibling};
use crate::names::Names;
use crate::normalize::normalize;
use crate::pyfmt::{cstring, float_repr, json_dumps};

pub struct Summary {
    pub movies: usize,
    pub bytecode_bytes: usize,
    pub globals: usize,
}

/// FNV-1a over the ASCII-folded name, matching `lv_text_hash` in
/// runtime/lingo/lingo_runtime.c: the key of every generated name index.
pub fn text_hash(name: &str) -> u32 {
    let mut digest: u32 = 2166136261;
    for byte in name.bytes() {
        let folded = if byte.is_ascii_uppercase() { byte + 32 } else { byte };
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

fn hex(digest: &[u8]) -> String {
    digest.iter().map(|b| format!("{b:02x}")).collect()
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
        if self.entries.is_empty() { 0 } else { self.buckets.len() - 1 }
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
        .map(|h| Handler { body: normalize(&h.body), ..h.clone() })
        .collect();
    let mut declared: BTreeSet<String> = BTreeSet::new();
    for handler in &handlers {
        for_each_stmt(&handler.body, &mut |stmt| {
            if let StmtKind::Global(names) = &stmt.kind {
                declared.extend(names.iter().cloned());
            }
        });
    }
    let globals: HashMap<String, usize> = declared.iter().enumerate().map(|(i, n)| (n.clone(), i)).collect();
    if globals.len() > 1024 {
        return Err("global budget exceeded".into());
    }
    let custom_calls: HashSet<String> = handlers.iter().map(|h| h.name.clone()).collect();
    let corpus = Corpus {
        handler_names: &custom_calls,
        ancestors: handlers.iter().any(|h| format!("{:?}", h.body).to_lowercase().contains("ancestor")),
    };
    let mut movies: BTreeSet<String> = handlers.iter().map(|h| h.movie.clone()).collect();
    movies.extend(program.files.iter().map(|f| f.strip_suffix(".lingo").unwrap_or(f).to_string()));
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
            blocks[block].2.push(format!("{} ({} member {})", handler.name, handler.cast, handler.member));
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

fn unit_source(unit: &Unit, compiled: &Compiled, names: &Names) -> String {
    let mut source = String::from("#include \"lingo_runtime.h\"\n");
    for (name, code, _) in &unit.blocks {
        source.push_str(&format!(
            "static const uint8_t {name}[]={{{}}};\n",
            code.iter().map(|b| b.to_string()).collect::<Vec<_>>().join(",")
        ));
    }
    let entries: Vec<String> = unit
        .entries
        .iter()
        .map(|e| {
            let local_names = if e.locals.is_empty() {
                "NULL".to_string()
            } else {
                e.locals.iter().map(|n| cstring(n)).collect::<Vec<_>>().join(",")
            };
            let property_names = if e.properties.is_empty() {
                "NULL".to_string()
            } else {
                e.properties.iter().map(|n| cstring(n)).collect::<Vec<_>>().join(",")
            };
            let name = &unit.blocks[e.block].0;
            format!(
                "{{{},{},{},{},{},{},{},NULL,(const char *const[]){{{}}},{},(const char *const[]){{{}}},{},sizeof({})}}",
                cstring(&e.name),
                e.member,
                cstring(&e.cast),
                cstring(&e.kind),
                e.arguments,
                e.locals.len(),
                e.entry,
                local_names,
                e.properties.len(),
                property_names,
                name,
                name
            )
        })
        .collect();
    source.push_str(&format!(
        "static const lv_handler_t entries[]={{ {} }};\n",
        if entries.is_empty() { "{0}".to_string() } else { entries.join(",") }
    ));
    if !unit.doubles.is_empty() {
        source.push_str(&format!(
            "static const double lx_doubles[]={{{}}};\n",
            unit.doubles.iter().map(|v| float_repr(*v)).collect::<Vec<_>>().join(",")
        ));
    }
    if !unit.entries.is_empty() {
        source.push_str(&format!(
            "static const uint16_t lx_handler_order[]={{{}}};\nstatic const uint16_t lx_handler_buckets[]={{{}}};\n",
            unit.order.iter().map(|i| i.to_string()).collect::<Vec<_>>().join(","),
            unit.buckets.iter().map(|i| i.to_string()).collect::<Vec<_>>().join(","),
        ));
    }
    // Every pooled name with its dispatch id and its symbol id
    // (LV_NO_SYMBOL where no op reads the name as a symbol).
    if !unit.names.is_empty() {
        let records: Vec<String> = unit
            .names
            .iter()
            .zip(&unit.symbol_use)
            .map(|(name, used)| {
                format!(
                    "{{{},{},{}}}",
                    cstring(name),
                    names.id(name),
                    if *used { compiled.symbol_ids[&fold(name)].to_string() } else { "65535".to_string() }
                )
            })
            .collect();
        source.push_str(&format!("static const lv_name_t lx_names[]={{{}}};\n", records.join(",")));
    }
    let has_entries = !unit.entries.is_empty();
    source.push_str(&format!(
        "const lv_movie_t {}={{{}, {}, entries, {}, {}, {}, {}, {}}};\n",
        unit.symbol,
        cstring(&unit.movie),
        unit.entries.len(),
        if unit.names.is_empty() { "NULL" } else { "lx_names" },
        if unit.doubles.is_empty() { "NULL" } else { "lx_doubles" },
        if has_entries { "lx_handler_order" } else { "NULL" },
        if has_entries { "lx_handler_buckets" } else { "NULL" },
        unit.bucket_count(),
    ));
    source
}

fn symbols_source(compiled: &Compiled) -> String {
    let mut source = String::from("#include \"lingo_runtime.h\"\n");
    source.push_str(&format!(
        "static const char *const text[]={{{}}};\n",
        if compiled.symbols.is_empty() {
            "NULL".to_string()
        } else {
            compiled.symbols.iter().map(|n| cstring(n)).collect::<Vec<_>>().join(",")
        }
    ));
    source.push_str(&format!(
        "static const uint16_t buckets[]={{{}}};\n",
        compiled.symbol_buckets.iter().map(|i| i.to_string()).collect::<Vec<_>>().join(",")
    ));
    source.push_str(&format!(
        "const lb_symbol_table_t lb_symbols={{text, {}, buckets, {}}};\n",
        compiled.symbols.len(),
        compiled.symbol_bucket_count
    ));
    source
}

pub fn generate(program: &Program, output: &Path, names: &Names) -> Result<Summary, String> {
    let compiled = compile_program(program, names)?;
    std::fs::create_dir_all(output).map_err(|e| format!("{}: {e}", output.display()))?;
    std::fs::write(output.join("symbols.c"), symbols_source(&compiled)).map_err(|e| e.to_string())?;
    let mut specs = Vec::new();
    for unit in &compiled.units {
        let source = unit_source(unit, &compiled, names);
        let mut text = format!("# {} bytecode\n", unit.movie);
        for (name, code, owners) in &unit.blocks {
            text.push_str(&format!("\n{name}: {}\n{}", owners.join("; "), listing(code)?));
        }
        std::fs::write(output.join(format!("{}.c", unit.stem)), &source).map_err(|e| e.to_string())?;
        std::fs::write(output.join(format!("{}.lst", unit.stem)), &text).map_err(|e| e.to_string())?;
        specs.push(json!({
            "movie": unit.movie,
            "file": format!("{}.c", unit.stem),
            "symbol": unit.symbol,
            "sha256": hex(&Sha256::digest(source.as_bytes())),
            "handlers": unit.entries.len(),
            "native_functions": unit.blocks.len(),
            "bytecode_bytes": unit.bytes,
            "local_calls": unit.local_calls,
            "builtin_calls": unit.builtin_calls,
            "dynamic_calls": unit.dynamic_calls,
        }));
    }
    let mut globals_json = Map::new();
    for (i, name) in compiled.globals.iter().enumerate() {
        globals_json.insert(name.clone(), Value::from(i));
    }
    let globals_value = Value::Object(globals_json);
    std::fs::write(output.join("globals.json"), json_dumps(&globals_value) + "\n").map_err(|e| e.to_string())?;
    std::fs::write(
        output.join("globals.inc"),
        compiled.globals.iter().map(|n| cstring(n)).collect::<Vec<_>>().join(",\n") + "\n",
    )
    .map_err(|e| e.to_string())?;
    let mut manifest = Map::new();
    manifest.insert("schema_version".into(), Value::from(1));
    manifest.insert("movies".into(), Value::Array(specs));
    manifest.insert("symbols".into(), Value::from(compiled.symbol_names.len()));
    manifest.insert("ancestors".into(), Value::Bool(compiled.ancestors));
    manifest.insert("globals".into(), globals_value);
    manifest.insert("native_execution_verified".into(), Value::Bool(false));
    if let Some(fixes) = &program.compatibility_fixes {
        manifest.insert("compatibility_fixes".into(), fixes.clone());
    }
    std::fs::write(output.join("manifest.json"), json_dumps(&Value::Object(manifest)) + "\n")
        .map_err(|e| e.to_string())?;
    Ok(Summary { movies: compiled.units.len(), bytecode_bytes: compiled.bytecode_bytes, globals: compiled.globals.len() })
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
        let names = ["exitframe", "ExitFrame", "enterframe", "helper", "helper", "new"];
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

//! The runtime's name vocabulary (runtime/lingo/names.txt): sorted, unique,
//! lower-case identifiers whose position is the id the runtime dispatches on.

use std::collections::HashMap;
use std::path::Path;

pub struct Names {
    ids: HashMap<String, u16>,
    list: Vec<String>,
}

impl Names {
    /// The id of a pooled name, or the count (the runtime's "no such name").
    pub fn id(&self, name: &str) -> u16 {
        // The vocabulary is lower case; a name reaches here as spelled.
        let folded = name.to_ascii_lowercase();
        self.ids.get(&folded).copied().unwrap_or(self.ids.len() as u16)
    }
    /// Every name, in id order.
    pub fn all(&self) -> &[String] {
        &self.list
    }
}

pub fn load(path: &Path) -> Result<Names, String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    parse(&text)
}

pub fn parse(text: &str) -> Result<Names, String> {
    let names: Vec<&str> = text.lines().map(str::trim).filter(|l| !l.is_empty()).collect();
    let mut sorted = names.clone();
    sorted.sort();
    sorted.dedup();
    if sorted != names {
        return Err("names.txt must be sorted and unique".into());
    }
    for name in &names {
        let mut chars = name.chars();
        let head = chars.next().map_or(false, |c| c.is_ascii_lowercase() || c == '_');
        if !head || !chars.all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_') {
            return Err(format!("name {name:?} is not a lower-case identifier"));
        }
    }
    Ok(Names {
        ids: names.iter().enumerate().map(|(i, n)| (n.to_string(), i as u16)).collect(),
        list: names.iter().map(|n| n.to_string()).collect(),
    })
}

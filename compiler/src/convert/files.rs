//! The file system the converter stages read and write: the host's on the
//! command line, an in-memory one in the browser. Paths are POSIX-style and
//! relative to the stage's root.

use std::collections::{BTreeMap, BTreeSet};
use std::path::PathBuf;

pub trait Files {
    fn read(&self, path: &str) -> Result<Vec<u8>, String>;
    fn write(&mut self, path: &str, data: &[u8]) -> Result<(), String>;
    fn exists(&self, path: &str) -> bool;
    fn is_file(&self, path: &str) -> bool;
    /// Directory entries (name, is anything but a regular file: a
    /// directory, or on disk also a symlink or device), sorted by name.
    fn list(&self, path: &str) -> Result<Vec<(String, bool)>, String>;
    fn mkdir_all(&mut self, path: &str) -> Result<(), String>;
    fn remove(&mut self, path: &str) -> Result<(), String>;
    fn size(&self, path: &str) -> Result<u64, String> {
        Ok(self.read(path)?.len() as u64)
    }
    fn read_text(&self, path: &str) -> Result<String, String> {
        String::from_utf8(self.read(path)?).map_err(|_| format!("{path}: not UTF-8"))
    }
}

/// POSIX normalization: "." and ".." resolved, no trailing slash.
pub fn normalize(path: &str) -> String {
    let absolute = path.starts_with('/');
    let mut parts: Vec<&str> = Vec::new();
    for part in path.split('/') {
        match part {
            "" | "." => {}
            ".." => {
                if parts.last().is_some_and(|p| *p != "..") {
                    parts.pop();
                } else if !absolute {
                    parts.push("..");
                }
            }
            p => parts.push(p),
        }
    }
    let joined = parts.join("/");
    if absolute { format!("/{joined}") } else if joined.is_empty() { ".".into() } else { joined }
}

pub fn join(a: &str, b: &str) -> String {
    if b.starts_with('/') { normalize(b) } else { normalize(&format!("{a}/{b}")) }
}

pub fn parent(path: &str) -> String {
    let path = normalize(path);
    match path.rfind('/') {
        Some(0) => "/".into(),
        Some(i) => path[..i].into(),
        None => ".".into(),
    }
}

pub fn file_name(path: &str) -> &str {
    path.rsplit('/').next().unwrap_or(path)
}

pub struct Disk {
    pub root: PathBuf,
}

impl Disk {
    fn at(&self, path: &str) -> PathBuf {
        let path = normalize(path);
        if path.starts_with('/') { PathBuf::from(path) } else { self.root.join(path) }
    }
}

impl Files for Disk {
    fn read(&self, path: &str) -> Result<Vec<u8>, String> {
        std::fs::read(self.at(path)).map_err(|e| format!("{path}: {e}"))
    }
    fn write(&mut self, path: &str, data: &[u8]) -> Result<(), String> {
        std::fs::write(self.at(path), data).map_err(|e| format!("{path}: {e}"))
    }
    fn exists(&self, path: &str) -> bool {
        self.at(path).exists()
    }
    fn is_file(&self, path: &str) -> bool {
        self.at(path).is_file()
    }
    fn list(&self, path: &str) -> Result<Vec<(String, bool)>, String> {
        let mut out = Vec::new();
        for entry in std::fs::read_dir(self.at(path)).map_err(|e| format!("{path}: {e}"))? {
            let entry = entry.map_err(|e| e.to_string())?;
            let name = entry.file_name().into_string().map_err(|_| "non-UTF-8 file name".to_string())?;
            out.push((name, !entry.file_type().map_err(|e| e.to_string())?.is_file()));
        }
        out.sort();
        Ok(out)
    }
    fn mkdir_all(&mut self, path: &str) -> Result<(), String> {
        std::fs::create_dir_all(self.at(path)).map_err(|e| format!("{path}: {e}"))
    }
    fn remove(&mut self, path: &str) -> Result<(), String> {
        let at = self.at(path);
        if at.is_dir() {
            std::fs::remove_dir_all(at).map_err(|e| e.to_string())
        } else if at.exists() {
            std::fs::remove_file(at).map_err(|e| e.to_string())
        } else {
            Ok(())
        }
    }
    fn size(&self, path: &str) -> Result<u64, String> {
        std::fs::metadata(self.at(path)).map(|m| m.len()).map_err(|e| format!("{path}: {e}"))
    }
}

#[derive(Default)]
pub struct Memory {
    pub files: BTreeMap<String, Vec<u8>>,
    pub dirs: BTreeSet<String>,
}

impl Memory {
    pub fn new() -> Self {
        let mut memory = Self::default();
        memory.dirs.insert("/".into());
        memory
    }
    fn key(path: &str) -> String {
        let path = normalize(path);
        if path.starts_with('/') { path } else { format!("/{path}") }
    }
    pub fn bytes(&self) -> usize {
        self.files.values().map(Vec::len).sum()
    }
}

impl Files for Memory {
    fn read(&self, path: &str) -> Result<Vec<u8>, String> {
        self.files.get(&Self::key(path)).cloned().ok_or_else(|| format!("{path}: no such file"))
    }
    fn write(&mut self, path: &str, data: &[u8]) -> Result<(), String> {
        let key = Self::key(path);
        if !self.dirs.contains(&parent(&key)) {
            return Err(format!("{path}: no such directory"));
        }
        self.files.insert(key, data.to_vec());
        Ok(())
    }
    fn exists(&self, path: &str) -> bool {
        let key = Self::key(path);
        self.files.contains_key(&key) || self.dirs.contains(&key)
    }
    fn is_file(&self, path: &str) -> bool {
        self.files.contains_key(&Self::key(path))
    }
    fn list(&self, path: &str) -> Result<Vec<(String, bool)>, String> {
        let key = Self::key(path);
        if !self.dirs.contains(&key) {
            return Err(format!("{path}: no such directory"));
        }
        let prefix = if key == "/" { "/".to_string() } else { format!("{key}/") };
        let mut out: Vec<(String, bool)> = Vec::new();
        for dir in self.dirs.range(prefix.clone()..) {
            if !dir.starts_with(&prefix) {
                break;
            }
            let rest = &dir[prefix.len()..];
            if !rest.is_empty() && !rest.contains('/') {
                out.push((rest.to_string(), true));
            }
        }
        for file in self.files.range(prefix.clone()..) {
            if !file.0.starts_with(&prefix) {
                break;
            }
            let rest = &file.0[prefix.len()..];
            if !rest.contains('/') {
                out.push((rest.to_string(), false));
            }
        }
        out.sort();
        Ok(out)
    }
    fn mkdir_all(&mut self, path: &str) -> Result<(), String> {
        let mut key = Self::key(path);
        while key != "/" && !self.dirs.contains(&key) {
            if self.files.contains_key(&key) {
                return Err(format!("{path}: a file is in the way"));
            }
            self.dirs.insert(key.clone());
            key = parent(&key);
        }
        Ok(())
    }
    fn remove(&mut self, path: &str) -> Result<(), String> {
        let key = Self::key(path);
        let prefix = format!("{key}/");
        self.files.retain(|k, _| *k != key && !k.starts_with(&prefix));
        self.dirs.retain(|k| *k != key && !k.starts_with(&prefix));
        Ok(())
    }
    fn size(&self, path: &str) -> Result<u64, String> {
        self.files.get(&Self::key(path)).map(|d| d.len() as u64).ok_or_else(|| format!("{path}: no such file"))
    }
}

//! Audited source files and Xtra envelopes (was tests/node/source-files.test.mjs).

mod common;

use common::*;
use director64_aot::convert::files::Disk;
use director64_aot::convert::source::source_files;
use director64_aot::convert::xtra::xtra_model;
use std::path::PathBuf;

struct Scratch(PathBuf);
impl Scratch {
    fn new(name: &str) -> Self {
        let path = std::env::temp_dir().join(format!("director64-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(&path).unwrap();
        Self(path)
    }
    fn write(&self, name: &str, text: &str) {
        std::fs::write(self.0.join(name), text).unwrap();
    }
    fn sources(&self, directories: &[&str]) -> Result<Vec<(String, String)>, String> {
        let fs = Disk { root: self.0.clone() };
        let directories: Vec<String> = directories.iter().map(|d| d.to_string()).collect();
        Ok(source_files(&fs, ".", &directories)?.into_iter().map(|s| (s.name, s.path)).collect())
    }
}
impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn pairs(items: &[(&str, &str)]) -> Vec<(String, String)> {
    items.iter().map(|(a, b)| (a.to_string(), b.to_string())).collect()
}

#[test]
fn nested_casts_retain_original_paths_in_a_collision_free_runtime_namespace() {
    let root = Scratch::new("sources");
    std::fs::create_dir(root.0.join("CASTS")).unwrap();
    root.write("MAIN.DXR", "movie");
    root.write("CASTS/shared.cxt", "cast");
    root.write("CASTS/ignored.txt", "ordinary file");
    assert_eq!(root.sources(&["."]).unwrap(), pairs(&[("MAIN.DXR", "MAIN.DXR")]));
    assert_eq!(root.sources(&[".", "CASTS"]).unwrap(),
        pairs(&[("MAIN.DXR", "MAIN.DXR"), ("SHARED.CXT", "CASTS/shared.cxt")]));
    root.write("SHARED.CXT", "ambiguous cast");
    assert!(root.sources(&[".", "CASTS"]).unwrap_err().contains("duplicate"));
    assert!(root.sources(&[".."]).unwrap_err().contains("escapes media root"));
    std::os::unix::fs::symlink(root.0.join("MAIN.DXR"), root.0.join("ALIAS.DXR")).unwrap();
    assert!(root.sources(&["."]).unwrap_err().contains("not a regular file"));
}

#[test]
fn xtra_envelope_preserves_symbol_and_exact_payload_boundaries() {
    let mut b = vec![0u8; 32];
    be32(&mut b, 0, 15);
    be32(&mut b, 4, 4);
    be32(&mut b, 8, 16);
    be32(&mut b, 16, 4);
    put(&mut b, 20, b"text");
    be32(&mut b, 24, 4);
    put(&mut b, 28, b"data");
    let xtra = xtra_model(&b).unwrap();
    assert_eq!((xtra.symbol.as_str(), xtra.payload_offset, xtra.payload_length), ("text", 28, 4));
    for i in 0..b.len() {
        assert!(xtra_model(&b[..i]).is_err(), "truncated at {i}");
    }
    for offset in [4, 8, 16, 24] {
        let mut bad = b.clone();
        be32(&mut bad, offset, 0xffffffff);
        assert!(xtra_model(&bad).is_err(), "corrupt at {offset}");
    }
}

#[test]
fn unprotected_cst_files_retain_paths_and_share_the_protected_cast_namespace() {
    let root = Scratch::new("cst");
    root.write("Data.cst", "database");
    assert_eq!(root.sources(&["."]).unwrap(), pairs(&[("DATA.CXT", "Data.cst")]));
    root.write("DATA.CXT", "collision");
    assert!(root.sources(&["."]).unwrap_err().contains("duplicate"));
}

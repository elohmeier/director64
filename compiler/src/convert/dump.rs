//! A Director file as ProjectorRays parsed it (tools/director/dump.mjs): its
//! chunks, their parsed records and the decompiled scripts. ProjectorRays is
//! the one converter stage that is not Rust; everything downstream reads
//! these dumps.
//!
//! Layout: "D64DUMP1", u32le header length, the UTF-8 JSON header
//! {"json": [...], "scripts": {...}, "chunks": [[fourCC, id, length], ...]},
//! then every chunk's bytes in header order.

use serde_json::Value;

pub struct Chunk {
    pub fourcc: String,
    pub id: i64,
    pub data: Vec<u8>,
}

pub struct Dump {
    /// dumpJSON(): [{fourCC, id, data}]
    pub json: Vec<Value>,
    /// dumpScripts(): {isCast, version, casts: [{name, scripts: [...]}]}
    pub scripts: Value,
    /// dumpJSON() after dumpScripts(), when decompiling changed it: the
    /// parser rewrites a protected movie's config while decompiling.
    pub json_after_scripts: Option<Vec<Value>>,
    pub chunks: Vec<Chunk>,
}

impl Dump {
    pub fn parse(bytes: &[u8]) -> Result<Dump, String> {
        if bytes.len() < 12 || &bytes[..8] != b"D64DUMP1" {
            return Err("not a ProjectorRays dump".into());
        }
        let length = u32::from_le_bytes(bytes[8..12].try_into().unwrap()) as usize;
        if length > bytes.len() - 12 {
            return Err("truncated dump header".into());
        }
        let mut header: Value =
            serde_json::from_slice(&bytes[12..12 + length]).map_err(|e| format!("dump header: {e}"))?;
        let mut at = 12 + length;
        let mut chunks = Vec::new();
        for entry in header["chunks"].as_array().ok_or("dump without chunks")? {
            let size = entry[2].as_u64().ok_or("dump chunk length")? as usize;
            if size > bytes.len() - at {
                return Err("truncated dump chunk".into());
            }
            chunks.push(Chunk {
                fourcc: entry[0].as_str().ok_or("dump chunk fourCC")?.to_string(),
                id: entry[1].as_i64().ok_or("dump chunk id")?,
                data: bytes[at..at + size].to_vec(),
            });
            at += size;
        }
        if at != bytes.len() {
            return Err("trailing bytes after dump chunks".into());
        }
        let json = match header["json"].take() {
            Value::Array(items) => items,
            _ => return Err("dump without parsed chunks".into()),
        };
        let json_after_scripts = match header["json_after_scripts"].take() {
            Value::Array(items) => Some(items),
            _ => None,
        };
        Ok(Dump { json, scripts: header["scripts"].take(), json_after_scripts, chunks })
    }
    pub fn parsed(&self, fourcc: &str) -> Option<&Value> {
        self.json.iter().find(|c| c["fourCC"] == fourcc).map(|c| &c["data"])
    }
    pub fn parsed_all<'a>(&'a self, fourcc: &'a str) -> impl Iterator<Item = &'a Value> + 'a {
        self.json.iter().filter(move |c| c["fourCC"] == fourcc)
    }
    pub fn chunk(&self, fourcc: &str, id: i64) -> Option<&Chunk> {
        self.chunks.iter().find(|c| c.fourcc == fourcc && c.id == id)
    }
}

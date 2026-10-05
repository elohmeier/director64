//! ZIP game sources (src/director64/media.py extract_zip): the central
//! directory's entries under the same safety rules, and each file's bytes,
//! stored or deflated, checked against its CRC-32. Reads go through the same
//! bounded `Source` as the ISO reader, so the browser importer reads a ZIP
//! from the page's File as it reads a disc image.

use std::collections::HashSet;

use crate::iso::Source;

pub const MAX_EXPANDED: u64 = 4 * 1024 * 1024 * 1024;
pub const MAX_FILES: usize = 100_000;

#[derive(Debug, Clone, PartialEq)]
pub struct Entry {
    pub path: String,
    /// Offset of the entry's local header.
    pub header: u64,
    pub method: u16,
    pub compressed: u64,
    pub length: u64,
    pub crc: u32,
}

fn read(source: &mut dyn Source, offset: u64, length: usize) -> Result<Vec<u8>, String> {
    if offset.checked_add(length as u64).is_none_or(|end| end > source.size()) {
        return Err("ZIP read outside the archive".into());
    }
    let mut buffer = vec![0u8; length];
    source.read_at(offset, &mut buffer)?;
    Ok(buffer)
}
fn u16_at(b: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([b[at], b[at + 1]])
}
fn u32_at(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(b[at..at + 4].try_into().unwrap())
}

// Code page 437 above ASCII: ZIP names without the UTF-8 flag.
const CP437: &str = "ÇüéâäàåçêëèïîìÄÅÉæÆôöòûùÿÖÜ¢£¥₧ƒáíóúñÑªº¿⌐¬½¼¡«»░▒▓│┤╡╢╖╕╣║╗╝╜╛┐└┴┬├─┼╞╟╚╔╩╦╠═╬╧╨╤╥╙╘╒╓╫╪┘┌█▄▌▐▀αßΓπΣσµτΦΘΩδ∞φε∩≡±≥≤⌠⌡÷≈°∙·√ⁿ²■\u{a0}";

fn decode_name(bytes: &[u8], utf8: bool) -> Result<String, String> {
    if utf8 {
        return String::from_utf8(bytes.to_vec()).map_err(|_| "ZIP name is not UTF-8".into());
    }
    let high: Vec<char> = CP437.chars().collect();
    Ok(bytes.iter().map(|&b| if b < 128 { b as char } else { high[b as usize - 128] }).collect())
}

/// The archive's file entries in central-directory order, directories
/// omitted, after media.py's safety rules.
pub fn inventory(source: &mut dyn Source) -> Result<Vec<Entry>, String> {
    let size = source.size();
    // The end-of-central-directory record, within the last 64 KiB + 22.
    let tail_length = size.min(65_557) as usize;
    let tail = read(source, size - tail_length as u64, tail_length)?;
    let at = (0..tail_length.saturating_sub(21))
        .rev()
        .find(|&i| u32_at(&tail, i) == 0x0605_4b50)
        .ok_or("not a ZIP archive")?;
    let count = u16_at(&tail, at + 10) as usize;
    let directory_size = u32_at(&tail, at + 12) as u64;
    let directory_offset = u32_at(&tail, at + 16) as u64;
    if count == 0xFFFF || directory_offset == 0xFFFF_FFFF {
        return Err("ZIP64 archives are not supported".into());
    }
    if count > MAX_FILES {
        return Err("ZIP exceeds import limits".into());
    }
    let directory = read(source, directory_offset, directory_size as usize)?;
    let (mut p, mut total) = (0usize, 0u64);
    let mut seen = HashSet::new();
    let mut entries = Vec::new();
    for _ in 0..count {
        if p + 46 > directory.len() || u32_at(&directory, p) != 0x0201_4b50 {
            return Err("damaged ZIP central directory".into());
        }
        let flags = u16_at(&directory, p + 8);
        let method = u16_at(&directory, p + 10);
        let crc = u32_at(&directory, p + 16);
        let compressed = u32_at(&directory, p + 20) as u64;
        let length = u32_at(&directory, p + 24) as u64;
        let name_length = u16_at(&directory, p + 28) as usize;
        let extra = u16_at(&directory, p + 30) as usize;
        let comment = u16_at(&directory, p + 32) as usize;
        let external = u32_at(&directory, p + 38);
        let header = u32_at(&directory, p + 42) as u64;
        let raw = directory.get(p + 46..p + 46 + name_length).ok_or("damaged ZIP central directory")?;
        let name = decode_name(raw, flags & 0x800 != 0)?;
        p += 46 + name_length + extra + comment;
        total += length;
        let unsafe_name = name.is_empty()
            || name.starts_with('/')
            || name.split('/').any(|part| part == "." || part == "..")
            || name.contains(['\\', ':', '\0'])
            || name.contains("//")
            || (external >> 16) & 0o170000 == 0o120000
            || flags & 1 != 0;
        if unsafe_name {
            return Err(format!("unsafe/unsupported ZIP entry: {name}"));
        }
        let path = name.trim_end_matches('/').to_string();
        if !seen.insert(path.to_lowercase()) {
            return Err(format!("duplicate ZIP path: {name}"));
        }
        if !name.ends_with('/') {
            entries.push(Entry { path, header, method, compressed, length, crc });
        }
    }
    if total > MAX_EXPANDED {
        return Err("ZIP exceeds import limits".into());
    }
    Ok(entries)
}

fn crc32(data: &[u8]) -> u32 {
    let mut table = [0u32; 256];
    for (i, slot) in table.iter_mut().enumerate() {
        let mut c = i as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
        }
        *slot = c;
    }
    !data.iter().fold(!0u32, |c, &b| table[((c ^ b as u32) & 0xFF) as usize] ^ (c >> 8))
}

/// One entry's bytes, inflated and checked.
pub fn file(source: &mut dyn Source, entry: &Entry) -> Result<Vec<u8>, String> {
    let local = read(source, entry.header, 30)?;
    if u32_at(&local, 0) != 0x0403_4b50 {
        return Err(format!("{}: damaged ZIP local header", entry.path));
    }
    let data = entry.header + 30 + u16_at(&local, 26) as u64 + u16_at(&local, 28) as u64;
    let packed = read(source, data, entry.compressed as usize)?;
    let bytes = match entry.method {
        0 => packed,
        8 => miniz_oxide::inflate::decompress_to_vec_with_limit(&packed, entry.length as usize)
            .map_err(|e| format!("{}: damaged deflate stream ({:?})", entry.path, e.status))?,
        other => return Err(format!("{}: unsupported ZIP compression method {other}", entry.path)),
    };
    if bytes.len() as u64 != entry.length || crc32(&bytes) != entry.crc {
        return Err(format!("{}: ZIP entry fails its CRC", entry.path));
    }
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Bytes(Vec<u8>);
    impl Source for Bytes {
        fn size(&self) -> u64 {
            self.0.len() as u64
        }
        fn read_at(&mut self, offset: u64, buffer: &mut [u8]) -> Result<(), String> {
            let start = offset as usize;
            buffer.copy_from_slice(self.0.get(start..start + buffer.len()).ok_or("eof")?);
            Ok(())
        }
    }

    // A two-entry archive, one stored and one deflated, written by hand.
    fn archive(names: &[&str]) -> Vec<u8> {
        let payloads: Vec<(Vec<u8>, u16, Vec<u8>)> = names
            .iter()
            .enumerate()
            .map(|(i, _)| {
                let data = format!("file {i} contents contents contents").into_bytes();
                if i % 2 == 1 {
                    (miniz_oxide::deflate::compress_to_vec(&data, 6), 8, data)
                } else {
                    (data.clone(), 0, data)
                }
            })
            .collect();
        let (mut out, mut central) = (Vec::new(), Vec::new());
        for (name, (packed, method, data)) in names.iter().zip(&payloads) {
            let header = out.len() as u32;
            let fields = |sig: u32, central: bool| {
                let mut h = sig.to_le_bytes().to_vec();
                if central {
                    h.extend_from_slice(&20u16.to_le_bytes());
                }
                for v in [20u16, 0, *method, 0, 0] {
                    h.extend_from_slice(&v.to_le_bytes());
                }
                for v in [crc32(data), packed.len() as u32, data.len() as u32] {
                    h.extend_from_slice(&v.to_le_bytes());
                }
                h.extend_from_slice(&(name.len() as u16).to_le_bytes());
                h.extend_from_slice(&0u16.to_le_bytes());
                h
            };
            out.extend(fields(0x0403_4b50, false));
            out.extend_from_slice(name.as_bytes());
            out.extend_from_slice(packed);
            central.extend(fields(0x0201_4b50, true));
            for v in [0u16, 0, 0] {
                central.extend_from_slice(&v.to_le_bytes());
            }
            central.extend_from_slice(&0u32.to_le_bytes());
            central.extend_from_slice(&header.to_le_bytes());
            central.extend_from_slice(name.as_bytes());
        }
        let offset = out.len() as u32;
        out.extend_from_slice(&central);
        out.extend_from_slice(&0x0605_4b50u32.to_le_bytes());
        for v in [0u16, 0, names.len() as u16, names.len() as u16] {
            out.extend_from_slice(&v.to_le_bytes());
        }
        out.extend_from_slice(&(central.len() as u32).to_le_bytes());
        out.extend_from_slice(&offset.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes());
        out
    }

    #[test]
    fn stored_and_deflated_entries_read_back() {
        let mut source = Bytes(archive(&["Root/A.DXR", "Root/B.CXT"]));
        let entries = inventory(&mut source).unwrap();
        assert_eq!(entries.iter().map(|e| e.path.as_str()).collect::<Vec<_>>(), ["Root/A.DXR", "Root/B.CXT"]);
        assert_eq!(file(&mut source, &entries[1]).unwrap(), b"file 1 contents contents contents");
        assert_eq!(entries[1].method, 8);
    }

    #[test]
    fn unsafe_and_duplicate_names_are_refused() {
        for names in [&["../evil"][..], &["a\\b"], &["/abs"], &["a/./b"], &["A.DXR", "a.dxr"]] {
            assert!(inventory(&mut Bytes(archive(names))).is_err(), "{names:?}");
        }
    }

    #[test]
    fn a_corrupted_entry_fails_its_crc() {
        let mut bytes = archive(&["A.DXR"]);
        bytes[30 + 5] ^= 1;
        let mut source = Bytes(bytes);
        let entries = inventory(&mut source).unwrap();
        assert!(file(&mut source, &entries[0]).unwrap_err().contains("CRC"));
    }
}

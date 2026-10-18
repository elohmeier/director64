//! Read pinned original font installers as data, without executing them.
//! Outputs are bounded; no archive path is ever written to a host filesystem.
use std::io::{Cursor, Read};

const MAX_ARCHIVE: usize = 16 * 1024 * 1024;
const MAX_FONT: usize = 4 * 1024 * 1024;

pub fn extract(archive: &[u8], entry: &str) -> Result<Vec<u8>, String> {
    if archive.len() > MAX_ARCHIVE
        || entry.is_empty()
        || entry.len() > 64
        || !entry
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b))
        || entry == "."
        || entry == ".."
    {
        return Err("invalid system font archive size or entry name".into());
    }
    let mut output = None;
    for offset in 0..archive.len().saturating_sub(35) {
        let b = &archive[offset..];
        if &b[..4] != b"MSCF" {
            continue;
        }
        let length = u32::from_le_bytes(b[8..12].try_into().unwrap()) as usize;
        if length < 36 || length > b.len() || length > MAX_ARCHIVE {
            continue;
        }
        // Single-file installer cabinets must not request a preceding or
        // following cabinet (CFHEADER flags bits0/1).
        if u16::from_le_bytes(b[30..32].try_into().unwrap()) & 3 != 0 {
            return Err("system font archive spans multiple cabinets".into());
        }
        let Ok(mut cabinet) = cab::Cabinet::new(Cursor::new(&b[..length])) else {
            continue;
        };
        let mut entries = 0usize;
        let mut total = 0u64;
        let mut selected = None;
        for folder in cabinet.folder_entries() {
            for file in folder.file_entries() {
                entries += 1;
                total += u64::from(file.uncompressed_size());
                if entries > 32 || total > MAX_ARCHIVE as u64 {
                    return Err("system font archive exceeds expanded capacity".into());
                }
                if file.name().eq_ignore_ascii_case(entry) {
                    if selected.is_some() {
                        return Err("ambiguous system font archive entry".into());
                    }
                    if file.uncompressed_size() == 0 || file.uncompressed_size() as usize > MAX_FONT
                    {
                        return Err("system font exceeds expanded capacity".into());
                    }
                    selected = Some((file.name().to_string(), file.uncompressed_size()));
                }
            }
        }
        let Some((name, size)) = selected else {
            continue;
        };
        if output.is_some() {
            return Err("ambiguous system font cabinet".into());
        }
        let mut reader = cabinet
            .read_file(&name)
            .map_err(|e| format!("system font cabinet entry: {e}"))?;
        let mut data = Vec::with_capacity(size as usize);
        reader
            .by_ref()
            .take(MAX_FONT as u64 + 1)
            .read_to_end(&mut data)
            .map_err(|e| format!("system font cabinet decoding: {e}"))?;
        if data.len() != size as usize {
            return Err("system font cabinet decoded size disagrees".into());
        }
        output = Some(data);
    }
    output.ok_or_else(|| format!("system font installer has no cabinet entry {entry}"))
}

#[cfg(test)]
mod tests {
    use super::*;
    fn fixture() -> Vec<u8> {
        let mut builder = cab::CabinetBuilder::new();
        builder
            .add_folder(cab::CompressionType::MsZip)
            .add_file("Example.TTF");
        let mut writer = builder.build(Cursor::new(Vec::new())).unwrap();
        use std::io::Write;
        writer
            .next_file()
            .unwrap()
            .unwrap()
            .write_all(b"synthetic original font payload")
            .unwrap();
        assert!(writer.next_file().unwrap().is_none());
        writer.finish().unwrap().into_inner()
    }
    #[test]
    fn standalone_and_self_extracting_cabinets_preserve_exact_entry_bytes() {
        let cabinet = fixture();
        let mut exe = b"MZ synthetic executable prefix, never executed".to_vec();
        exe.extend(&cabinet);
        for bytes in [&cabinet, &exe] {
            assert_eq!(
                extract(bytes, "example.ttf").unwrap(),
                b"synthetic original font payload"
            );
        }
    }
    #[test]
    fn rejects_missing_ambiguous_truncated_or_unsafe_inputs() {
        let cabinet = fixture();
        assert!(extract(&cabinet, "Missing.ttf").is_err());
        for path in [
            "../Example.TTF",
            "a/Example.TTF",
            "a\\Example.TTF",
            "..",
            "",
        ] {
            assert!(extract(&cabinet, path).is_err());
        }
        for length in [0, 4, 35, cabinet.len() - 1] {
            assert!(extract(&cabinet[..length], "Example.TTF").is_err());
        }
        let mut duplicate = cabinet.clone();
        duplicate.extend(&cabinet);
        assert!(
            extract(&duplicate, "Example.TTF")
                .unwrap_err()
                .contains("ambiguous")
        );
    }
}

//! ISO 9660 inventory over bounded reads: the Rust port of
//! src/director64/iso.py. The image is never mounted, never read whole and
//! never written; a source only answers offset/length reads, so the browser
//! can serve them from a `File` and the native CLI from a file handle.
//!
//! An image is either cooked (2048-byte sectors, `.iso`) or a raw rip of
//! the disc (2352-byte sectors with sync, header and error correction: a
//! CloneCD `.img` or a `.bin` of a `.cue`). Offsets are always logical,
//! counted in cooked sectors; `Layout` maps them onto the image. A raw
//! image's descriptors and directories must sit in valid data sectors; file
//! contents are copied from each sector's user-data position as they are,
//! since copy-protected discs carry deliberately damaged sectors in decoy
//! files (every catalogued image is pinned by its SHA-256 regardless).

pub const SECTOR: u64 = 2048;
pub const RAW_SECTOR: u64 = 2352;
const SYNC: [u8; 12] = [0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0];

/// Random access to an image. `read_at` fills the whole buffer or fails.
pub trait Source {
    fn size(&self) -> u64;
    fn read_at(&mut self, offset: u64, buffer: &mut [u8]) -> Result<(), String>;
}

#[derive(Debug, Clone, PartialEq)]
pub struct Entry {
    pub path: String,
    pub offset: u64,
    pub length: u64,
}

/// Where each logical sector's 2048 bytes of user data sit in the image:
/// sector `n` at `n * stride + data`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Layout {
    pub stride: u64,
    pub data: u64,
}

pub const COOKED: Layout = Layout { stride: SECTOR, data: 0 };

impl Layout {
    /// The user-data offset of a raw sector after checking its sync
    /// pattern and mode; Mode 2 Form 2 sectors carry no ISO 9660 data.
    fn raw_data(sector: &[u8]) -> Option<u64> {
        if sector[..12] != SYNC {
            return None;
        }
        match sector[15] {
            1 => Some(16),
            2 if sector[18] & 0x20 == 0 => Some(24),
            _ => None,
        }
    }

    /// The image's layout, from its first volume descriptor (sector 16).
    pub fn detect(source: &mut dyn Source) -> Result<Layout, String> {
        let size = source.size();
        let mut probe = |offset: u64, length: u64| -> Option<Vec<u8>> {
            let mut buffer = vec![0u8; length as usize];
            (offset + length <= size && source.read_at(offset, &mut buffer).is_ok()).then_some(buffer)
        };
        if probe(16 * SECTOR, 7).is_some_and(|d| &d[1..7] == b"CD001\x01") {
            return Ok(COOKED);
        }
        if size % RAW_SECTOR == 0 {
            if let Some(sector) = probe(16 * RAW_SECTOR, RAW_SECTOR) {
                if let Some(data) = Self::raw_data(&sector) {
                    let at = data as usize;
                    if &sector[at + 1..at + 7] == b"CD001\x01" {
                        return Ok(Layout { stride: RAW_SECTOR, data });
                    }
                }
            }
        }
        Err("not an ISO 9660 image (cooked or raw 2352-byte sectors)".into())
    }
}

/// A raw image seen as the cooked image it carries: logical offsets in,
/// user data out. A strict view also checks every touched sector's framing.
pub struct Cooked<'a> {
    inner: &'a mut dyn Source,
    pub layout: Layout,
    strict: bool,
}

impl<'a> Cooked<'a> {
    pub fn new(inner: &'a mut dyn Source) -> Result<Self, String> {
        let layout = Layout::detect(inner)?;
        Ok(Cooked { inner, layout, strict: true })
    }
}

impl Source for Cooked<'_> {
    fn size(&self) -> u64 {
        self.inner.size() / self.layout.stride * SECTOR
    }
    fn read_at(&mut self, offset: u64, buffer: &mut [u8]) -> Result<(), String> {
        if self.layout == COOKED {
            return self.inner.read_at(offset, buffer);
        }
        let end = offset + buffer.len() as u64;
        if end > self.size() {
            return Err("read outside image".into());
        }
        let mut done = 0usize;
        // Whole raw sectors in bounded runs, so large files cost few reads.
        let mut sector = offset / SECTOR;
        while done < buffer.len() {
            let last = ((end - 1) / SECTOR).min(sector + 511);
            let mut raw = vec![0u8; ((last - sector + 1) * RAW_SECTOR) as usize];
            self.inner.read_at(sector * RAW_SECTOR, &mut raw)?;
            for (i, frame) in raw.chunks_exact(RAW_SECTOR as usize).enumerate() {
                if self.strict && Layout::raw_data(frame) != Some(self.layout.data) {
                    return Err(format!("raw sector {} is not ISO 9660 data", sector + i as u64));
                }
                let start = (sector + i as u64) * SECTOR;
                let from = offset.max(start) - start;
                let to = end.min(start + SECTOR) - start;
                let data = &frame[(self.layout.data + from) as usize..(self.layout.data + to) as usize];
                buffer[done..done + data.len()].copy_from_slice(data);
                done += data.len();
            }
            sector = last + 1;
        }
        Ok(())
    }
}

fn read(source: &mut dyn Source, offset: u64, length: u64) -> Result<Vec<u8>, String> {
    let size = source.size();
    if offset > size || length > size - offset {
        return Err("ISO extent outside image".into());
    }
    let mut data = vec![0u8; length as usize];
    source.read_at(offset, &mut data).map_err(|_| "short ISO read".to_string())?;
    Ok(data)
}

fn dual(data: &[u8], offset: usize, width: usize) -> Result<u64, String> {
    if offset + width * 2 > data.len() {
        return Err("truncated ISO integer".into());
    }
    let mut little = 0u64;
    for i in (0..width).rev() {
        little = little << 8 | data[offset + i] as u64;
    }
    let mut big = 0u64;
    for i in 0..width {
        big = big << 8 | data[offset + width + i] as u64;
    }
    if little != big {
        return Err("inconsistent ISO endian copies".into());
    }
    Ok(little)
}

/// Every file of a bounded, single-extent ISO 9660 image (no HFS or Rock
/// Ridge), sorted by path. Associated streams are listed under
/// `__associated__/`. Offsets are logical; read them with `file`.
pub fn inventory(source: &mut dyn Source) -> Result<Vec<Entry>, String> {
    inventory_cooked(&mut Cooked::new(source)?)
}

fn inventory_cooked(source: &mut dyn Source) -> Result<Vec<Entry>, String> {
    let size = source.size();
    let mut primary: Option<Vec<u8>> = None;
    let mut terminated = false;
    for sector in 16..(size / SECTOR).min(256) {
        let descriptor = read(source, sector * SECTOR, SECTOR)?;
        if &descriptor[1..7] != b"CD001\x01" {
            return Err("invalid ISO volume descriptor".into());
        }
        if descriptor[0] == 1 {
            if primary.is_some() {
                return Err("duplicate primary volume descriptor".into());
            }
            primary = Some(descriptor.clone());
        }
        if descriptor[0] == 255 {
            terminated = true;
            break;
        }
    }
    if !terminated {
        return Err("missing volume descriptor terminator".into());
    }
    let primary = match primary {
        Some(p) if dual(&p, 128, 2)? == SECTOR => p,
        _ => return Err("unsupported logical block size or missing primary volume".into()),
    };
    // The declared size may exceed the image (copy protection overstates
    // it); every read is still bounded by the image itself.
    let volume_size = dual(&primary, 80, 4)? * SECTOR;
    let extent = |record: &[u8]| -> Result<(u64, u64), String> {
        let offset = dual(record, 2, 4)? * SECTOR;
        let length = dual(record, 10, 4)?;
        if offset > volume_size || length > volume_size - offset {
            return Err("extent outside declared ISO volume".into());
        }
        if record[1] != 0 || record[26] != 0 || record[27] != 0 || record[25] & 0x80 != 0 {
            return Err("extended attributes, interleaving or multi-extent ISO unsupported".into());
        }
        if dual(record, 28, 2)? != 1 {
            return Err("multi-volume ISO unsupported".into());
        }
        Ok((offset, length))
    };
    let root_len = primary[156] as usize;
    if root_len < 34 || primary[181] & 2 == 0 {
        return Err("invalid ISO root".into());
    }
    let (root_offset, root_length) = extent(&primary[156..156 + root_len])?;
    let mut pending = vec![(String::new(), root_offset, root_length, 0u32)];
    let mut visited = std::collections::HashSet::new();
    let mut paths = std::collections::HashSet::new();
    let mut files = Vec::new();
    while let Some((parent, offset, length, depth)) = pending.pop() {
        if depth > 16 || visited.contains(&(offset, length)) || length > 16 * 1024 * 1024 {
            return Err("cyclic or excessive ISO directory".into());
        }
        visited.insert((offset, length));
        let data = read(source, offset, length)?;
        let mut position = 0usize;
        while position < data.len() {
            let count = data[position] as usize;
            if count == 0 {
                position = (position / SECTOR as usize + 1) * SECTOR as usize;
                continue;
            }
            if count < 34 || position + count > data.len() {
                return Err("truncated ISO directory record".into());
            }
            let record = &data[position..position + count];
            if position % SECTOR as usize + count > SECTOR as usize {
                return Err("ISO record crosses a sector".into());
            }
            position += count;
            let name_len = record[32] as usize;
            if name_len == 0 || 33 + name_len > count {
                return Err("invalid ISO identifier length".into());
            }
            let name_bytes = &record[33..33 + name_len];
            if name_bytes == b"\x00" || name_bytes == b"\x01" {
                continue;
            }
            if !name_bytes.is_ascii() {
                return Err("non-ASCII ISO identifier".into());
            }
            let text = std::str::from_utf8(name_bytes).unwrap();
            let name = text.split(';').next().unwrap().trim_end_matches('.');
            if name.is_empty() || name == "." || name == ".." || name.contains(['/', '\\', ':', '\0']) {
                return Err("unsafe ISO identifier".into());
            }
            let mut path = if parent.is_empty() { name.to_string() } else { format!("{parent}/{name}") };
            if record[25] & 4 != 0 {
                path = format!("__associated__/{path}");
            }
            // str.casefold() for the ASCII identifiers ISO 9660 allows.
            if !paths.insert(path.to_ascii_lowercase()) {
                return Err(format!("duplicate ISO path: {path}"));
            }
            let (child_offset, child_length) = extent(record)?;
            if record[25] & 2 != 0 {
                pending.push((path, child_offset, child_length, depth + 1));
            } else {
                files.push(Entry { path, offset: child_offset, length: child_length });
            }
        }
    }
    files.sort_by(|a, b| a.path.cmp(&b.path));
    Ok(files)
}

/// Reads one inventoried file, its sectors' user data as they are. A file
/// running past the image's end (a protection decoy on a disc whose volume
/// is overstated) reads as zeros there, as a drive's unreadable sectors.
pub fn file(source: &mut dyn Source, entry: &Entry) -> Result<Vec<u8>, String> {
    let mut cooked = Cooked::new(source)?;
    cooked.strict = false;
    let present = entry.length.min(cooked.size().saturating_sub(entry.offset));
    let mut data = read(&mut cooked, entry.offset, present)?;
    data.resize(entry.length as usize, 0);
    Ok(data)
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Memory(Vec<u8>);
    impl Source for Memory {
        fn size(&self) -> u64 {
            self.0.len() as u64
        }
        fn read_at(&mut self, offset: u64, buffer: &mut [u8]) -> Result<(), String> {
            let start = offset as usize;
            buffer.copy_from_slice(self.0.get(start..start + buffer.len()).ok_or("eof")?);
            Ok(())
        }
    }

    fn put_dual(image: &mut [u8], at: usize, value: u64, width: usize) {
        for i in 0..width {
            image[at + i] = (value >> (8 * i)) as u8;
            image[at + width + i] = (value >> (8 * (width - 1 - i))) as u8;
        }
    }
    fn record(name: &[u8], sector: u64, length: u64, flags: u8) -> Vec<u8> {
        let mut r = vec![0u8; 33 + name.len() + (1 - name.len() % 2)];
        r[0] = r.len() as u8;
        put_dual(&mut r, 2, sector, 4);
        put_dual(&mut r, 10, length, 4);
        r[25] = flags;
        put_dual(&mut r, 28, 1, 2);
        r[32] = name.len() as u8;
        r[33..33 + name.len()].copy_from_slice(name);
        r
    }
    /// A 24-sector image: root at 20 with DIR (21) and A.TXT;1 (22).
    fn image() -> Vec<u8> {
        let mut image = vec![0u8; 24 * 2048];
        let pvd = 16 * 2048;
        image[pvd] = 1;
        image[pvd + 1..pvd + 7].copy_from_slice(b"CD001\x01");
        put_dual(&mut image, pvd + 80, 24, 4);
        put_dual(&mut image, pvd + 128, 2048, 2);
        let root = record(b"\x00", 20, 2048, 2);
        image[pvd + 156..pvd + 156 + root.len()].copy_from_slice(&root);
        let term = 17 * 2048;
        image[term] = 255;
        image[term + 1..term + 7].copy_from_slice(b"CD001\x01");
        let mut at = 20 * 2048;
        for r in [record(b"\x00", 20, 2048, 2), record(b"\x01", 20, 2048, 2), record(b"DIR", 21, 2048, 2), record(b"A.TXT;1", 22, 5, 0)] {
            image[at..at + r.len()].copy_from_slice(&r);
            at += r.len();
        }
        let r = record(b"B.BIN;1", 23, 3, 4);
        image[21 * 2048..21 * 2048 + r.len()].copy_from_slice(&r);
        image[22 * 2048..22 * 2048 + 5].copy_from_slice(b"hello");
        image[23 * 2048..23 * 2048 + 3].copy_from_slice(b"abc");
        image
    }

    #[test]
    fn inventories_files_and_associated_streams() {
        let mut source = Memory(image());
        let entries = inventory(&mut source).unwrap();
        assert_eq!(
            entries,
            vec![
                Entry { path: "A.TXT".into(), offset: 22 * 2048, length: 5 },
                Entry { path: "__associated__/DIR/B.BIN".into(), offset: 23 * 2048, length: 3 },
            ]
        );
        assert_eq!(file(&mut source, &entries[0]).unwrap(), b"hello");
    }

    /// The cooked image framed as a raw Mode 1 (or Mode 2 Form 1) rip with
    /// trailing audio, as a CloneCD `.img` or a `.bin` stores it.
    fn raw(cooked: &[u8], mode: u8) -> Vec<u8> {
        let mut raw = Vec::new();
        for (n, sector) in cooked.chunks(2048).enumerate() {
            let mut frame = vec![0u8; 2352];
            frame[..12].copy_from_slice(&SYNC);
            frame[12] = n as u8;
            frame[15] = mode;
            let data = if mode == 1 { 16 } else { 24 };
            frame[data..data + 2048].copy_from_slice(sector);
            raw.extend(frame);
        }
        raw.extend(vec![0x55u8; 3 * 2352]);
        raw
    }

    #[test]
    fn reads_raw_sector_images() {
        for (mode, data) in [(1, 16), (2, 24)] {
            let mut source = Memory(raw(&image(), mode));
            assert_eq!(Layout::detect(&mut source).unwrap(), Layout { stride: 2352, data });
            let cooked = inventory(&mut Memory(image())).unwrap();
            let entries = inventory(&mut source).unwrap();
            assert_eq!(entries, cooked);
            assert_eq!(file(&mut source, &entries[0]).unwrap(), b"hello");
            assert_eq!(file(&mut source, &entries[1]).unwrap(), b"abc");
        }
        assert_eq!(Layout::detect(&mut Memory(image())).unwrap(), COOKED);
    }

    #[test]
    fn raw_images_fail_closed() {
        // A damaged sector in a file (a protection decoy) reads as it is...
        let mut damaged = raw(&image(), 1);
        damaged[22 * 2352 + 15] = 0x62;
        let mut source = Memory(damaged);
        let entries = inventory(&mut source).unwrap();
        assert_eq!(file(&mut source, &entries[0]).unwrap(), b"hello");
        // ...but not in a directory.
        let mut form2 = raw(&image(), 2);
        form2[21 * 2352 + 18] |= 0x20;
        assert!(inventory(&mut Memory(form2)).is_err());
        let mut unsynced = raw(&image(), 1);
        unsynced[16 * 2352 + 3] = 0;
        assert!(inventory(&mut Memory(unsynced)).is_err());
        let mut short = raw(&image(), 1);
        short.pop();
        assert!(inventory(&mut Memory(short)).is_err());
    }

    #[test]
    fn an_overstated_volume_reads_past_the_image_as_zeros() {
        let mut overstated = image();
        put_dual(&mut overstated, 16 * 2048 + 80, 40, 4);
        // A.TXT runs 2048 bytes past the image's last sector.
        let r = record(b"A.TXT;1", 23, 4096, 0);
        overstated[20 * 2048 + 104..20 * 2048 + 104 + r.len()].copy_from_slice(&r);
        let mut source = Memory(overstated);
        let entries = inventory(&mut source).unwrap();
        let data = file(&mut source, &entries[0]).unwrap();
        assert_eq!((&data[..3], data.len(), data[2048..].iter().all(|&b| b == 0)), (&b"abc"[..], 4096, true));
    }

    #[test]
    fn damaged_images_fail_closed() {
        let mut broken = image();
        broken[16 * 2048 + 80] ^= 1; // endian copies disagree
        assert!(inventory(&mut Memory(broken)).is_err());
        let mut truncated = image();
        truncated.truncate(21 * 2048);
        assert!(inventory(&mut Memory(truncated)).is_err());
        let mut unsafe_name = image();
        let at = 20 * 2048 + 34 + 34 + 33;
        unsafe_name[at] = b'/';
        assert!(inventory(&mut Memory(unsafe_name)).is_err());
    }
}

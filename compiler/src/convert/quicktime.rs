//! QuickTime movies (Löwenzahn's linked digital video) and external AIFF
//! sounds, read without ffmpeg so the browser importer converts them the way
//! the host pipeline does. The demuxer reads the atoms a 1990s QuickTime 2
//! movie has (moov/mvhd, trak/tkhd/mdia/mdhd/hdlr/minf/stbl: stsd, stts,
//! stsc, stsz, stco/co64, stss) and lists every sample. Sound tracks decode
//! to 16-bit PCM WAV with ffmpeg's own conversion rules (`-acodec pcm_s16le`
//! at the source rate and channel count), so both pipelines produce the same
//! sample data: `raw ` unsigned 8-bit, `twos` signed 8/16-bit big-endian,
//! `sowt` signed 16-bit little-endian and `ima4` QuickTime IMA ADPCM.

type R<T> = Result<T, String>;

fn be16(b: &[u8], at: usize) -> R<u16> {
    b.get(at..at + 2).map(|s| u16::from_be_bytes([s[0], s[1]])).ok_or_else(|| "truncated QuickTime atom".into())
}
fn be32(b: &[u8], at: usize) -> R<u32> {
    b.get(at..at + 4).map(|s| u32::from_be_bytes(s.try_into().unwrap())).ok_or_else(|| "truncated QuickTime atom".into())
}
fn be64(b: &[u8], at: usize) -> R<u64> {
    b.get(at..at + 8).map(|s| u64::from_be_bytes(s.try_into().unwrap())).ok_or_else(|| "truncated QuickTime atom".into())
}

/// One media sample: where it is in the file, when it plays in the track's
/// time scale, and whether it is a sync (key) sample.
#[derive(Debug, Clone, PartialEq)]
pub struct Sample {
    pub offset: u64,
    pub size: u32,
    pub time: u64,
    pub duration: u32,
    pub keyframe: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TrackKind {
    Video,
    Audio,
    Other,
}

#[derive(Debug, Clone)]
pub struct Track {
    pub kind: TrackKind,
    /// The sample description's four-character code ("cvid", "twos", ...).
    pub codec: String,
    pub timescale: u32,
    /// Media duration in the track's time scale.
    pub duration: u64,
    pub width: u16,
    pub height: u16,
    pub depth: u16,
    /// Sound: sample rate (whole Hz), channels, bits per sample.
    pub rate: u32,
    pub channels: u16,
    pub bits: u16,
    /// Sound description version 1 fields (0 for version 0).
    pub samples_per_packet: u32,
    pub bytes_per_frame: u32,
    pub samples: Vec<Sample>,
    /// Every chunk's file offset and sample count, in file order: how
    /// uncompressed sound is laid out, where stsz counts one per frame.
    pub chunks: Vec<(u64, u32)>,
    /// Where the edit list's presentation ends, in the track's time scale
    /// (media time plus the segment's duration); None without an edit list.
    /// ffmpeg drops every packet that starts at or after it.
    pub edit_end: Option<u64>,
}

#[derive(Debug, Clone)]
pub struct Movie {
    pub timescale: u32,
    pub duration: u64,
    /// The movie header's duration over its time scale.
    pub duration_seconds: f64,
    pub tracks: Vec<Track>,
}

impl Movie {
    pub fn video(&self) -> Option<&Track> {
        self.tracks.iter().find(|t| t.kind == TrackKind::Video)
    }
    pub fn audio(&self) -> Option<&Track> {
        self.tracks.iter().find(|t| t.kind == TrackKind::Audio)
    }
}

/// Child atoms of `b[start..end]`: (kind, body start, body end).
fn atoms(b: &[u8], start: usize, end: usize) -> R<Vec<([u8; 4], usize, usize)>> {
    let mut out = Vec::new();
    let mut at = start;
    while at + 8 <= end {
        let mut size = be32(b, at)? as u64;
        let kind: [u8; 4] = b[at + 4..at + 8].try_into().unwrap();
        let mut header = 8;
        if size == 1 {
            size = be64(b, at + 8)?;
            header = 16;
        } else if size == 0 {
            size = (end - at) as u64;
        }
        if size < header as u64 || at as u64 + size > end as u64 {
            return Err(format!("QuickTime atom {} overruns its parent", String::from_utf8_lossy(&kind)));
        }
        out.push((kind, at + header, at + size as usize));
        at += size as usize;
    }
    Ok(out)
}
fn child(list: &[([u8; 4], usize, usize)], kind: &[u8; 4]) -> Option<(usize, usize)> {
    list.iter().find(|a| &a.0 == kind).map(|a| (a.1, a.2))
}

/// av_rescale: a * b / c, rounded to nearest with halves away from zero.
fn rescale(a: u64, b: u64, c: u64) -> u64 {
    ((a as u128 * b as u128 + c as u128 / 2) / c as u128) as u64
}

pub fn parse(b: &[u8]) -> R<Movie> {
    let top = atoms(b, 0, b.len())?;
    let (ms, me) = child(&top, b"moov").ok_or("QuickTime movie without moov")?;
    let moov = atoms(b, ms, me)?;
    let (hs, _) = child(&moov, b"mvhd").ok_or("QuickTime movie without mvhd")?;
    let (timescale, duration) = if b[hs] == 1 { (be32(b, hs + 20)?, be64(b, hs + 24)?) } else { (be32(b, hs + 12)?, be32(b, hs + 16)? as u64) };
    if timescale == 0 {
        return Err("QuickTime movie time scale 0".into());
    }
    let mut tracks = Vec::new();
    for &(kind, s, e) in &moov {
        if &kind == b"trak" {
            tracks.push(track(b, s, e, timescale)?);
        }
    }
    Ok(Movie { timescale, duration, duration_seconds: duration as f64 / timescale as f64, tracks })
}

fn track(b: &[u8], s: usize, e: usize, movie_timescale: u32) -> R<Track> {
    let trak = atoms(b, s, e)?;
    let (ds, de) = child(&trak, b"mdia").ok_or("track without mdia")?;
    let mdia = atoms(b, ds, de)?;
    let (hs, _) = child(&mdia, b"mdhd").ok_or("track without mdhd")?;
    let (timescale, duration) = if b[hs] == 1 { (be32(b, hs + 20)?, be64(b, hs + 24)?) } else { (be32(b, hs + 12)?, be32(b, hs + 16)? as u64) };
    let (hs, _) = child(&mdia, b"hdlr").ok_or("track without hdlr")?;
    let kind = match b.get(hs + 8..hs + 12) {
        Some(b"vide") => TrackKind::Video,
        Some(b"soun") => TrackKind::Audio,
        _ => TrackKind::Other,
    };
    let (is, ie) = child(&mdia, b"minf").ok_or("track without minf")?;
    let minf = atoms(b, is, ie)?;
    let (ts, te) = child(&minf, b"stbl").ok_or("track without stbl")?;
    let stbl = atoms(b, ts, te)?;
    let table = |k: &[u8; 4]| child(&stbl, k).ok_or_else(|| format!("sample table without {}", String::from_utf8_lossy(k)));

    let mut t = Track {
        kind,
        codec: String::new(),
        timescale,
        duration,
        width: 0,
        height: 0,
        depth: 0,
        rate: 0,
        channels: 0,
        bits: 0,
        samples_per_packet: 0,
        bytes_per_frame: 0,
        samples: Vec::new(),
        chunks: Vec::new(),
        edit_end: None,
    };
    // The disc's edit lists are one segment from media time 0; a list with
    // more segments or an empty edit is refused rather than misread.
    if let Some((es, ee)) = child(&trak, b"edts")
        && let Some((ls, _)) = child(&atoms(b, es, ee)?, b"elst")
    {
        let count = be32(b, ls + 4)?;
        if count != 1 || b[ls] != 0 {
            return Err(format!("unsupported QuickTime edit list ({count} segments)"));
        }
        let duration = be32(b, ls + 8)? as u64;
        let media = be32(b, ls + 12)? as i32;
        if media < 0 {
            return Err("unsupported empty QuickTime edit".into());
        }
        t.edit_end = Some(media as u64 + rescale(duration, timescale as u64, movie_timescale as u64));
    }
    // The first sample description; every disc movie has exactly one.
    let (ss, _) = table(b"stsd")?;
    if be32(b, ss + 4)? == 0 {
        return Err("sample table without a description".into());
    }
    let d = ss + 8; // description: size, format, 6 reserved, data ref index
    t.codec = String::from_utf8_lossy(b.get(d + 4..d + 8).ok_or("truncated stsd")?).into_owned();
    let body = d + 16;
    match kind {
        TrackKind::Video => {
            t.width = be16(b, body + 16)?;
            t.height = be16(b, body + 18)?;
            t.depth = be16(b, body + 66)?;
        }
        TrackKind::Audio => {
            let version = be16(b, body)?;
            t.channels = be16(b, body + 8)?;
            t.bits = be16(b, body + 10)?;
            t.rate = be32(b, body + 16)? >> 16;
            if version == 1 {
                t.samples_per_packet = be32(b, body + 20)?;
                t.bytes_per_frame = be32(b, body + 28)?;
            }
        }
        TrackKind::Other => {}
    }

    let (cs, _) = table(b"stsc")?;
    let stsc: Vec<(u32, u32)> = (0..be32(b, cs + 4)? as usize)
        .map(|i| Ok((be32(b, cs + 8 + i * 12)?, be32(b, cs + 12 + i * 12)?)))
        .collect::<R<_>>()?;
    let offsets: Vec<u64> = if let Ok((os, _)) = table(b"stco") {
        (0..be32(b, os + 4)? as usize).map(|i| Ok(be32(b, os + 8 + i * 4)? as u64)).collect::<R<_>>()?
    } else {
        let (os, _) = table(b"co64")?;
        (0..be32(b, os + 4)? as usize).map(|i| be64(b, os + 8 + i * 8)).collect::<R<_>>()?
    };
    for (i, &offset) in offsets.iter().enumerate() {
        let chunk = i as u32 + 1;
        let per = stsc.iter().rev().find(|e| e.0 <= chunk).map_or(0, |e| e.1);
        t.chunks.push((offset, per));
    }
    let (zs, _) = table(b"stsz")?;
    let fixed = be32(b, zs + 4)?;
    let count = be32(b, zs + 8)? as usize;
    let sizes: Vec<u32> = if fixed != 0 { vec![fixed; count] } else { (0..count).map(|i| be32(b, zs + 12 + i * 4)).collect::<R<_>>()? };
    let (xs, _) = table(b"stts")?;
    let mut durations = Vec::with_capacity(count);
    for i in 0..be32(b, xs + 4)? as usize {
        let n = be32(b, xs + 8 + i * 8)?;
        let delta = be32(b, xs + 12 + i * 8)?;
        durations.extend(std::iter::repeat_n(delta, n as usize));
    }
    let sync: Option<std::collections::HashSet<u32>> = match table(b"stss") {
        Ok((ys, _)) => Some((0..be32(b, ys + 4)? as usize).map(|i| be32(b, ys + 8 + i * 4)).collect::<R<_>>()?),
        Err(_) => None,
    };
    let (mut index, mut time) = (0usize, 0u64);
    for &(offset, per) in &t.chunks {
        let mut at = offset;
        for _ in 0..per {
            if index >= count {
                break;
            }
            let duration = durations.get(index).copied().unwrap_or(0);
            t.samples.push(Sample {
                offset: at,
                size: sizes[index],
                time,
                duration,
                keyframe: sync.as_ref().is_none_or(|s| s.contains(&(index as u32 + 1))),
            });
            at += sizes[index] as u64;
            time += duration as u64;
            index += 1;
        }
    }
    Ok(t)
}

/// A canonical 44-byte PCM WAV header over s16le data.
fn wav(rate: u32, channels: u16, data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(44 + data.len());
    out.extend_from_slice(b"RIFF");
    out.extend_from_slice(&(36 + data.len() as u32).to_le_bytes());
    out.extend_from_slice(b"WAVEfmt ");
    out.extend_from_slice(&16u32.to_le_bytes());
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&channels.to_le_bytes());
    out.extend_from_slice(&rate.to_le_bytes());
    out.extend_from_slice(&(rate * channels as u32 * 2).to_le_bytes());
    out.extend_from_slice(&(channels * 2).to_le_bytes());
    out.extend_from_slice(&16u16.to_le_bytes());
    out.extend_from_slice(b"data");
    out.extend_from_slice(&(data.len() as u32).to_le_bytes());
    out.extend_from_slice(data);
    out
}

const IMA_INDEX: [i32; 16] = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8];
const IMA_STEP: [i32; 89] = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
];

/// QuickTime IMA ADPCM, as ffmpeg's adpcm_ima_qt: per channel, 34-byte
/// packets of a 2-byte header (9-bit predictor, 7-bit step index) and 64
/// nibbles, low nibble first. A header close to the running state keeps it.
fn ima4(data: &[u8], channels: usize) -> R<Vec<i16>> {
    let packet = 34 * channels;
    let mut state = vec![(0i32, 0i32); channels];
    let mut out = Vec::with_capacity(data.len() / packet * 64 * channels);
    for block in data.chunks_exact(packet) {
        let mut planes = vec![[0i16; 64]; channels];
        for (c, plane) in planes.iter_mut().enumerate() {
            let p = &block[c * 34..c * 34 + 34];
            let header = i16::from_be_bytes([p[0], p[1]]) as i32;
            let (predictor, step_index) = (header & !0x7f, header & 0x7f);
            let (sp, si) = &mut state[c];
            if *si != step_index || (predictor - *sp).abs() > 0x7f {
                *si = step_index;
                *sp = predictor;
            }
            if *si > 88 {
                return Err(format!("ima4 step index {si}"));
            }
            for m in 0..32 {
                for (k, nibble) in [p[2 + m] & 15, p[2 + m] >> 4].into_iter().enumerate() {
                    let nibble = nibble as i32;
                    let step = IMA_STEP[*si as usize];
                    let mut diff = step >> 3;
                    if nibble & 4 != 0 {
                        diff += step;
                    }
                    if nibble & 2 != 0 {
                        diff += step >> 1;
                    }
                    if nibble & 1 != 0 {
                        diff += step >> 2;
                    }
                    let value = if nibble & 8 != 0 { *sp - diff } else { *sp + diff };
                    *sp = value.clamp(-32768, 32767);
                    *si = (*si + IMA_INDEX[nibble as usize]).clamp(0, 88);
                    plane[m * 2 + k] = *sp as i16;
                }
            }
        }
        for i in 0..64 {
            for plane in &planes {
                out.push(plane[i]);
            }
        }
    }
    Ok(out)
}

/// The movie's first sound track as 16-bit PCM WAV at its own rate and
/// channel count; None for a movie without sound.
pub fn audio_wav(bytes: &[u8]) -> R<Option<Vec<u8>>> {
    let movie = parse(bytes)?;
    let Some(t) = movie.audio() else {
        return Ok(None);
    };
    let channels = t.channels.max(1) as usize;
    let codec = t.codec.as_str();
    // Bytes one chunk's samples occupy: stsz counts frames for these codecs.
    let chunk_bytes = |frames: u32| -> u64 {
        if codec == "ima4" {
            frames as u64 / 64 * 34 * channels as u64
        } else if t.samples_per_packet != 0 && t.bytes_per_frame != 0 {
            frames as u64 / t.samples_per_packet as u64 * t.bytes_per_frame as u64
        } else {
            frames as u64 * channels as u64 * (t.bits as u64).div_ceil(8)
        }
    };
    // ffmpeg's packets: each chunk split into runs of at most 1024 frames
    // (whole ima4 packets of 64); a packet starting at or after the edit's
    // end is dropped, one straddling it plays whole.
    let per_packet: u32 = if codec == "ima4" { 64 } else if t.samples_per_packet > 1 { t.samples_per_packet } else { 1 };
    let run = (1024 / per_packet).max(1) * per_packet;
    let mut raw = Vec::new();
    let mut time = 0u64;
    'chunks: for &(offset, frames) in &t.chunks {
        let mut at = offset as usize;
        let mut left = frames;
        while left > 0 {
            let samples = run.min(left);
            if t.edit_end.is_some_and(|end| time >= end) {
                break 'chunks;
            }
            let length = chunk_bytes(samples) as usize;
            raw.extend_from_slice(bytes.get(at..at + length).ok_or("sound chunk outside the movie")?);
            at += length;
            time += samples as u64;
            left -= samples;
        }
    }
    let samples: Vec<i16> = match (codec, t.bits) {
        ("raw ", 8) => raw.iter().map(|&v| ((v as i16) - 128) << 8).collect(),
        ("twos", 8) => raw.iter().map(|&v| (v as i8 as i16) << 8).collect(),
        ("twos", 16) => raw.chunks_exact(2).map(|p| i16::from_be_bytes([p[0], p[1]])).collect(),
        ("sowt", 16) => raw.chunks_exact(2).map(|p| i16::from_le_bytes([p[0], p[1]])).collect(),
        ("ima4", _) => ima4(&raw, channels)?,
        (other, bits) => return Err(format!("unsupported QuickTime sound {other:?} at {bits} bits")),
    };
    let data: Vec<u8> = samples.iter().flat_map(|s| s.to_le_bytes()).collect();
    Ok(Some(wav(t.rate, channels as u16, &data)))
}

/// An 80-bit IEEE extended sample rate (AIFF COMM), rounded as ffmpeg does.
fn extended(b: &[u8]) -> R<u32> {
    let exponent = (u16::from_be_bytes([b[0], b[1]]) & 0x7fff) as i32 - 16383;
    let mantissa = u64::from_be_bytes(b[2..10].try_into().unwrap());
    if !(0..=31).contains(&exponent) {
        return Err("AIFF sample rate out of range".into());
    }
    Ok((mantissa >> (63 - exponent)) as u32)
}

/// An external AIFF (not AIFC) as 16-bit PCM WAV at its own rate and
/// channel count: every sample frame the SSND chunk holds.
pub fn aiff_wav(bytes: &[u8]) -> R<Vec<u8>> {
    if bytes.get(..4) != Some(b"FORM") || bytes.get(8..12) != Some(b"AIFF") {
        return Err("not an uncompressed AIFF".into());
    }
    let (mut format, mut samples) = (None, None);
    for (kind, s, e) in atoms_le(bytes)? {
        match &kind {
            b"COMM" if e - s >= 18 => format = Some((be16(bytes, s)?, be16(bytes, s + 6)?, extended(&bytes[s + 8..s + 18])?)),
            b"SSND" if e - s >= 8 => samples = Some(&bytes[s + 8 + be32(bytes, s)? as usize..e]),
            _ => {}
        }
    }
    let (channels, bits, rate) = format.ok_or("AIFF without COMM")?;
    let samples = samples.ok_or("AIFF without SSND")?;
    let pcm: Vec<i16> = match bits {
        8 => samples.iter().map(|&v| (v as i8 as i16) << 8).collect(),
        16 => samples.chunks_exact(2).map(|p| i16::from_be_bytes([p[0], p[1]])).collect(),
        _ => return Err(format!("unsupported AIFF sample size {bits}")),
    };
    let width = channels.max(1) as usize;
    let data: Vec<u8> = pcm[..pcm.len() / width * width].iter().flat_map(|s| s.to_le_bytes()).collect();
    Ok(wav(rate, channels, &data))
}

/// IFF chunks of an AIFF form: (kind, body start, body end), padded to even.
fn atoms_le(b: &[u8]) -> R<Vec<([u8; 4], usize, usize)>> {
    let mut out = Vec::new();
    let mut at = 12;
    while at + 8 <= b.len() {
        let kind: [u8; 4] = b[at..at + 4].try_into().unwrap();
        let size = be32(b, at + 4)? as usize;
        let end = (at + 8 + size).min(b.len());
        out.push((kind, at + 8, end));
        at = at + 8 + size + (size & 1);
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn atom(kind: &[u8; 4], body: &[u8]) -> Vec<u8> {
        let mut out = ((body.len() + 8) as u32).to_be_bytes().to_vec();
        out.extend_from_slice(kind);
        out.extend_from_slice(body);
        out
    }
    fn full(kind: &[u8; 4], body: &[u8]) -> Vec<u8> {
        let mut b = vec![0u8; 4];
        b.extend_from_slice(body);
        atom(kind, &b)
    }
    fn words(values: &[u32]) -> Vec<u8> {
        values.iter().flat_map(|v| v.to_be_bytes()).collect()
    }

    /// A movie with one `raw ` sound track of `data` in two chunks.
    fn sound_movie(data: &[u8]) -> Vec<u8> {
        let mdat = atom(b"mdat", data);
        let mut desc = vec![0, 0, 0, 0, 0, 0, 0, 1]; // reserved, data ref
        desc.extend_from_slice(&[0, 0, 0, 0, 0, 0, 0, 0]); // version, revision, vendor
        desc.extend_from_slice(&1u16.to_be_bytes());
        desc.extend_from_slice(&8u16.to_be_bytes());
        desc.extend_from_slice(&[0, 0, 0, 0]);
        desc.extend_from_slice(&(22050u32 << 16).to_be_bytes());
        let mut entry = ((desc.len() + 8) as u32).to_be_bytes().to_vec();
        entry.extend_from_slice(b"raw ");
        entry.extend_from_slice(&desc);
        let mut stsd = words(&[1]);
        stsd.extend(entry);
        let half = data.len() as u32 / 2;
        let stbl = [
            full(b"stsd", &stsd),
            full(b"stts", &words(&[1, data.len() as u32, 1])),
            full(b"stsc", &words(&[1, 1, half, 1])),
            full(b"stsz", &words(&[1, data.len() as u32])),
            full(b"stco", &words(&[2, 8, 8 + half])),
        ]
        .concat();
        let minf = atom(b"minf", &atom(b"stbl", &stbl));
        let mut hdlr = words(&[0]);
        hdlr.extend_from_slice(b"soun");
        let mdia = atom(b"mdia", &[full(b"mdhd", &words(&[0, 0, 22050, data.len() as u32, 0])), full(b"hdlr", &hdlr), minf].concat());
        let moov = atom(b"moov", &[full(b"mvhd", &words(&[0, 0, 600, 1200])), atom(b"trak", &mdia)].concat());
        [mdat, moov].concat()
    }

    #[test]
    fn a_raw_sound_track_becomes_s16_wav() {
        let movie = sound_movie(&[0x80, 0xff, 0x00, 0x81]);
        let parsed = parse(&movie).unwrap();
        assert_eq!(parsed.duration_seconds, 2.0);
        let t = parsed.audio().unwrap();
        assert_eq!((t.codec.as_str(), t.rate, t.channels, t.bits), ("raw ", 22050, 1, 8));
        assert_eq!(t.chunks, vec![(8, 2), (10, 2)]);
        let wav = audio_wav(&movie).unwrap().unwrap();
        let samples: Vec<i16> = wav[44..].chunks(2).map(|p| i16::from_le_bytes([p[0], p[1]])).collect();
        assert_eq!(samples, vec![0, 127 << 8, -128 << 8, 1 << 8]);
    }

    #[test]
    fn ima4_decodes_a_silent_packet_to_its_predictor() {
        let mut packet = vec![0x01, 0x00]; // predictor 0x0100, step index 0
        packet.extend_from_slice(&[0x88; 32]); // nibble 8: minus step>>3 = 0
        let out = ima4(&packet, 1).unwrap();
        assert_eq!(out.len(), 64);
        assert!(out.iter().all(|&s| s == 0x100));
    }

    #[test]
    fn aiff_eight_bit_samples_shift_into_sixteen() {
        let mut comm = 1u16.to_be_bytes().to_vec();
        comm.extend_from_slice(&3u32.to_be_bytes());
        comm.extend_from_slice(&8u16.to_be_bytes());
        comm.extend_from_slice(&[0x40, 0x0d, 0xac, 0x44, 0, 0, 0, 0, 0, 0]);
        let mut ssnd = vec![0u8; 8];
        ssnd.extend_from_slice(&[0x00, 0x7f, 0x80]);
        // IFF chunks: kind, then the body's size, padded to even.
        let chunk = |kind: &[u8; 4], data: &[u8]| {
            let mut out = kind.to_vec();
            out.extend_from_slice(&(data.len() as u32).to_be_bytes());
            out.extend_from_slice(data);
            if data.len() % 2 == 1 {
                out.push(0);
            }
            out
        };
        let mut body = b"AIFF".to_vec();
        body.extend(chunk(b"COMM", &comm));
        body.extend(chunk(b"SSND", &ssnd));
        let aiff = [b"FORM".to_vec(), (body.len() as u32).to_be_bytes().to_vec(), body].concat();
        let wav = aiff_wav(&aiff).unwrap();
        assert_eq!(u32::from_le_bytes(wav[24..28].try_into().unwrap()), 22050);
        assert_eq!(&wav[44..], &[0, 0, 0, 0x7f, 0, 0x80]);
    }
}

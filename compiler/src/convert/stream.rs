//! External sound files as the browser player streams them (`sound playFile`).
//! The console converts them to wav64 with audioconv64; a browser decodes WAV
//! and MP3 itself, so those ship as they are and only AIFF, which Chrome does
//! not decode, is rewritten as the same PCM in a WAV container. The container
//! is chosen by magic bytes, never by extension (the Lernerfolg disc names
//! MP3 and WAV files `.aif`). The duration is what the runtime needs to hold
//! the channel busy for as long as the sound plays.

type R<T> = Result<T, String>;

fn be16(b: &[u8], at: usize) -> R<u16> {
    b.get(at..at + 2).map(|s| u16::from_be_bytes([s[0], s[1]])).ok_or_else(|| "truncated sound".into())
}
fn be32(b: &[u8], at: usize) -> R<u32> {
    b.get(at..at + 4).map(|s| u32::from_be_bytes(s.try_into().unwrap())).ok_or_else(|| "truncated sound".into())
}
fn le32(b: &[u8], at: usize) -> R<u32> {
    b.get(at..at + 4).map(|s| u32::from_le_bytes(s.try_into().unwrap())).ok_or_else(|| "truncated sound".into())
}

/// An 80-bit IEEE extended sample rate (AIFF COMM), as a whole number of Hz.
fn extended_rate(b: &[u8]) -> R<u32> {
    let exponent = (u16::from_be_bytes([b[0], b[1]]) & 0x7fff) as i32 - 16383;
    let mantissa = u64::from_be_bytes(b[2..10].try_into().unwrap());
    if !(0..=31).contains(&exponent) {
        return Err("AIFF sample rate out of range".into());
    }
    Ok((mantissa >> (63 - exponent)) as u32)
}

/// A stream the browser can decode, and its length in milliseconds.
pub fn browser_stream(bytes: &[u8]) -> R<(Vec<u8>, u32)> {
    match bytes.get(..4) {
        Some(b"FORM") => aiff_to_wav(bytes),
        Some(b"RIFF") => Ok((bytes.to_vec(), wav_milliseconds(bytes)?)),
        Some(head) if head[..3] == *b"ID3" || (head[0] == 0xff && head[1] & 0xe0 == 0xe0) => {
            Ok((bytes.to_vec(), mp3_milliseconds(bytes)?))
        }
        _ => Err("unrecognized audio container".into()),
    }
}

fn aiff_to_wav(bytes: &[u8]) -> R<(Vec<u8>, u32)> {
    if bytes.get(8..12) != Some(b"AIFF") {
        return Err("not an uncompressed AIFF".into());
    }
    let (mut format, mut samples) = (None, None);
    let mut at = 12;
    while at + 8 <= bytes.len() {
        let id = &bytes[at..at + 4];
        let size = be32(bytes, at + 4)? as usize;
        let body = bytes.get(at + 8..at + 8 + size).ok_or("truncated AIFF chunk")?;
        match id {
            b"COMM" if size >= 18 => {
                format = Some((be16(body, 0)?, be32(body, 2)?, be16(body, 6)?, extended_rate(&body[8..18])?))
            }
            b"SSND" if size >= 8 => samples = Some(&body[8 + be32(body, 0)? as usize..]),
            _ => {}
        }
        at += 8 + size + (size & 1);
    }
    let (channels, frames, bits, rate) = format.ok_or("AIFF without COMM")?;
    let samples = samples.ok_or("AIFF without SSND")?;
    if channels == 0 || rate == 0 || !(bits == 8 || bits == 16) {
        return Err(format!("unsupported AIFF format: {channels} channels, {bits} bits, {rate} Hz"));
    }
    let width = bits as usize / 8 * channels as usize;
    let length = (frames as usize * width).min(samples.len() / width * width);
    let mut out = Vec::with_capacity(44 + length);
    out.extend_from_slice(b"RIFF");
    out.extend_from_slice(&(36 + length as u32).to_le_bytes());
    out.extend_from_slice(b"WAVEfmt ");
    out.extend_from_slice(&16u32.to_le_bytes());
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&channels.to_le_bytes());
    out.extend_from_slice(&rate.to_le_bytes());
    out.extend_from_slice(&(rate * width as u32).to_le_bytes());
    out.extend_from_slice(&(width as u16).to_le_bytes());
    out.extend_from_slice(&bits.to_le_bytes());
    out.extend_from_slice(b"data");
    out.extend_from_slice(&(length as u32).to_le_bytes());
    if bits == 16 {
        // Big-endian samples become little-endian.
        for pair in samples[..length].chunks_exact(2) {
            out.extend_from_slice(&[pair[1], pair[0]]);
        }
    } else {
        // Signed 8-bit samples become WAV's unsigned ones.
        out.extend(samples[..length].iter().map(|&s| s ^ 0x80));
    }
    let milliseconds = (length / width) as u64 * 1000 / rate as u64;
    Ok((out, milliseconds as u32))
}

fn wav_milliseconds(bytes: &[u8]) -> R<u32> {
    let (mut rate_bytes, mut data) = (None, None);
    let mut at = 12;
    while at + 8 <= bytes.len() {
        let size = le32(bytes, at + 4)? as usize;
        match &bytes[at..at + 4] {
            b"fmt " => rate_bytes = Some(le32(bytes, at + 16)?),
            b"data" => data = Some(size.min(bytes.len() - at - 8)),
            _ => {}
        }
        at += 8 + size + (size & 1);
    }
    let (per_second, data) = (rate_bytes.ok_or("WAV without fmt")?, data.ok_or("WAV without data")?);
    if per_second == 0 {
        return Err("WAV with no byte rate".into());
    }
    Ok((data as u64 * 1000 / per_second as u64) as u32)
}

/// MPEG audio layer III: the frames' samples over the stream's rate, walking
/// frame headers from the first sync after any ID3v2 tag to the first bytes
/// that are not a frame (trailing tags).
fn mp3_milliseconds(bytes: &[u8]) -> R<u32> {
    let mut at = 0;
    if bytes.starts_with(b"ID3") && bytes.len() >= 10 {
        let size = bytes[6..10].iter().fold(0usize, |n, &b| n << 7 | (b & 0x7f) as usize);
        at = 10 + size + if bytes[5] & 0x10 != 0 { 10 } else { 0 };
    }
    const RATES: [[u32; 3]; 4] = [[11025, 12000, 8000], [0, 0, 0], [22050, 24000, 16000], [44100, 48000, 32000]];
    const V1: [u32; 16] = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0];
    const V2: [u32; 16] = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0];
    let (mut samples, mut rate) = (0u64, 0u32);
    while at + 4 <= bytes.len() {
        let h = be32(bytes, at)?;
        let version = (h >> 19) & 3; // 0: 2.5, 2: 2, 3: 1
        let layer = (h >> 17) & 3; // 1: layer III
        let bitrate = (h >> 12) & 15;
        let rate_index = (h >> 10) & 3;
        if h >> 21 != 0x7ff || version == 1 || layer != 1 || bitrate == 0 || bitrate == 15 || rate_index == 3 {
            break;
        }
        let frame_rate = RATES[version as usize][rate_index as usize];
        let kbps = if version == 3 { V1 } else { V2 }[bitrate as usize];
        let per_frame = if version == 3 { 1152 } else { 576 };
        let padding = (h >> 9) & 1;
        let length = (per_frame / 8 * kbps * 1000 / frame_rate + padding) as usize;
        if rate != 0 && frame_rate != rate {
            break;
        }
        rate = frame_rate;
        samples += per_frame as u64;
        at += length;
    }
    if rate == 0 {
        return Err("MP3 without a layer III frame".into());
    }
    Ok((samples * 1000 / rate as u64) as u32)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn aiff(bits: u16, samples: &[u8]) -> Vec<u8> {
        let frames = (samples.len() / (bits as usize / 8)) as u32;
        let mut comm = Vec::new();
        comm.extend_from_slice(&1u16.to_be_bytes());
        comm.extend_from_slice(&frames.to_be_bytes());
        comm.extend_from_slice(&bits.to_be_bytes());
        // 22050 Hz as an 80-bit extended: exponent 16383 + 14.
        comm.extend_from_slice(&[0x40, 0x0d, 0xac, 0x44, 0, 0, 0, 0, 0, 0]);
        let mut ssnd = vec![0u8; 8];
        ssnd.extend_from_slice(samples);
        let mut body = b"AIFF".to_vec();
        for (id, chunk) in [(b"COMM", comm), (b"SSND", ssnd)] {
            body.extend_from_slice(id);
            body.extend_from_slice(&(chunk.len() as u32).to_be_bytes());
            body.extend_from_slice(&chunk);
        }
        let mut out = b"FORM".to_vec();
        out.extend_from_slice(&(body.len() as u32).to_be_bytes());
        out.extend(body);
        out
    }

    #[test]
    fn aiff_becomes_the_same_pcm_in_wav() {
        let (wav, ms) = browser_stream(&aiff(16, &[0x12, 0x34, 0xff, 0xfe])).unwrap();
        assert_eq!(&wav[..4], b"RIFF");
        assert_eq!(le32(&wav, 24).unwrap(), 22050);
        assert_eq!(&wav[44..], &[0x34, 0x12, 0xfe, 0xff]);
        assert_eq!(ms, 0);
        let (wav, _) = browser_stream(&aiff(8, &[0x00, 0x80, 0x7f])).unwrap();
        assert_eq!(&wav[44..], &[0x80, 0x00, 0xff]);
        let (_, ms) = browser_stream(&aiff(16, &vec![0; 22050 * 2])).unwrap();
        assert_eq!(ms, 1000);
        let (wav, ms) = browser_stream(&aiff(16, &vec![0; 4410 * 2])).unwrap();
        assert_eq!((wav_milliseconds(&wav).unwrap(), ms), (200, 200));
    }

    #[test]
    fn mp3_duration_counts_its_frames() {
        // MPEG-2 layer III, 32 kbps, 22050 Hz: 576 samples in 104 bytes.
        let mut frame = vec![0xff, 0xf3, 0x40, 0xc4];
        frame.resize(104, 0);
        let stream: Vec<u8> = frame.iter().copied().cycle().take(104 * 100).collect();
        let (bytes, ms) = browser_stream(&stream).unwrap();
        assert_eq!(bytes.len(), stream.len());
        assert_eq!(ms, (100 * 576 * 1000 / 22050) as u32);
    }

    #[test]
    fn unknown_containers_are_refused() {
        assert!(browser_stream(b"OggS....").is_err());
    }
}

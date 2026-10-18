//! MPEG-1 video metadata without a platform decoder. Packet identifiers and
//! PES header variants corroborated against clean ScummVM41ac2b31847622d0662d22c03fe6979e3b43cfbc,
//! video/mpegps_decoder.cpp. This parser is an independent implementation.
use serde_json::{Value, json};

struct Stream {
    video: Vec<u8>,
    audio: Vec<u8>,
    audio_start: Option<f64>,
    video_start: Option<f64>,
}
fn timestamp(packet: &[u8], at: usize) -> Result<f64, String> {
    let b = packet.get(at..at + 5).ok_or("truncated MPEG PTS")?;
    if b[0] & 1 == 0 || b[2] & 1 == 0 || b[4] & 1 == 0 {
        return Err("invalid MPEG PTS markers".into());
    }
    let pts = ((b[0] as u64 & 14) << 29)
        | ((b[1] as u64) << 22)
        | ((b[2] as u64 & 254) << 14)
        | ((b[3] as u64) << 7)
        | ((b[4] as u64) >> 1);
    Ok(pts as f64 / 90000.0)
}
fn elementary(bytes: &[u8]) -> Result<Stream, String> {
    if bytes.starts_with(&[0, 0, 1, 0xb3]) {
        return Ok(Stream {
            video: bytes.to_vec(),
            audio: Vec::new(),
            audio_start: None,
            video_start: None,
        });
    }
    let mut video = Vec::new();
    let mut audio = Vec::new();
    let mut audio_stream = None;
    let mut audio_start = None;
    let mut video_start = None;
    let mut stream = None;
    let mut at = 0;
    while at < bytes.len() {
        // Authored program streams also pad between packs with zero bytes.
        while bytes.get(at..at + 3) == Some(&[0, 0, 0]) {
            at += 1;
        }
        if bytes[at..].iter().all(|&v| v == 0) {
            break;
        }
        if bytes.get(at..at + 3) != Some(&[0, 0, 1]) {
            return Err(format!("MPEG packet sync at {at}"));
        }
        let code = *bytes.get(at + 3).ok_or("truncated MPEG packet code")?;
        if code == 0xb9 {
            break;
        }
        if code == 0xba {
            let header = *bytes.get(at + 4).ok_or("truncated MPEG pack")?;
            let size = if header & 0xf0 == 0x20 {
                12
            } else if header & 0xc0 == 0x40 {
                14 + (*bytes.get(at + 13).ok_or("truncated MPEG2 pack")? & 7) as usize
            } else {
                return Err("invalid MPEG pack header".into());
            };
            if at + size > bytes.len() {
                return Err("truncated MPEG pack".into());
            }
            at += size;
            continue;
        }
        let length = bytes
            .get(at + 4..at + 6)
            .ok_or("truncated MPEG packet length")?;
        let end = at + 6 + u16::from_be_bytes(length.try_into().unwrap()) as usize;
        let packet = bytes.get(at + 6..end).ok_or("truncated MPEG packet")?;
        if (0xc0..=0xef).contains(&code) {
            let is_video = code >= 0xe0;
            if is_video && stream.is_some_and(|s| s != code) {
                return Err("multiple MPEG video streams".into());
            }
            if is_video {
                stream = Some(code);
            }
            let mut p = 0;
            while packet.get(p) == Some(&255) {
                p += 1;
            }
            let first = *packet.get(p).ok_or("empty MPEG video packet")?;
            let mut pts = None;
            if first & 0xc0 == 0x80 {
                if packet.get(p + 1).is_some_and(|v| v & 128 != 0) {
                    pts = Some(timestamp(packet, p + 3)?);
                }
                p += 3 + *packet.get(p + 2).ok_or("truncated MPEG2 PES")? as usize;
            } else {
                if first & 0xc0 == 0x40 {
                    p += 2;
                }
                let marker = *packet.get(p).ok_or("truncated MPEG PES")?;
                if matches!(marker >> 4, 2 | 3) {
                    pts = Some(timestamp(packet, p)?);
                }
                p += match marker >> 4 {
                    2 => 5,
                    3 => 10,
                    0 if marker == 15 => 1,
                    _ => return Err("unsupported MPEG PES header".into()),
                };
            }
            if is_video {
                if video_start.is_none() {
                    video_start = pts;
                }
                video.extend_from_slice(packet.get(p..).ok_or("truncated MPEG timestamp")?);
            } else {
                if audio_stream.is_some_and(|s| s != code) {
                    return Err("multiple MPEG audio streams".into());
                }
                audio_stream = Some(code);
                if audio_start.is_none() {
                    audio_start = pts;
                }
                audio.extend_from_slice(packet.get(p..).ok_or("truncated MPEG timestamp")?);
            }
        }
        at = end;
    }
    Ok(Stream {
        video,
        audio,
        audio_start,
        video_start,
    })
}

/// A little-endian video byte count, followed by elementary video and audio.
/// The browser uses this same bounded demuxer for MPEG-1 and MPEG-2 PS packs;
/// its decoder only needs MPEG-1 pictures and MP2 samples.
pub fn demux(bytes: &[u8]) -> Result<Vec<u8>, String> {
    let stream = elementary(bytes)?;
    let length = u32::try_from(stream.video.len()).map_err(|_| "MPEG video exceeds 4 GiB")?;
    let mut result = Vec::with_capacity(4 + stream.video.len() + stream.audio.len());
    result.extend_from_slice(&length.to_le_bytes());
    result.extend_from_slice(&stream.video);
    result.extend_from_slice(&stream.audio);
    Ok(result)
}

pub fn info(bytes: &[u8]) -> Result<Value, String> {
    let stream = elementary(bytes)?;
    let video = &stream.video;
    let mut references = 0;
    let mut skipped_initial_frames = 0;
    let mut sequence = None;
    let mut frames = 0u32;
    let mut at = 0;
    while at + 4 <= video.len() {
        if video[at..at + 3] != [0, 0, 1] {
            at += 1;
            continue;
        }
        match video[at + 3] {
            0xb3 => {
                let s = video
                    .get(at + 4..at + 8)
                    .ok_or("truncated MPEG sequence header")?;
                let width = ((s[0] as u16) << 4) | (s[1] as u16 >> 4);
                let height = ((s[1] as u16 & 15) << 8) | s[2] as u16;
                let (rate, scale) = match s[3] & 15 {
                    1 => (24000, 1001),
                    2 => (24, 1),
                    3 => (25, 1),
                    4 => (30000, 1001),
                    5 => (30, 1),
                    6 => (50, 1),
                    7 => (60000, 1001),
                    8 => (60, 1),
                    _ => return Err("invalid MPEG frame rate".into()),
                };
                let current = (width, height, rate, scale);
                if width == 0 || height == 0 || sequence.is_some_and(|s| s != current) {
                    return Err("unsupported changing MPEG geometry/frame rate".into());
                }
                sequence = Some(current);
            }
            0 => {
                if sequence.is_none() || at + 6 > video.len() {
                    return Err("MPEG picture before sequence or truncated".into());
                }
                let kind = (video[at + 5] >> 3) & 7;
                if kind == 3 && references < 2 {
                    skipped_initial_frames += 1;
                }
                if kind == 1 || kind == 2 {
                    references += 1;
                }
                frames = frames.checked_add(1).ok_or("too many MPEG pictures")?;
            }
            0xb5 => return Err("MPEG2 sequence extensions require different frame timing".into()),
            _ => (),
        }
        at += 4;
    }
    let (width, height, rate, scale) = sequence.ok_or("MPEG has no video sequence")?;
    if frames == 0 {
        return Err("MPEG has no video pictures".into());
    }
    let (audio_frames, audio_rate, audio_tail) = audio_info(&stream.audio)?;
    Ok(
        json!({"width":width,"height":height,"frames":frames,"rate":rate,"scale":scale,
        "decoded_frames":frames-skipped_initial_frames,"skipped_initial_frames":skipped_initial_frames,
        "video_start_seconds":stream.video_start,"audio_start_seconds":stream.audio_start,
        "audio_lead_seconds":stream.video_start.zip(stream.audio_start).map(|(v,a)| v-a),
        "audio_frames":audio_frames,"audio_rate":audio_rate,"audio_trailing_bytes":audio_tail,
        "duration_seconds":(frames-skipped_initial_frames) as f64 * scale as f64 / rate as f64}),
    )
}

fn audio_info(bytes: &[u8]) -> Result<(u32, u32, usize), String> {
    let mut at = 0;
    let mut frames = 0;
    let mut rate = 0;
    while at + 4 <= bytes.len() {
        let h = &bytes[at..at + 4];
        if h[0] != 255 || h[1] & 0xfe != 0xfc {
            return Err(format!("MP2 frame sync at {at}"));
        }
        let bitrate = [
            0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0,
        ][(h[2] >> 4) as usize];
        let sample_rate = [44100, 48000, 32000, 0][((h[2] >> 2) & 3) as usize];
        if bitrate == 0 || sample_rate == 0 || (rate != 0 && rate != sample_rate) {
            return Err("unsupported MP2 bitrate or changing sample rate".into());
        }
        rate = sample_rate;
        let length = (144000 * bitrate / rate + ((h[2] >> 1) & 1) as u32) as usize;
        if at + length > bytes.len() {
            break;
        }
        at += length;
        frames += 1;
    }
    if !bytes.is_empty() && frames == 0 {
        return Err("MP2 has no complete audio frames".into());
    }
    Ok((frames, rate, bytes.len() - at))
}

//! Strict MP3 cast-media decoding shared by native and Wasm conversion.
use std::io::Cursor;

use symphonia::core::{
    audio::SampleBuffer, codecs::DecoderOptions, errors::Error, formats::FormatOptions,
    io::MediaSourceStream, meta::MetadataOptions, probe::Hint,
};

use super::d5media::Sound;

/// Validate complete frame boundaries before the demuxer can resynchronize.
/// Stripped Shockwave ediM resources may retain zero-filled allocation tails.
fn payload(bytes: &[u8], shockwave_cast: bool) -> Result<&[u8], String> {
    if bytes.len() > 64 * 1024 * 1024 {
        return Err("MP3 source exceeds conversion budget".into());
    }
    let mut at = 0;
    if bytes.starts_with(b"ID3") {
        if bytes.len() < 10
            || !(2..=4).contains(&bytes[3])
            || bytes[4] == 255
            || bytes[6..10].iter().any(|b| b & 128 != 0)
        {
            return Err("invalid MP3 ID3 header".into());
        }
        let length = bytes[6..10]
            .iter()
            .fold(0usize, |n, b| (n << 7) | *b as usize);
        at = 10
            + length
            + if bytes[3] == 4 && bytes[5] & 16 != 0 {
                10
            } else {
                0
            };
        if at > bytes.len() {
            return Err("truncated MP3 ID3 tag".into());
        }
    }
    // Some Shockwave cast sounds retain one blank allocation frame before
    // the MP3 packets. It is entirely zero (optionally a single leading FF),
    // and its size must equal the following complete packet's declared size.
    // This is a cast-resource envelope rule, never general MP3 resync.
    if shockwave_cast
        && bytes.get(at).is_some_and(|b| [0, 255].contains(b))
        && bytes.get(at + 1) == Some(&0)
    {
        let lead = 1 + bytes[at + 1..]
            .iter()
            .take(2048)
            .take_while(|&&b| b == 0)
            .count();
        if let Ok((size, _, _)) = frame_header(bytes, at + lead) {
            if lead == size && at + lead + size <= bytes.len() {
                at += lead;
            }
        }
    }
    let start = at;
    let id3_tail = bytes
        .len()
        .checked_sub(128)
        .filter(|at| bytes[*at..].starts_with(b"TAG"));
    let (mut frames, mut rate, mut channels) = (0, 0, 0);
    while at < bytes.len() {
        let tail = &bytes[at..];
        // Some encoders place the tag in unused ancillary bytes of the
        // final frame. Keep that complete frame for codec validation; only
        // the remaining tag bytes lie outside its declared frame boundary.
        if frames > 0 && (tail.iter().all(|b| *b == 0) || id3_tail.is_some_and(|tag| at >= tag)) {
            break;
        }
        let (size, hz, channel_count) = frame_header(bytes, at)?;
        if rate != 0 && rate != hz {
            return Err("MP3 sample rate changes within cast media".into());
        }
        rate = hz;
        if channels != 0 && channels != channel_count {
            return Err("MP3 channel count changes within cast media".into());
        }
        channels = channel_count;
        if size > tail.len() {
            return Err(format!("truncated MP3 frame at {at}"));
        }
        at += size;
        frames += 1;
    }
    if frames == 0 {
        return Err("MP3 cast has no audio frames".into());
    }
    Ok(&bytes[start..at])
}

fn frame_header(bytes: &[u8], at: usize) -> Result<(usize, u32, u32), String> {
    let h = u32::from_be_bytes(
        bytes
            .get(at..at + 4)
            .ok_or("truncated MP3 frame header")?
            .try_into()
            .unwrap(),
    );
    let (version, bitrate, frequency) = ((h >> 19) & 3, (h >> 12) & 15, (h >> 10) & 3);
    if h >> 21 != 0x7ff
        || version == 1
        || (h >> 17) & 3 != 1
        || bitrate == 0
        || bitrate == 15
        || frequency == 3
    {
        return Err(format!("invalid MP3 frame header at {at}"));
    }
    let hz = [44100, 48000, 32000][frequency as usize]
        >> match version {
            3 => 0,
            2 => 1,
            _ => 2,
        };
    let channels = if (h >> 6) & 3 == 3 { 1 } else { 2 };
    let kbps = if version == 3 {
        [
            0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0,
        ]
    } else {
        [
            0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0,
        ]
    }[bitrate as usize];
    let size = ((if version == 3 { 144 } else { 72 }) * kbps * 1000 / hz + ((h >> 9) & 1)) as usize;
    Ok((size, hz, channels))
}

pub fn decode(bytes: &[u8]) -> Result<Sound, String> {
    decode_source(payload(bytes, false)?)
}

pub fn decode_shockwave_cast(bytes: &[u8]) -> Result<Sound, String> {
    decode_source(payload(bytes, true)?)
}

fn decode_source(source: &[u8]) -> Result<Sound, String> {
    let stream = MediaSourceStream::new(Box::new(Cursor::new(source.to_vec())), Default::default());
    let mut hint = Hint::new();
    hint.with_extension("mp3");
    let options = FormatOptions {
        enable_gapless: true,
        ..Default::default()
    };
    let mut input = symphonia::default::get_probe()
        .format(&hint, stream, &options, &MetadataOptions::default())
        .map_err(|e| format!("MP3 format: {e}"))?
        .format;
    let track = input.default_track().ok_or("MP3 cast has no audio track")?;
    let track_id = track.id;
    let mut codec = symphonia::default::get_codecs()
        .make(&track.codec_params, &DecoderOptions { verify: true })
        .map_err(|e| format!("MP3 decoder: {e}"))?;
    let mut samples = Vec::new();
    let (mut rate, mut channels) = (0, 0);
    loop {
        let packet = match input.next_packet() {
            Ok(packet) => packet,
            Err(Error::IoError(e)) if e.kind() == std::io::ErrorKind::UnexpectedEof => break,
            Err(e) => return Err(format!("MP3 packet: {e}")),
        };
        if packet.track_id() != track_id {
            return Err("MP3 cast contains unexpected tracks".into());
        }
        let decoded = codec
            .decode(&packet)
            .map_err(|e| format!("MP3 decode: {e}"))?;
        let spec = *decoded.spec();
        let count = spec.channels.count() as u32;
        if ![1, 2].contains(&count) || (rate != 0 && (rate != spec.rate || channels != count)) {
            return Err("MP3 audio format changes within cast media".into());
        }
        rate = spec.rate;
        channels = count;
        let mut pcm = SampleBuffer::<i16>::new(decoded.capacity() as u64, spec);
        pcm.copy_interleaved_ref(decoded);
        if samples.len() + pcm.len() * 2 > 64 * 1024 * 1024 {
            return Err("MP3 decoded audio exceeds conversion budget".into());
        }
        // The existing sound conversion path accepts big-endian PCM.
        samples.extend(pcm.samples().iter().flat_map(|v| v.to_be_bytes()));
    }
    if samples.is_empty() || rate == 0 || channels == 0 {
        return Err("MP3 cast decoded no audio".into());
    }
    let frames = (samples.len() / (channels as usize * 2)) as u32;
    Ok(Sound {
        rate,
        frames,
        channels,
        bits: 16,
        loop_start: 0,
        loop_end: frames,
        samples,
    })
}

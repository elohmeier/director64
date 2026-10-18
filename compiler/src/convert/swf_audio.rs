//! SWF stream and event audio, independently implemented from Adobe SWF19
//! pp.178–190. MP3 blocks contain whole codec frames; SeekSamples maps each
//! SWF frame onto the concatenated PCM after the initial encoder latency.
use super::{R, byte, i16le, u16le, u32le};
use crate::convert::{d5media::Sound, mp3};

pub struct Audio {
    pub sound: Sound,
    pub positions: Vec<u32>,
}
pub struct Stream {
    rate: u32,
    channels: u32,
    compression: u8,
    bits: u32,
    latency: i64,
    data: Vec<u8>,
    positions: Vec<u32>,
    samples: u32,
    previous: Option<usize>,
}
fn format(flags: u8) -> (u8, u32, u32, u32) {
    (
        flags >> 4,
        [5512, 11025, 22050, 44100][((flags >> 2) & 3) as usize],
        1 + (flags & 1) as u32,
        if flags & 2 != 0 { 16 } else { 8 },
    )
}
// Validate each independently stored block before concatenation. Decode uses
// the full stream so bit-reservoir dependencies cross block boundaries.
fn mp3_frames(data: &[u8], rate: u32, channels: u32) -> R<u32> {
    let (mut p, mut count) = (0, 0u32);
    while p < data.len() {
        let h = u32::from_be_bytes(
            data.get(p..p + 4)
                .ok_or("truncated SWF MP3 frame")?
                .try_into()
                .unwrap(),
        );
        let (version, bitrate, freq) = ((h >> 19) & 3, (h >> 12) & 15, (h >> 10) & 3);
        if h >> 21 != 0x7ff
            || version == 1
            || (h >> 17) & 3 != 1
            || bitrate == 0
            || bitrate == 15
            || freq == 3
        {
            return Err("invalid SWF MP3 frame header".into());
        }
        let hz = [44100, 48000, 32000][freq as usize]
            >> match version {
                3 => 0,
                2 => 1,
                _ => 2,
            };
        let ch = if (h >> 6) & 3 == 3 { 1 } else { 2 };
        if hz != rate || ch != channels {
            return Err("SWF MP3 format disagrees with sound header".into());
        }
        let kbps = if version == 3 {
            [
                0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0,
            ]
        } else {
            [
                0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0,
            ]
        }[bitrate as usize];
        p += ((if version == 3 { 144 } else { 72 }) * kbps * 1000 / hz + ((h >> 9) & 1)) as usize;
        if p > data.len() {
            return Err("truncated SWF MP3 block".into());
        }
        count = count
            .checked_add(if version == 3 { 1152 } else { 576 })
            .ok_or("SWF audio sample overflow")?;
    }
    Ok(count)
}
fn decoded(
    data: &[u8],
    compression: u8,
    rate: u32,
    channels: u32,
    bits: u32,
    frames: u32,
) -> R<Sound> {
    let sound = match compression {
        2 => mp3::decode(data)?,
        3 => {
            let bytes = frames as usize * channels as usize * (bits / 8) as usize;
            if data.len() != bytes {
                return Err("SWF PCM sample count disagrees".into());
            }
            let mut samples = data.to_vec();
            if bits == 16 {
                for pair in samples.chunks_exact_mut(2) {
                    pair.swap(0, 1);
                }
            }
            Sound {
                rate,
                frames,
                channels,
                bits,
                loop_start: 0,
                loop_end: frames,
                samples,
            }
        }
        _ => return Err(format!("unsupported SWF audio compression {compression}")),
    };
    if sound.rate != rate || sound.channels != channels || sound.frames != frames {
        return Err("SWF decoded audio format or sample count disagrees".into());
    }
    Ok(sound)
}
fn trim(mut sound: Sound, latency: i64, count: Option<u32>) -> R<Sound> {
    let stride = (sound.channels * sound.bits / 8) as usize;
    if latency >= 0 {
        if latency as u64 > sound.frames as u64 {
            return Err("SWF sound latency exceeds samples".into());
        }
        sound.samples.drain(..latency as usize * stride);
    } else {
        let mut padded = vec![if sound.bits == 8 { 128 } else { 0 }; (-latency) as usize * stride];
        padded.extend(sound.samples);
        sound.samples = padded;
    }
    let frames = (sound.samples.len() / stride) as u32;
    if let Some(n) = count {
        if n > frames {
            return Err("SWF event sound is shorter than its sample count".into());
        }
        sound.samples.truncate(n as usize * stride);
    }
    sound.frames = count.unwrap_or(frames);
    sound.loop_start = 0;
    sound.loop_end = sound.frames;
    Ok(sound)
}
impl Stream {
    pub fn new(header: &[u8]) -> R<Self> {
        if byte(header, 0)? & 0xf0 != 0 {
            return Err("invalid SWF stream reserved bits".into());
        }
        let (compression, rate, channels, bits) = format(byte(header, 1)?);
        let latency = if compression == 2 {
            i16le(header, 4)?
        } else {
            0
        };
        if header.len() != if compression == 2 { 6 } else { 4 } {
            return Err("invalid SWF stream header length".into());
        }
        u16le(header, 2)?;
        Ok(Self {
            rate,
            channels,
            compression,
            bits,
            latency,
            data: Vec::new(),
            positions: Vec::new(),
            samples: 0,
            previous: None,
        })
    }
    pub fn block(&mut self, frame: usize, data: &[u8]) -> R<()> {
        if self.previous.is_some_and(|last| frame <= last) {
            return Err("multiple SWF sound blocks in one frame".into());
        }
        if self.data.len() + data.len() > 64 * 1024 * 1024 {
            return Err("SWF compressed audio exceeds budget".into());
        }
        let (samples, seek, bytes) = if self.compression == 2 {
            let samples = u16le(data, 0)? as u32;
            let seek = i16le(data, 2)?;
            let bytes = &data[4..];
            if mp3_frames(bytes, self.rate, self.channels)? != samples {
                return Err("SWF MP3 block sample count disagrees".into());
            }
            if self.previous.is_none() && seek != self.latency {
                return Err("SWF first stream seek disagrees with latency".into());
            }
            (samples, seek, bytes)
        } else if self.compression == 3 {
            let stride = (self.channels * self.bits / 8) as usize;
            if data.len() % stride != 0 {
                return Err("truncated SWF PCM block".into());
            }
            ((data.len() / stride) as u32, 0, data)
        } else {
            return Err(format!(
                "unsupported SWF stream compression {}",
                self.compression
            ));
        };
        let position = if self.previous.is_none() {
            0
        } else {
            self.samples as i64 + seek - self.latency
        };
        if position < 0 || position > u32::MAX as i64 {
            return Err("SWF stream seek outside decoded audio".into());
        }
        self.positions.resize(frame + 1, u32::MAX);
        self.positions[frame] = position as u32;
        self.data.extend(bytes);
        self.samples = self
            .samples
            .checked_add(samples)
            .ok_or("SWF stream sample overflow")?;
        self.previous = Some(frame);
        Ok(())
    }
    pub fn finish(mut self, frames: usize) -> R<Option<Audio>> {
        if self.previous.is_none() {
            return Ok(None);
        }
        if self.positions.len() > frames {
            return Err("SWF sound block outside timeline".into());
        }
        let sound = trim(
            decoded(
                &self.data,
                self.compression,
                self.rate,
                self.channels,
                self.bits,
                self.samples,
            )?,
            self.latency,
            None,
        )?;
        self.positions.resize(frames, u32::MAX);
        if self
            .positions
            .iter()
            .any(|&n| n != u32::MAX && n > sound.frames)
        {
            return Err("SWF stream seek exceeds audio".into());
        }
        Ok(Some(Audio {
            sound,
            positions: self.positions,
        }))
    }
}

pub fn event(data: &[u8]) -> R<(usize, Sound)> {
    let id = u16le(data, 0)?;
    let (compression, rate, channels, bits) = format(byte(data, 2)?);
    let count = u32le(data, 3)? as u32;
    if count > 32 * 1024 * 1024 {
        return Err("SWF event sound exceeds budget".into());
    }
    let (bytes, latency, frames) = if compression == 2 {
        let bytes = data.get(9..).ok_or("truncated SWF event MP3")?;
        (bytes, i16le(data, 7)?, mp3_frames(bytes, rate, channels)?)
    } else {
        (data.get(7..).ok_or("truncated SWF event sound")?, 0, count)
    };
    Ok((
        id,
        trim(
            decoded(bytes, compression, rate, channels, bits, frames)?,
            latency,
            Some(count),
        )?,
    ))
}

pub fn wav(sound: &Sound) -> Vec<u8> {
    let mut out = Vec::with_capacity(44 + sound.samples.len());
    out.extend(b"RIFF");
    out.extend((36 + sound.samples.len() as u32).to_le_bytes());
    out.extend(b"WAVEfmt ");
    out.extend(16u32.to_le_bytes());
    out.extend(1u16.to_le_bytes());
    out.extend((sound.channels as u16).to_le_bytes());
    out.extend(sound.rate.to_le_bytes());
    out.extend((sound.rate * sound.channels * sound.bits / 8).to_le_bytes());
    out.extend(
        (sound.channels * sound.bits / 8)
            .to_le_bytes()
            .split_at(2)
            .0,
    );
    out.extend((sound.bits as u16).to_le_bytes());
    out.extend(b"data");
    out.extend((sound.samples.len() as u32).to_le_bytes());
    out.extend(&sound.samples);
    if sound.bits == 16 {
        for pair in out[44..].chunks_exact_mut(2) {
            pair.swap(0, 1);
        }
    }
    out
}

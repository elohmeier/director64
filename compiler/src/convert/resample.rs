//! Converted sounds at 48 kHz for the browser importer's Opus encoder.
//! Opus codes at 48 kHz; resampling here, rather than in the browser's
//! encoder, keeps the decoded sound aligned with the source to the sample
//! (the browser's own resampler adds latency) and makes the step
//! deterministic and testable.

type R<T> = Result<T, String>;

/// A PCM WAV's samples at 48 kHz, planar, as 32-bit floats.
pub struct Resampled {
    pub channels: usize,
    pub source_rate: u32,
    pub source_frames: usize,
    /// Frames per channel at 48 kHz.
    pub frames: usize,
    /// Channel after channel, each `frames` long.
    pub samples: Vec<f32>,
}

pub const RATE: u32 = 48_000;

/// A canonical PCM WAV (8-bit unsigned or 16-bit signed, any channel count)
/// resampled to 48 kHz with a windowed-sinc filter: 16 zero crossings each
/// side, a Blackman window, the cut-off just under the lower Nyquist rate.
pub fn wav_to_48k(wav: &[u8]) -> R<Resampled> {
    let (channels, rate, bits, data) = parse(wav)?;
    let width = bits as usize / 8;
    let source_frames = data.len() / (width * channels);
    let sample = |frame: usize, channel: usize| -> f32 {
        let at = (frame * channels + channel) * width;
        if bits == 8 {
            (data[at] as f32 - 128.0) / 128.0
        } else {
            i16::from_le_bytes([data[at], data[at + 1]]) as f32 / 32768.0
        }
    };
    let ratio = RATE as f64 / rate as f64;
    let frames = (source_frames as f64 * ratio).round() as usize;
    let mut samples = vec![0f32; frames * channels];
    if rate == RATE {
        for c in 0..channels {
            for i in 0..frames {
                samples[c * frames + i] = sample(i, c);
            }
        }
    } else {
        // Output frame i sits at source position i * rate / RATE: a whole
        // part and one of RATE / gcd phases, whose weights are computed once.
        const ZEROS: f64 = 8.0;
        let gcd = {
            let (mut a, mut b) = (RATE as u64, rate as u64);
            while b != 0 {
                (a, b) = (b, a % b);
            }
            a
        };
        let phases = (RATE as u64 / gcd) as usize;
        let cutoff = 0.95 * ratio.min(1.0);
        let half = (ZEROS / cutoff).ceil() as i64;
        let taps = (2 * half) as usize;
        let mut table = vec![0f32; phases * taps];
        for p in 0..phases {
            let fraction = p as f64 / phases as f64;
            let row = &mut table[p * taps..(p + 1) * taps];
            let mut total = 0.0;
            for (k, w) in row.iter_mut().enumerate() {
                // Tap k is source frame floor + k - half + 1.
                let d = (k as i64 - half + 1) as f64 - fraction;
                let x = d * cutoff;
                let sinc = if x.abs() < 1e-9 { 1.0 } else { (std::f64::consts::PI * x).sin() / (std::f64::consts::PI * x) };
                let t = d / half as f64;
                let window = if t.abs() >= 1.0 {
                    0.0
                } else {
                    let a = std::f64::consts::PI * (t + 1.0);
                    0.42 - 0.5 * a.cos() + 0.08 * (2.0 * a).cos()
                };
                *w = (sinc * window) as f32;
                total += sinc * window;
            }
            row.iter_mut().for_each(|w| *w = (*w as f64 / total) as f32);
        }
        let plane: Vec<Vec<f32>> = (0..channels).map(|c| (0..source_frames).map(|i| sample(i, c)).collect()).collect();
        for i in 0..frames {
            let position = i as u64 * rate as u64;
            let whole = (position / RATE as u64) as i64;
            let phase = ((position % RATE as u64) / gcd) as usize;
            let row = &table[phase * taps..(phase + 1) * taps];
            let first = whole - half + 1;
            for c in 0..channels {
                let source = &plane[c];
                let mut acc = 0f32;
                for (k, w) in row.iter().enumerate() {
                    let j = first + k as i64;
                    if j >= 0 && (j as usize) < source_frames {
                        acc += w * source[j as usize];
                    }
                }
                samples[c * frames + i] = acc.clamp(-1.0, 1.0);
            }
        }
    }
    Ok(Resampled { channels, source_rate: rate, source_frames, frames, samples })
}

fn parse(wav: &[u8]) -> R<(usize, u32, u16, &[u8])> {
    let le16 = |at: usize| wav.get(at..at + 2).map(|b| u16::from_le_bytes([b[0], b[1]]));
    let le32 = |at: usize| wav.get(at..at + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap()));
    if wav.get(..4) != Some(b"RIFF") || wav.get(8..12) != Some(b"WAVE") {
        return Err("not a WAVE file".into());
    }
    let (mut format, mut data) = (None, None);
    let mut at = 12;
    while at + 8 <= wav.len() {
        let size = le32(at + 4).ok_or("truncated WAVE")? as usize;
        let body = at + 8;
        match &wav[at..at + 4] {
            b"fmt " => {
                format = Some((le16(body).ok_or("fmt")?, le16(body + 2).ok_or("fmt")?, le32(body + 4).ok_or("fmt")?, le16(body + 14).ok_or("fmt")?))
            }
            b"data" => data = Some(&wav[body..(body + size).min(wav.len())]),
            _ => {}
        }
        at = body + size + (size & 1);
    }
    let (code, channels, rate, bits) = format.ok_or("WAVE without fmt")?;
    let data = data.ok_or("WAVE without data")?;
    if code != 1 || channels == 0 || rate == 0 || !(bits == 8 || bits == 16) {
        return Err(format!("unsupported WAVE: format {code}, {channels} channels, {bits} bits, {rate} Hz"));
    }
    Ok((channels as usize, rate, bits, data))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn wav(rate: u32, samples: &[i16]) -> Vec<u8> {
        let data: Vec<u8> = samples.iter().flat_map(|s| s.to_le_bytes()).collect();
        let mut out = b"RIFF".to_vec();
        out.extend_from_slice(&(36 + data.len() as u32).to_le_bytes());
        out.extend_from_slice(b"WAVEfmt ");
        for v in [16u32] {
            out.extend_from_slice(&v.to_le_bytes());
        }
        for v in [1u16, 1] {
            out.extend_from_slice(&v.to_le_bytes());
        }
        out.extend_from_slice(&rate.to_le_bytes());
        out.extend_from_slice(&(rate * 2).to_le_bytes());
        for v in [2u16, 16] {
            out.extend_from_slice(&v.to_le_bytes());
        }
        out.extend_from_slice(b"data");
        out.extend_from_slice(&(data.len() as u32).to_le_bytes());
        out.extend(data);
        out
    }

    #[test]
    fn a_tone_keeps_its_pitch_level_and_timing() {
        let rate = 22_050;
        let tone: Vec<i16> = (0..rate).map(|i| (16_000.0 * (i as f64 * 2.0 * std::f64::consts::PI * 441.0 / rate as f64).sin()) as i16).collect();
        let out = wav_to_48k(&wav(rate, &tone)).unwrap();
        assert_eq!((out.frames, out.source_frames, out.source_rate), (48_000, 22_050, 22_050));
        // Compare with the ideal tone away from the edges.
        let mut error: f64 = 0.0;
        for i in 1_000..47_000 {
            let ideal = 16_000.0 / 32_768.0 * (i as f64 * 2.0 * std::f64::consts::PI * 441.0 / 48_000.0).sin();
            error = error.max((out.samples[i] as f64 - ideal).abs());
        }
        assert!(error < 0.002, "{error}");
    }

    #[test]
    fn an_impulse_stays_in_place() {
        let mut click = vec![0i16; 11_025];
        click[5_000] = 30_000;
        let out = wav_to_48k(&wav(11_025, &click)).unwrap();
        let peak = (0..out.frames).max_by(|&a, &b| out.samples[a].total_cmp(&out.samples[b])).unwrap();
        let expected = (5_000.0 * 48_000.0 / 11_025.0f64).round() as usize;
        assert!(peak.abs_diff(expected) <= 1, "{peak} {expected}");
    }

    #[test]
    fn forty_eight_kilohertz_passes_through() {
        let out = wav_to_48k(&wav(48_000, &[0, 16_384, -16_384])).unwrap();
        assert_eq!(out.samples, vec![0.0, 0.5, -0.5]);
    }
}

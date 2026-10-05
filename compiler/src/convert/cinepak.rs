// SPDX-License-Identifier: LGPL-2.1-or-later
// Derived from FFmpeg's libavcodec/cinepak.c (Copyright (C) 2003 The FFmpeg
// project), whose decoding it reproduces bit for bit. This file is licensed
// under the GNU Lesser General Public License, version 2.1 or later; see
// https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.

//! Cinepak ('cvid') video, decoded to RGB24 frames bit for bit as ffmpeg's
//! libavcodec/cinepak.c does: per strip V1 and V4 codebooks of 2x2 YUV
//! blocks (converted to RGB when loaded, as ffmpeg does), partial codebook
//! and vector updates against the previous frame, and strips inheriting the
//! previous strip's codebooks unless the frame says otherwise. Löwenzahn's
//! linked movies are all 24-bit Cinepak.

const MAX_STRIPS: usize = 32;

#[derive(Clone)]
struct Strip {
    /// 256 entries of 4 RGB pixels (top-left, top-right, bottom-left,
    /// bottom-right).
    v4: Vec<[u8; 12]>,
    v1: Vec<[u8; 12]>,
    x1: usize,
    y1: usize,
    x2: usize,
    y2: usize,
}

pub struct Decoder {
    pub width: usize,
    pub height: usize,
    frame: Vec<u8>,
    strips: Vec<Strip>,
    /// Extra header bytes of Sega FILM data, settled by the first frame.
    sega_skip: Option<usize>,
}

fn be16(b: &[u8], at: usize) -> usize {
    (b[at] as usize) << 8 | b[at + 1] as usize
}
fn be24(b: &[u8], at: usize) -> usize {
    (b[at] as usize) << 16 | (b[at + 1] as usize) << 8 | b[at + 2] as usize
}
fn be32(b: &[u8], at: usize) -> u32 {
    u32::from_be_bytes(b[at..at + 4].try_into().unwrap())
}

fn codebook(book: &mut [[u8; 12]], chunk: u8, data: &[u8]) {
    let n = if chunk & 4 != 0 { 4 } else { 6 };
    let (mut flag, mut mask) = (0u32, 0u32);
    let mut at = 0;
    for entry in book.iter_mut() {
        if chunk & 1 != 0 {
            mask >>= 1;
            if mask == 0 {
                if at + 4 > data.len() {
                    break;
                }
                flag = be32(data, at);
                at += 4;
                mask = 0x8000_0000;
            }
        }
        if chunk & 1 == 0 || flag & mask != 0 {
            if at + n > data.len() {
                break;
            }
            for k in 0..4 {
                entry[k * 3..k * 3 + 3].fill(data[at + k]);
            }
            if n == 6 {
                let u = data[at + 4] as i8 as i32;
                let v = data[at + 5] as i8 as i32;
                for k in 0..4 {
                    let y = entry[k * 3] as i32;
                    entry[k * 3] = (y + v * 2).clamp(0, 255) as u8;
                    entry[k * 3 + 1] = (y - u / 2 - v).clamp(0, 255) as u8;
                    entry[k * 3 + 2] = (y + u * 2).clamp(0, 255) as u8;
                }
            }
            at += n;
        }
    }
}

impl Decoder {
    pub fn new(width: usize, height: usize) -> Self {
        let strip = Strip { v4: vec![[0; 12]; 256], v1: vec![[0; 12]; 256], x1: 0, y1: 0, x2: 0, y2: 0 };
        Decoder { width, height, frame: vec![0; width * height * 3], strips: vec![strip; MAX_STRIPS], sega_skip: None }
    }

    /// The picture as it stands: RGB24 rows of `width` pixels.
    pub fn frame(&self) -> &[u8] {
        &self.frame
    }

    /// Decodes one sample. Returns whether ffmpeg outputs a frame for it: an
    /// empty sample or one that fails the header checks outputs none, and a
    /// damaged strip keeps what was decoded before it.
    pub fn decode(&mut self, data: &[u8]) -> bool {
        if data.len() < 10 {
            return false;
        }
        let strips = be16(data, 8);
        if strips == 0 {
            return false;
        }
        let encoded = be24(data, 1);
        if data.len() < encoded || data.len() < 10 + strips * 12 {
            return false;
        }
        let skip = *self.sega_skip.get_or_insert_with(|| {
            if encoded != 0 && encoded != data.len() && !data.len().is_multiple_of(encoded) {
                if data.len() >= 16 && data[10..16] == [0xfe, 0, 0, 6, 0, 0] { 6 } else { 2 }
            } else {
                0
            }
        });
        if encoded == 0 {
            return false;
        }
        let first = 10 + skip;
        if first + 4 > data.len() {
            return false;
        }
        let size = be24(data, first + 1);
        if size < 12 || size > encoded {
            return false;
        }
        self.frame_body(data, skip);
        true
    }

    fn frame_body(&mut self, data: &[u8], skip: usize) {
        let flags = data[0];
        let count = be16(data, 8).min(MAX_STRIPS);
        let mut at = 10 + skip;
        let mut y0 = 0;
        for i in 0..count {
            if at + 12 > data.len() {
                return;
            }
            let h = &data[at..at + 12];
            let y1 = be16(h, 4);
            let (y1, y2) = if y1 == 0 { (y0, y0 + be16(h, 8)) } else { (y1, be16(h, 8)) };
            let (x1, x2) = (be16(h, 6), be16(h, 10));
            let Some(size) = be24(h, 1).checked_sub(12) else {
                return;
            };
            at += 12;
            let size = size.min(data.len() - at);
            if i > 0 && flags & 1 == 0 {
                let (before, after) = self.strips.split_at_mut(i);
                after[0].v4.clone_from(&before[i - 1].v4);
                after[0].v1.clone_from(&before[i - 1].v1);
            }
            let strip = &mut self.strips[i];
            (strip.x1, strip.y1, strip.x2, strip.y2) = (x1, y1, x2, y2);
            if !self.strip(i, &data[at..at + size]) {
                return;
            }
            at += size;
            y0 = y2;
        }
    }

    fn strip(&mut self, index: usize, data: &[u8]) -> bool {
        let (x1, y1, x2, y2) = {
            let s = &self.strips[index];
            (s.x1, s.y1, s.x2, s.y2)
        };
        // ffmpeg bounds strips by the picture rounded up to whole blocks.
        if x2 > (self.width + 3) & !3 || y2 > (self.height + 3) & !3 || x1 >= x2 || y1 >= y2 {
            return false;
        }
        let mut at = 0;
        while at + 4 <= data.len() {
            let chunk = data[at];
            let Some(size) = be24(data, at + 1).checked_sub(4) else {
                return false;
            };
            at += 4;
            let size = size.min(data.len() - at);
            let body = &data[at..at + size];
            match chunk {
                0x20 | 0x21 | 0x24 | 0x25 => codebook(&mut self.strips[index].v4, chunk, body),
                0x22 | 0x23 | 0x26 | 0x27 => codebook(&mut self.strips[index].v1, chunk, body),
                0x30..=0x32 => return self.vectors(index, chunk, body),
                _ => {}
            }
            at += size;
        }
        false
    }

    fn vectors(&mut self, index: usize, chunk: u8, data: &[u8]) -> bool {
        let strip = &self.strips[index];
        let (width, height) = (self.width, self.height);
        let stride = width * 3;
        let frame = &mut self.frame;
        let (mut flag, mut mask) = (0u32, 0u32);
        let mut at = 0;
        let mut y = strip.y1;
        while y < strip.y2 {
            // Rows past the picture's bottom collapse onto its last row, and
            // the block fills bottom-up so the true rows are written last.
            let r0 = y;
            let r1 = if height - y > 1 { y + 1 } else { r0 };
            let r2 = if height - y > 2 { y + 2 } else { r1 };
            let r3 = if height - y > 3 { y + 3 } else { r2 };
            let mut x = strip.x1;
            while x < strip.x2 {
                if chunk & 1 != 0 {
                    mask >>= 1;
                    if mask == 0 {
                        if at + 4 > data.len() {
                            return false;
                        }
                        flag = be32(data, at);
                        at += 4;
                        mask = 0x8000_0000;
                    }
                }
                if chunk & 1 == 0 || flag & mask != 0 {
                    if chunk & 2 == 0 {
                        mask >>= 1;
                        if mask == 0 {
                            if at + 4 > data.len() {
                                return false;
                            }
                            flag = be32(data, at);
                            at += 4;
                            mask = 0x8000_0000;
                        }
                    }
                    let base = x * 3;
                    let mut put = |row: usize, column: usize, pixel: &[u8]| {
                        let o = row * stride + base + column * 3;
                        if base + column * 3 < stride && o + 3 <= frame.len() {
                            frame[o..o + 3].copy_from_slice(pixel);
                        }
                    };
                    if chunk & 2 != 0 || !flag & mask != 0 {
                        if at >= data.len() {
                            return false;
                        }
                        let p = strip.v1[data[at] as usize];
                        at += 1;
                        for (rows, (left, right)) in [((r3, r2), (6, 9)), ((r1, r0), (0, 3))] {
                            for row in [rows.0, rows.1] {
                                put(row, 0, &p[left..left + 3]);
                                put(row, 1, &p[left..left + 3]);
                                put(row, 2, &p[right..right + 3]);
                                put(row, 3, &p[right..right + 3]);
                            }
                        }
                    } else if flag & mask != 0 {
                        if at + 4 > data.len() {
                            return false;
                        }
                        let cb = [data[at], data[at + 1], data[at + 2], data[at + 3]].map(|i| strip.v4[i as usize]);
                        at += 4;
                        for (row, a, b, half) in [(r3, 2, 3, 6), (r2, 2, 3, 0), (r1, 0, 1, 6), (r0, 0, 1, 0)] {
                            put(row, 0, &cb[a][half..half + 3]);
                            put(row, 1, &cb[a][half + 3..half + 6]);
                            put(row, 2, &cb[b][half..half + 3]);
                            put(row, 3, &cb[b][half + 3..half + 6]);
                        }
                    }
                }
                x += 4;
            }
            y += 4;
        }
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// One 4x4 key frame: a strip with a full V1 codebook entry 0 of luma
    /// 10/20/30/40 and zero chroma, and one V1-coded block.
    #[test]
    fn a_v1_block_upsamples_its_four_lumas() {
        let mut codebook = vec![0x26u8];
        let entries = [10u8, 20, 30, 40];
        let size = 4 + entries.len();
        codebook.extend_from_slice(&[(size >> 16) as u8, (size >> 8) as u8, size as u8]);
        codebook.extend_from_slice(&entries);
        let vectors = [0x32u8, 0, 0, 5, 0];
        let strip_body = [codebook, vectors.to_vec()].concat();
        let strip_size = 12 + strip_body.len();
        let mut strip = vec![0x10, (strip_size >> 16) as u8, (strip_size >> 8) as u8, strip_size as u8];
        strip.extend_from_slice(&[0, 0, 0, 0, 0, 4, 0, 4]);
        strip.extend(strip_body);
        let total = 10 + strip.len();
        let mut frame = vec![0, (total >> 16) as u8, (total >> 8) as u8, total as u8, 0, 4, 0, 4, 0, 1];
        frame.extend(strip);
        let mut decoder = Decoder::new(4, 4);
        assert!(decoder.decode(&frame));
        let luma: Vec<u8> = decoder.frame().chunks(3).map(|p| p[0]).collect();
        assert_eq!(luma, vec![10, 10, 20, 20, 10, 10, 20, 20, 30, 30, 40, 40, 30, 30, 40, 40]);
    }

    #[test]
    fn empty_and_short_samples_output_nothing() {
        let mut decoder = Decoder::new(4, 4);
        assert!(!decoder.decode(&[0; 9]));
        assert!(!decoder.decode(&[0, 0, 0, 10, 0, 4, 0, 4, 0, 0]));
    }
}

/// A movie's Cinepak frames in presentation order, as the browser importer
/// encodes them: each frame's time in microseconds and its pixels as RGBX,
/// the samples the edit list ends before dropped and a sample without a
/// picture skipped, as ffmpeg outputs them.
pub struct Frames<'a> {
    bytes: &'a [u8],
    samples: Vec<crate::convert::quicktime::Sample>,
    timescale: u64,
    decoder: Decoder,
    next: usize,
}

impl<'a> Frames<'a> {
    pub fn new(bytes: &'a [u8]) -> Result<Self, String> {
        let movie = crate::convert::quicktime::parse(bytes)?;
        let track = movie.video().ok_or("movie without video")?;
        if track.codec != "cvid" {
            return Err(format!("video codec {:?}", track.codec));
        }
        let samples = track.samples.iter().filter(|s| track.edit_end.is_none_or(|end| s.time < end)).cloned().collect();
        Ok(Frames {
            bytes,
            samples,
            timescale: track.timescale.max(1) as u64,
            decoder: Decoder::new(track.width as usize, track.height as usize),
            next: 0,
        })
    }
    pub fn width(&self) -> usize {
        self.decoder.width
    }
    pub fn height(&self) -> usize {
        self.decoder.height
    }
    /// The next frame: (microseconds, RGBX pixels).
    pub fn next_frame(&mut self) -> Result<Option<(u64, Vec<u8>)>, String> {
        while let Some(sample) = self.samples.get(self.next) {
            self.next += 1;
            let start = sample.offset as usize;
            let data = self.bytes.get(start..start + sample.size as usize).ok_or("sample outside the movie")?;
            if self.decoder.decode(data) {
                let rgbx = self.decoder.frame().chunks_exact(3).flat_map(|p| [p[0], p[1], p[2], 255]).collect();
                return Ok(Some((sample.time * 1_000_000 / self.timescale, rgbx)));
            }
        }
        Ok(None)
    }
}

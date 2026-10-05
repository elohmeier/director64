//! Byte and bit writers for the converter's fixtures, mirroring the Node
//! Buffer calls the JavaScript tests they replace used.
#![allow(dead_code)]

pub fn be16(b: &mut [u8], at: usize, v: i64) {
    b[at..at + 2].copy_from_slice(&(v as u16).to_be_bytes());
}
pub fn be32(b: &mut [u8], at: usize, v: i64) {
    b[at..at + 4].copy_from_slice(&(v as u32).to_be_bytes());
}
pub fn le16(b: &mut [u8], at: usize, v: i64) {
    b[at..at + 2].copy_from_slice(&(v as u16).to_le_bytes());
}
pub fn le32(b: &mut [u8], at: usize, v: i64) {
    b[at..at + 4].copy_from_slice(&(v as u32).to_le_bytes());
}
pub fn read_be16(b: &[u8], at: usize) -> u16 {
    u16::from_be_bytes([b[at], b[at + 1]])
}
pub fn read_be32(b: &[u8], at: usize) -> u32 {
    u32::from_be_bytes(b[at..at + 4].try_into().unwrap())
}
pub fn put(b: &mut [u8], at: usize, text: &[u8]) {
    b[at..at + text.len()].copy_from_slice(text);
}
pub fn latin1(text: &str) -> Vec<u8> {
    text.chars().map(|c| c as u32 as u8).collect()
}
pub fn concat(parts: &[&[u8]]) -> Vec<u8> {
    parts.concat()
}

/// MSB-first bit packing (the SWF fixtures' Writer).
#[derive(Default)]
pub struct Writer {
    bits: Vec<u8>,
}
impl Writer {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn put(&mut self, value: i64, count: u32) -> &mut Self {
        for i in (0..count).rev() {
            self.bits.push(((value as u32 >> i) & 1) as u8);
        }
        self
    }
    pub fn done(&self) -> Vec<u8> {
        let mut out = vec![0u8; self.bits.len().div_ceil(8)];
        for (i, bit) in self.bits.iter().enumerate() {
            out[i >> 3] |= bit << (7 - (i & 7));
        }
        out
    }
}

/// A 60x40-twip stage rectangle: 3x2 pixels.
pub fn stage_rect() -> Vec<u8> {
    Writer::new().put(7, 5).put(0, 7).put(60, 7).put(0, 7).put(40, 7).done()
}
pub fn tag(kind: u16, payload: &[u8]) -> Vec<u8> {
    let long = payload.len() >= 63;
    let mut out = ((kind << 6) | if long { 63 } else { payload.len() as u16 }).to_le_bytes().to_vec();
    if long {
        out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    }
    out.extend_from_slice(payload);
    out
}

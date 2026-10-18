//! Bounded VP6 import using Ruffle's MIT-relicensed NihAV decoder. The same
//! Rust implementation runs in native and WebAssembly conversion; playback
//! consumes converted timeline frames and never depends on browser VP6 support.
use nihav_core::codecs::{NADecoderSupport, NAVideoInfo, YUV420_FORMAT};
use nihav_duck::codecs::vp6::{VP6BR, VP56Decoder, VP56Parser};
use nihav_duck::codecs::vpcommon::BoolCoder;
type R<T> = Result<T, String>;
#[derive(Clone)]
pub struct Frame {
    pub width: usize,
    pub height: usize,
    pub yuv: Vec<u8>,
}
impl Frame {
    pub fn rgba(&self) -> Vec<[u8; 4]> {
        let size = self.width * self.height;
        let cw = self.width.div_ceil(2);
        let ch = self.height.div_ceil(2);
        (0..size)
            .map(|i| {
                let x = i % self.width;
                let y = i / self.width;
                let c = self.yuv[i] as i32 - 16;
                let u = self.yuv[size + (y / 2) * cw + x / 2] as i32 - 128;
                let v = self.yuv[size + cw * ch + (y / 2) * cw + x / 2] as i32 - 128;
                let rgb = [
                    (298 * c + 409 * v + 128) >> 8,
                    (298 * c - 100 * u - 208 * v + 128) >> 8,
                    (298 * c + 516 * u + 128) >> 8,
                ];
                [
                    rgb[0].clamp(0, 255) as u8,
                    rgb[1].clamp(0, 255) as u8,
                    rgb[2].clamp(0, 255) as u8,
                    255,
                ]
            })
            .collect()
    }
}
pub struct Decoder {
    width: usize,
    height: usize,
    encoded: Option<(usize, usize)>,
    decoder: VP56Decoder,
    support: NADecoderSupport,
    parser: VP6BR,
    last: Option<Frame>,
}
impl Decoder {
    pub fn new(width: usize, height: usize) -> R<Self> {
        if width == 0 || height == 0 || width > 2048 || height > 2048 {
            return Err("VP6 dimensions exceed import bounds".into());
        }
        Ok(Self {
            width,
            height,
            encoded: None,
            decoder: VP56Decoder::new(6, false, true),
            support: NADecoderSupport::new(),
            parser: VP6BR::new(),
            last: None,
        })
    }
    pub fn decode(&mut self, packet: &[u8]) -> R<Frame> {
        if packet.is_empty() {
            return self
                .last
                .clone()
                .ok_or("VP6 skip frame has no previous picture".into());
        }
        if packet.len() < 4 {
            return Err("truncated VP6 packet".into());
        }
        if packet.len() > 16 * 1024 * 1024 {
            return Err("VP6 packet exceeds import bounds".into());
        }
        let key = packet[0] & 128 == 0;
        if self.encoded.is_none() && !key {
            return Err("VP6 stream starts without a keyframe".into());
        }
        if key {
            let mut parser = VP6BR::new();
            let mut bits = BoolCoder::new(packet).map_err(|e| format!("VP6 header: {e:?}"))?;
            let header = parser
                .parse_header(&mut bits)
                .map_err(|e| format!("VP6 header: {e:?}"))?;
            let size = (header.mb_w as usize * 16, header.mb_h as usize * 16);
            if size.0 < self.width || size.1 < self.height || size.0 > 2048 || size.1 > 2048 {
                return Err("VP6 coded dimensions disagree with bounded video".into());
            }
            if self.encoded.is_some_and(|old| old != size) {
                return Err("VP6 coded dimensions change within stream".into());
            }
            if self.encoded.is_none() {
                self.decoder
                    .init(
                        &mut self.support,
                        NAVideoInfo::new(size.0, size.1, true, YUV420_FORMAT),
                    )
                    .map_err(|e| format!("VP6 initialization: {e:?}"))?;
                self.encoded = Some(size);
            }
        }
        let (buffer, _) = self
            .decoder
            .decode_frame(&mut self.support, packet, &mut self.parser)
            .map_err(|e| format!("VP6 frame: {e:?}"))?;
        let frame = buffer
            .get_vbuf()
            .ok_or("VP6 decoder returned no planar frame")?;
        let data = frame.get_data();
        let mut yuv = Vec::with_capacity(
            self.width * self.height + self.width.div_ceil(2) * self.height.div_ceil(2) * 2,
        );
        for component in 0..3 {
            let (w, h) = if component == 0 {
                (self.width, self.height)
            } else {
                (self.width.div_ceil(2), self.height.div_ceil(2))
            };
            let (actual_w, actual_h) = frame.get_dimensions(component);
            let stride = frame.get_stride(component);
            let base = frame.get_offset(component);
            if w > actual_w || h > actual_h || stride < actual_w {
                return Err("VP6 output plane dimensions disagree".into());
            }
            for y in 0..h {
                let row = data
                    .get(base + y * stride..base + y * stride + w)
                    .ok_or("VP6 output plane overruns buffer")?;
                yuv.extend_from_slice(row);
            }
        }
        let frame = Frame {
            width: self.width,
            height: self.height,
            yuv,
        };
        self.last = Some(frame.clone());
        Ok(frame)
    }
}

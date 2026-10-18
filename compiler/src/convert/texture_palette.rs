//! Original brush color quantization is a five-bit lookup, rather than a
//! nearest-color search. This measures the function on synthetic RGB samples;
//! retail brush pixels still come exclusively from the selected engine DLL.
use std::sync::OnceLock;

include!("texture_palette_data.rs");

fn indices() -> &'static [u8] {
    static TABLE: OnceLock<Vec<u8>> = OnceLock::new();
    TABLE.get_or_init(|| {
        let table = miniz_oxide::inflate::decompress_to_vec_zlib(RGB5_INDICES)
            .expect("built-in synthetic brush quantizer is valid zlib");
        assert_eq!(table.len(), 32768);
        table
    })
}

pub fn index(rgb: &[u8]) -> u8 {
    indices()
        [((rgb[0] as usize >> 3) << 10) | ((rgb[1] as usize >> 3) << 5) | (rgb[2] as usize >> 3)]
}

#[cfg(test)]
pub fn apply(rgba: &mut [u8]) {
    let palette = super::bitmap::windows_palette(8, true).expect("built-in Windows palette");
    for color in rgba.chunks_exact_mut(4) {
        let index = index(color) as usize;
        color[..3].copy_from_slice(&[
            palette[index * 6],
            palette[index * 6 + 2],
            palette[index * 6 + 4],
        ]);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn synthetic_original_observations_keep_lookup_boundaries_and_palette_aliases() {
        // Independent unaligned samples supplement the complete RGB5 census.
        // Blue191's legacy palette slot becomes blue128 in the D5+ palette;
        // the mapping also differs from a nearest-color search at (0,16,16).
        let cases = [
            ([0, 0, 0], [0, 0, 0]),
            ([128, 184, 232], [166, 202, 240]),
            ([255, 255, 255], [255, 255, 255]),
            ([0, 16, 16], [0, 0, 0]),
            ([0, 16, 32], [0, 0, 17]),
            ([0, 0, 176], [0, 0, 128]),
            ([0, 0, 191], [0, 0, 128]),
            ([1, 254, 37], [0, 255, 51]),
            ([3, 252, 111], [0, 255, 102]),
            ([17, 238, 117], [0, 255, 102]),
            ([31, 224, 123], [0, 204, 102]),
            ([45, 210, 129], [51, 204, 153]),
            ([97, 158, 5], [102, 153, 0]),
            ([115, 140, 159], [102, 153, 153]),
            ([151, 104, 211], [153, 102, 204]),
            ([191, 64, 155], [204, 51, 153]),
            ([224, 31, 96], [204, 0, 102]),
            ([254, 1, 182], [255, 0, 153]),
        ];
        for (input, expected) in cases {
            let mut pixel = [input[0], input[1], input[2], 127];
            apply(&mut pixel);
            assert_eq!(&pixel[..3], &expected, "input {input:?}");
            assert_eq!(pixel[3], 127);
        }
        assert_eq!(indices().len(), 32768);
    }
}

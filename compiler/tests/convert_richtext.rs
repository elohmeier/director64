use director64_aot::convert::{
    fdi::{FdiMeta, fdi_image, fdi_pixel},
    richtext::decode,
};

fn metadata(width: u16, height: u16) -> Vec<u8> {
    let mut bytes = vec![0; 34];
    bytes[4..6].copy_from_slice(&height.to_be_bytes());
    bytes[6..8].copy_from_slice(&width.to_be_bytes());
    bytes[25..28].copy_from_slice(&[255, 0, 0]);
    bytes[28..34].fill(255);
    bytes
}

fn bitmap(width: u16, height: u16, commands: &[u8]) -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend(width.to_be_bytes());
    bytes.extend(height.to_be_bytes());
    bytes.extend(4u16.to_be_bytes());
    bytes.extend(commands);
    bytes
}

#[test]
fn coverage_runs_color_commands_and_end_of_row_preserve_authored_pixels() {
    let rich = decode(
        &metadata(5, 2),
        &bitmap(5, 2, &[0, 1, 7, 15, 2, 0, 0, 31, 0, 255, 0, 15, 1, 3, 0, 0]),
    )
    .unwrap();
    assert_eq!(rich.background, [255; 3]);
    assert_eq!(
        rich.pixels,
        [
            255, 0, 0, 0, 255, 0, 0, 119, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0, 255, 0,
            255, 0, 255, 0, 51, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0,
        ]
    );
    let data = fdi_image(
        &FdiMeta {
            width: rich.width,
            height: rich.height,
            use_alpha: true,
            rgba32: true,
            rich_background: Some(rich.background),
            ..Default::default()
        },
        &rich.pixels,
    )
    .unwrap();
    assert_eq!(fdi_pixel(&data, 0, 0), [255; 4]);
    assert_eq!(fdi_pixel(&data, 1, 0), [255, 136, 136, 255]);
    assert_eq!(fdi_pixel(&data, 1, 36), [255, 0, 0, 119]);
    assert_eq!(fdi_pixel(&data, 5, 36), [0, 255, 0, 255]);
}

#[test]
fn rich_text_records_its_background() {
    let data = fdi_image(
        &FdiMeta {
            width: 20,
            height: 20,
            use_alpha: true,
            rgba32: true,
            rich_background: Some([0, 0, 255]),
            ..Default::default()
        },
        &[255, 255, 255, 119].repeat(400),
    )
    .unwrap();
    assert_eq!(&data[24..27], &[0, 0, 255]);
}

#[test]
fn malformed_rich_text_fails_at_the_format_boundary() {
    let meta = metadata(2, 1);
    for commands in [
        vec![15],
        vec![15, 3],
        vec![15, 0],
        vec![16],
        vec![31, 0, 0],
        vec![0, 0, 1],
    ] {
        assert!(
            decode(&meta, &bitmap(2, 1, &commands)).is_err(),
            "{commands:?}"
        );
    }
    assert!(decode(&meta, &bitmap(3, 1, &[0, 0])).is_err());
    let mut scrolling = meta.clone();
    scrolling[19] = 1;
    assert!(decode(&scrolling, &bitmap(2, 1, &[0, 0])).is_err());
}

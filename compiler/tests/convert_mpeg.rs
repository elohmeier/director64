use director64_aot::convert::mpeg::info;
fn sequence() -> Vec<u8> {
    vec![0, 0, 1, 0xb3, 0x14, 0, 0xf0, 0x13]
} // 320x240,25fps
fn picture() -> [u8; 6] {
    [0, 0, 1, 0, 0, 8]
}
fn packet(id: u8, payload: &[u8]) -> Vec<u8> {
    let mut out = vec![0, 0, 1, id];
    out.extend_from_slice(&(payload.len() as u16).to_be_bytes());
    out.extend_from_slice(payload);
    out
}
fn audio_frame(mode: u8) -> Vec<u8> {
    let mut frame = vec![0; 417]; // MPEG1 LayerII,128kb/s,44100Hz
    frame[..4].copy_from_slice(&[255, 253, 128, mode << 6]);
    frame
}
#[test]
fn silent_elementary_video_duration_counts_pictures() {
    let mut b = sequence();
    for _ in 0..50 {
        b.extend_from_slice(&picture());
    }
    let m = info(&b).unwrap();
    assert_eq!(m["width"], 320);
    assert_eq!(m["height"], 240);
    assert_eq!(m["frames"], 50);
    assert_eq!(m["duration_seconds"], 2.0);
}
#[test]
fn demux_handles_packet_split_start_codes_and_ignores_audio_lookalikes() {
    let mut elementary = sequence();
    elementary.extend_from_slice(&picture());
    elementary.extend_from_slice(&picture());
    let mut b = vec![0, 0, 1, 0xba, 0x21, 0, 1, 0, 1, 0x80, 0, 1];
    let mut audio = audio_frame(0);
    audio[20..26].copy_from_slice(&picture()); // audio payload can resemble a video picture
    let mut audio_payload = vec![0x0f];
    audio_payload.extend_from_slice(&audio);
    b.extend(packet(0xc0, &audio_payload));
    let mut payload = vec![0xff, 0x40, 0, 0x21, 0, 1, 0, 1];
    payload.extend_from_slice(&elementary[..10]);
    b.extend(packet(0xe0, &payload));
    b.extend_from_slice(&[0; 20]);
    let mut payload = vec![0x0f];
    payload.extend_from_slice(&elementary[10..]);
    b.extend(packet(0xe0, &payload));
    b.extend_from_slice(&[0, 0, 1, 0xb9]);
    assert_eq!(info(&b).unwrap()["frames"], 2);
    let streams = director64_aot::convert::mpeg::demux(&b).unwrap();
    let size = u32::from_le_bytes(streams[..4].try_into().unwrap()) as usize;
    assert_eq!(&streams[4..4 + size], elementary);
    assert_eq!(&streams[4 + size..], audio);
}
#[test]
fn mp2_mode_changes_and_partial_tail_have_explicit_counts() {
    let mut video = vec![0x0f];
    video.extend(sequence());
    video.extend(picture());
    let mut b = packet(0xe0, &video);
    let mut audio = vec![0x0f];
    audio.extend(audio_frame(1));
    audio.extend(audio_frame(0));
    audio.extend_from_slice(&audio_frame(1)[..24]);
    b.extend(packet(0xc0, &audio));
    let m = info(&b).unwrap();
    assert_eq!(m["audio_frames"], 2);
    assert_eq!(m["audio_rate"], 44100);
    assert_eq!(m["audio_trailing_bytes"], 24);
    // Corruption between complete audio frames must not become silent audio.
    let n = b.len();
    b[n - 24] = 0;
    assert!(info(&b).unwrap_err().contains("MP2 frame sync"));
}
#[test]
fn malformed_or_variable_timing_is_rejected() {
    assert!(info(&[0, 0, 1, 0xe0, 0xff, 0xff]).is_err());
    let mut b = sequence();
    b.extend_from_slice(&picture());
    b.extend_from_slice(&sequence());
    *b.last_mut().unwrap() = 0x14;
    assert!(info(&b).unwrap_err().contains("changing"));
    let mut b = sequence();
    b.extend_from_slice(&picture());
    b.extend_from_slice(&[0, 0, 1, 0xb5]);
    assert!(info(&b).is_err());
}

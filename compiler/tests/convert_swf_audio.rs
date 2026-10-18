use director64_aot::convert::swf::audio::{Stream, event, wav};

fn mp3_frame() -> Vec<u8> {
    let mut bytes = vec![0; 417];
    bytes[..4].copy_from_slice(&[0xff, 0xfb, 0x90, 0xc0]);
    bytes
}
fn block(frames: u16, seek: i16) -> Vec<u8> {
    let mut bytes = (frames * 1152).to_le_bytes().to_vec();
    bytes.extend(seek.to_le_bytes());
    bytes.extend(mp3_frame().repeat(frames as usize));
    bytes
}
#[test]
fn mp3_stream_preserves_seek_offsets_and_trims_initial_latency_once() {
    let mut stream = Stream::new(&[0x0e, 0x2e, 0, 4, 100, 0]).unwrap();
    stream.block(0, &block(2, 100)).unwrap();
    stream.block(2, &block(1, -1204)).unwrap();
    let audio = stream.finish(4).unwrap().unwrap();
    assert_eq!(audio.positions, vec![0, u32::MAX, 1000, u32::MAX]);
    assert_eq!(
        (audio.sound.frames, audio.sound.rate, audio.sound.channels),
        (3356, 44100, 1)
    );
    assert!(audio.sound.samples.iter().all(|&v| v == 0));
    let wave = wav(&audio.sound);
    assert_eq!(&wave[..4], b"RIFF");
    assert_eq!(&wave[8..12], b"WAVE");
    assert_eq!(wave.len(), 44 + 3356 * 2);
}
#[test]
fn empty_stream_header_is_not_an_audio_loss_and_pcm_is_little_endian() {
    assert!(
        Stream::new(&[0x0a, 0, 0, 0])
            .unwrap()
            .finish(7)
            .unwrap()
            .is_none()
    );
    let mut stream = Stream::new(&[0x06, 0x36, 2, 0]).unwrap();
    stream.block(1, &[0x34, 0x12, 0xcc, 0xed]).unwrap();
    let audio = stream.finish(2).unwrap().unwrap();
    assert_eq!(audio.positions, vec![u32::MAX, 0]);
    assert_eq!(audio.sound.samples, vec![0x12, 0x34, 0xed, 0xcc]);
    assert_eq!(&wav(&audio.sound)[44..], &[0x34, 0x12, 0xcc, 0xed]);
}
#[test]
fn stream_block_counts_boundaries_order_and_latency_are_checked() {
    let mut stream = Stream::new(&[0x0e, 0x2e, 0, 4, 100, 0]).unwrap();
    assert!(stream.block(0, &block(2, 0)).is_err());
    let mut bad = block(2, 100);
    bad[0] ^= 1;
    assert!(stream.block(0, &bad).is_err());
    let mut bad = block(2, 100);
    bad.pop();
    assert!(stream.block(0, &bad).is_err());
    stream.block(0, &block(2, 100)).unwrap();
    assert!(stream.block(0, &block(1, 0)).is_err());
    assert!(stream.finish(0).is_err());
}
#[test]
fn event_mp3_honors_exact_declared_samples_and_negative_latency_silence() {
    let mut data = vec![7, 0, 0x2e];
    data.extend(2000u32.to_le_bytes());
    data.extend(100i16.to_le_bytes());
    data.extend(mp3_frame().repeat(2));
    let (id, sound) = event(&data).unwrap();
    assert_eq!((id, sound.frames, sound.samples.len()), (7, 2000, 4000));
    data[7..9].copy_from_slice(&(-100i16).to_le_bytes());
    data[3..7].copy_from_slice(&2404u32.to_le_bytes());
    assert_eq!(event(&data).unwrap().1.frames, 2404);
    data[3..7].copy_from_slice(&2405u32.to_le_bytes());
    assert!(event(&data).is_err());
}

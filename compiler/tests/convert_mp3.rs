use director64_aot::convert::mp3;

#[test]
fn shockwave_blank_leading_frame_is_narrowly_bounded_and_not_general_resync() {
    for first in [0, 255] {
        let mut resource = vec![0; 417];
        resource[0] = first;
        resource.extend(frame(9, false).repeat(3));
        assert!(mp3::decode(&resource).is_err());
        assert_eq!(mp3::decode_shockwave_cast(&resource).unwrap().frames, 3456);
        let mut corrupt = resource.clone();
        corrupt[20] = 1;
        assert!(mp3::decode_shockwave_cast(&corrupt).is_err());
        let mut wrong_length = resource.clone();
        wrong_length.insert(4, 0);
        assert!(mp3::decode_shockwave_cast(&wrong_length).is_err());
        let mut truncated = resource.clone();
        truncated.pop();
        assert!(mp3::decode_shockwave_cast(&truncated).is_err());
    }
}

// Source-free MPEG-1 Layer III frames: zero side information means no
// quantized spectral values, so synthesis produces silence. There is no
// bit reservoir dependency, ID3 content or game media in this fixture.
fn frame(bitrate: u8, stereo: bool) -> Vec<u8> {
    let kbps = match bitrate {
        9 => 128,
        10 => 160,
        _ => panic!("fixture bitrate"),
    };
    let mut bytes = vec![0; 144 * kbps * 1000 / 44100];
    bytes[..4].copy_from_slice(&[0xff, 0xfb, bitrate << 4, if stereo { 0 } else { 0xc0 }]);
    bytes
}

#[test]
fn compressed_cast_decodes_complete_mono_frames_and_zero_allocation_tail() {
    let mut bytes = frame(9, false).repeat(3);
    bytes.extend([0; 120]);
    let sound = mp3::decode(&bytes).unwrap();
    assert_eq!(
        (sound.rate, sound.channels, sound.bits, sound.frames),
        (44100, 1, 16, 3456)
    );
    assert_eq!((sound.loop_start, sound.loop_end), (0, 3456));
    assert_eq!(sound.samples.len(), 3456 * 2);
    assert!(sound.samples.iter().all(|v| *v == 0));
}

#[test]
fn vbr_stereo_and_empty_id3_tags_preserve_all_sample_frames() {
    let mut bytes = b"ID3\x03\0\0\0\0\0\0".to_vec();
    bytes.extend(frame(9, true));
    bytes.extend(frame(10, true));
    bytes.extend(frame(9, true));
    let sound = mp3::decode(&bytes).unwrap();
    assert_eq!((sound.rate, sound.channels, sound.frames), (44100, 2, 3456));
    assert_eq!(sound.samples.len(), 3456 * 4);
    assert!(sound.samples.iter().all(|v| *v == 0));
}

#[test]
fn id3_tail_can_begin_in_unused_final_frame_ancillary_bytes() {
    let mut bytes = frame(9, false).repeat(3);
    bytes.truncate(bytes.len() - 8);
    bytes.extend(b"TAG");
    bytes.extend([0; 124]);
    bytes.push(255);
    let sound = mp3::decode(&bytes).unwrap();
    assert_eq!(sound.frames, 3456);
    assert!(sound.samples.iter().all(|v| *v == 0));
}

#[test]
fn missing_partial_or_corrupt_frames_do_not_become_successful_audio() {
    let valid = frame(9, false).repeat(3);
    assert!(mp3::decode(&[]).is_err());
    for n in [1, 3, valid.len() - 1] {
        assert!(mp3::decode(&valid[..n]).is_err(), "truncated at {n}");
    }
    let mut bad = valid.clone();
    bad[417] = 0;
    assert!(mp3::decode(&bad).err().unwrap().contains("frame header"));
    let mut changed = frame(9, false);
    changed.extend(frame(9, true));
    changed.extend(frame(9, true));
    assert!(mp3::decode(&changed).is_err());
    let mut tag = b"ID3\x03\0\0\0\0\0\x7f".to_vec();
    assert!(mp3::decode(&tag).is_err());
    tag[6] = 0x80;
    assert!(mp3::decode(&tag).is_err());
}

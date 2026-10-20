use director64_aot::convert::{
    files::{Files, Memory},
    stream, swa,
};

fn source() -> Vec<u8> {
    let mut b = vec![0; 396];
    for (at, value) in [
        (0, 392u32),
        (4, 3),
        (8, 44100),
        (16, 100),
        (20, 3000),
        (320, 2),
        (324, 40),
        (360, 20),
    ] {
        b[at..at + 4].copy_from_slice(&value.to_be_bytes());
    }
    b[32..36].copy_from_slice(&[0, 2, 0, 1]);
    b[328..333].copy_from_slice(b"later");
    b[364..371].copy_from_slice(b"earlier");
    // Source-free MPEG-1 Layer III silence, with complete frame boundaries.
    let mut frame = vec![0; 417];
    frame[..4].copy_from_slice(&[255, 251, 144, 192]);
    b.extend(frame.repeat(3));
    b
}

#[test]
fn swa_trims_encoder_delay_preserves_samples_and_orders_cue_times() {
    let b = source();
    let audio = swa::decode(&b).unwrap();
    assert_eq!(
        (audio.sound.rate, audio.sound.channels, audio.sound.frames),
        (44100, 1, 3000)
    );
    assert_eq!(audio.sound.samples.len(), 6000);
    assert!(audio.sound.samples.iter().all(|b| *b == 0));
    assert_eq!(audio.cues[0]["name"], "earlier");
    assert_eq!(audio.cues[1]["milliseconds"], 40);
    let (wav, duration) = stream::browser_stream(&b).unwrap();
    assert_eq!(duration, 68);
    assert_eq!(wav.len(), 6044);
    assert_eq!(&wav[..4], b"RIFF");
}

#[test]
fn partial_audio_and_inconsistent_headers_are_rejected() {
    let valid = source();
    for length in [0, 12, 323, 395, valid.len() - 1] {
        assert!(swa::decode(&valid[..length]).is_err());
    }
    for (offset, value) in [
        (0, 324u32),
        (4, 4),
        (8, 22050),
        (16, 10000),
        (20, 10000),
        (320, 1000),
        (324, 10000),
    ] {
        let mut b = valid.clone();
        b[offset..offset + 4].copy_from_slice(&value.to_be_bytes());
        assert!(swa::decode(&b).is_err(), "offset {offset}");
    }
}

#[test]
fn linked_swa_resolves_case_without_path_escape_or_ambiguous_names() {
    let mut fs = Memory::new();
    fs.mkdir_all("disc/SOUNDS").unwrap();
    fs.write("disc/SOUNDS/VOICE.SWA", &source()).unwrap();
    let descriptor = |path: &str| {
        let mut b = b"MACR".to_vec();
        b.extend((path.len() as u32).to_be_bytes());
        b.extend([0; 12]);
        b.extend(path.as_bytes());
        b
    };
    assert_eq!(
        swa::linked_path(&fs, "disc", &descriptor("@:sounds:voice.swa")).unwrap(),
        "disc/SOUNDS/VOICE.SWA"
    );
    for path in [
        "C:\\VOICE.SWA",
        "@::VOICE.SWA",
        "@:..:VOICE.SWA",
        "@:SOUNDS:missing.swa",
    ] {
        assert!(swa::linked_path(&fs, "disc", &descriptor(path)).is_err());
    }
    fs.write("disc/SOUNDS/voice.swa", &[]).unwrap();
    assert!(swa::linked_path(&fs, "disc", &descriptor("@:sounds:voice.swa")).is_err());
}

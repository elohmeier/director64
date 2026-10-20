//! Shockwave Audio v3 files and linked SWA cast descriptors. Layout facts
//! corroborated against all 135 local Löwenzahn 3 sound headers: a bounded
//! 324-byte header plus 36-byte cue records, followed by complete MP3 frames.
use super::{
    d5media::Sound,
    files::{self, Files},
    mp3,
};
use serde_json::{Value, json};

pub struct Audio {
    pub sound: Sound,
    pub cues: Vec<Value>,
}

fn word(b: &[u8], at: usize) -> Result<u32, String> {
    Ok(u32::from_be_bytes(
        b.get(at..at + 4)
            .ok_or("truncated SWA header")?
            .try_into()
            .unwrap(),
    ))
}

pub fn decode(bytes: &[u8]) -> Result<Audio, String> {
    if bytes.len() < 324 || word(bytes, 4)? != 3 {
        return Err("unsupported SWA file header".into());
    }
    let start = 4usize + word(bytes, 0)? as usize;
    let count = word(bytes, 320)? as usize;
    if count > 65535 || start != 324 + count * 36 || start >= bytes.len() {
        return Err("invalid SWA header or cue extent".into());
    }
    let rate = word(bytes, 8)?;
    let skip = word(bytes, 16)? as usize;
    let frames = word(bytes, 20)? as usize;
    let channels = u16::from_be_bytes(bytes[34..36].try_into().unwrap()) as u32;
    if u16::from_be_bytes(bytes[32..34].try_into().unwrap()) != 2 || ![1, 2].contains(&channels) {
        return Err("unsupported SWA sample format".into());
    }
    let mut sound = mp3::decode(&bytes[start..])?;
    if sound.rate != rate
        || sound.channels != channels
        || sound.bits != 16
        || frames == 0
        || skip as u64 + frames as u64 > sound.frames as u64
    {
        return Err("SWA decoded sample bounds disagree with header".into());
    }
    let width = channels as usize * 2;
    sound.samples = sound.samples[skip * width..(skip + frames) * width].to_vec();
    sound.frames = frames as u32;
    sound.loop_start = 0;
    sound.loop_end = frames as u32;
    let mut cues = Vec::new();
    for i in 0..count {
        let at = 324 + i * 36;
        let milliseconds = word(bytes, at)?;
        let raw = &bytes[at + 4..at + 36];
        let end = raw
            .iter()
            .position(|b| *b == 0)
            .ok_or("unterminated SWA cue name")?;
        if end == 0 || milliseconds as u64 * rate as u64 > frames as u64 * 1000 {
            return Err("invalid SWA cue timing or name".into());
        }
        cues.push(json!({"milliseconds":milliseconds,"name":super::js::mac_roman(&raw[..end])}));
    }
    cues.sort_by_key(|cue| cue["milliseconds"].as_u64().unwrap());
    Ok(Audio { sound, cues })
}

pub fn wav(sound: &Sound) -> Vec<u8> {
    let n = sound.samples.len() as u32;
    let mut out = b"RIFF".to_vec();
    out.extend((36 + n).to_le_bytes());
    out.extend(b"WAVEfmt ");
    out.extend(16u32.to_le_bytes());
    out.extend(1u16.to_le_bytes());
    out.extend((sound.channels as u16).to_le_bytes());
    out.extend(sound.rate.to_le_bytes());
    out.extend((sound.rate * sound.channels * 2).to_le_bytes());
    out.extend((sound.channels as u16 * 2).to_le_bytes());
    out.extend(16u16.to_le_bytes());
    out.extend(b"data");
    out.extend(n.to_le_bytes());
    for pair in sound.samples.chunks_exact(2) {
        out.extend([pair[1], pair[0]]);
    }
    out
}

pub fn linked_path(fs: &dyn Files, root: &str, payload: &[u8]) -> Result<String, String> {
    optional_linked_path(fs, root, payload)?.ok_or_else(|| "unresolved SWA path".into())
}

pub fn optional_linked_path(
    fs: &dyn Files,
    root: &str,
    payload: &[u8],
) -> Result<Option<String>, String> {
    if payload.len() < 20
        || payload.get(..4) != Some(b"MACR")
        || word(payload, 4)? as usize != payload.len() - 20
    {
        return Err("unsupported linked SWA descriptor".into());
    }
    let authored = super::js::latin1(&payload[20..]);
    let relative = authored
        .strip_prefix("@:")
        .or_else(|| authored.strip_prefix("@\\"))
        .ok_or("SWA path is not relative to its movie")?;
    let components: Vec<_> = relative.split([':', '\\', '/']).collect();
    if components
        .iter()
        .any(|c| c.is_empty() || *c == "." || *c == "..")
    {
        return Err("invalid linked SWA path component".into());
    }
    let mut path = root.to_string();
    for component in components {
        let matches: Vec<_> = fs
            .list(&path)?
            .into_iter()
            .filter(|(name, _)| name.eq_ignore_ascii_case(component))
            .collect();
        if matches.is_empty() {
            return Ok(None);
        }
        if matches.len() != 1 {
            return Err(format!("ambiguous SWA path {authored}"));
        }
        path = files::join(&path, &matches[0].0);
    }
    if !fs.is_file(&path) {
        return Err(format!("SWA path is not a file: {authored}"));
    }
    Ok(Some(path))
}

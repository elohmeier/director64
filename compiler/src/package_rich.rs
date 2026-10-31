//! Optional RICH member descriptors; existing MOVI layout is unchanged.
use crate::scene::Scene;
pub fn write_rich(scenes: &[Scene]) -> Result<Vec<u8>, String> {
    let members: Vec<_> = scenes
        .iter()
        .flat_map(|s| &s.members)
        .filter(|m| !m.text_formats.is_empty())
        .collect();
    let mut bytes = Vec::new();
    bytes.extend_from_slice(
        &u32::try_from(members.len())
            .map_err(|_| "too many rich text members")?
            .to_le_bytes(),
    );
    for member in members {
        bytes.extend_from_slice(&member.id.to_le_bytes());
        bytes.extend_from_slice(
            &u16::try_from(member.text_formats.len())
                .map_err(|_| "too many text format runs")?
                .to_le_bytes(),
        );
        bytes.extend_from_slice(&0u16.to_le_bytes());
        for run in &member.text_formats {
            bytes.extend_from_slice(&run.offset.to_le_bytes());
            bytes.extend_from_slice(&[run.font, run.size, run.face, run.align]);
            bytes.extend_from_slice(&run.ascent.to_le_bytes());
            bytes.extend_from_slice(&run.line_height.to_le_bytes());
            bytes.extend_from_slice(&run.rgb.to_le_bytes());
        }
    }
    Ok(bytes)
}

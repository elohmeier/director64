use crate::scene::Scene;
pub fn write(scenes: &[Scene]) -> Result<Vec<u8>, String> {
    let members: Vec<_> = scenes
        .iter()
        .flat_map(|s| &s.members)
        .filter(|m| m.text_box.is_some())
        .collect();
    let mut out = Vec::new();
    out.extend_from_slice(
        &u32::try_from(members.len())
            .map_err(|_| "too many text boxes")?
            .to_le_bytes(),
    );
    for member in members {
        let data = member.text_box.as_ref().unwrap();
        out.extend_from_slice(&member.id.to_le_bytes());
        out.extend_from_slice(&[
            data.kind,
            data.margin,
            data.border,
            data.shadow,
            u8::from(data.wrap),
            (data.background >> 16) as u8,
            (data.background >> 8) as u8,
            data.background as u8,
        ]);
    }
    Ok(out)
}

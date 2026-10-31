//! Optional transition cast metadata. Existing MOVI records stay compatible.
use crate::scene::Scene;

pub fn write_transitions(scenes: &[Scene]) -> Result<Vec<u8>, String> {
    let members: Vec<_> = scenes
        .iter()
        .flat_map(|s| &s.members)
        .filter(|m| m.transition.is_some())
        .collect();
    let count = u32::try_from(members.len()).map_err(|_| "too many transition members")?;
    let mut bytes = Vec::with_capacity(4 + members.len() * 12);
    bytes.extend_from_slice(&count.to_le_bytes());
    for member in members {
        if member.kind != 14 {
            return Err("transition metadata on a non-transition member".into());
        }
        let transition = member.transition.as_ref().unwrap();
        bytes.extend_from_slice(&member.id.to_le_bytes());
        bytes.extend_from_slice(&transition.duration_ms.to_le_bytes());
        bytes.extend_from_slice(&[transition.kind, transition.chunk, transition.area, 0, 0, 0]);
    }
    Ok(bytes)
}

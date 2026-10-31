//! Optional authored character spacing; RICH's existing16-byte runs stay intact.
use crate::scene::Scene;
pub fn write(scenes: &[Scene]) -> Result<Vec<u8>, String> {
    let runs: Vec<_> = scenes
        .iter()
        .flat_map(|s| &s.members)
        .flat_map(|m| {
            m.text_formats
                .iter()
                .filter(|r| r.char_spacing != 0)
                .map(move |r| (m.id, r))
        })
        .collect();
    let mut out = Vec::new();
    out.extend_from_slice(
        &u32::try_from(runs.len())
            .map_err(|_| "too many character spacing runs")?
            .to_le_bytes(),
    );
    for (id, run) in runs {
        out.extend_from_slice(&id.to_le_bytes());
        out.extend_from_slice(&run.offset.to_le_bytes());
        out.extend_from_slice(&run.char_spacing.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes());
    }
    Ok(out)
}

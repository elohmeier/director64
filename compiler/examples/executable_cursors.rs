//! Bounded, read-only-source verification of a pinned cursor recovery policy.
use director64_aot::convert::{
    executable_cursor,
    files::{Disk, Files},
    source::sha256,
};
use serde_json::{Value, json};
use std::{env, path::PathBuf};

fn main() -> Result<(), String> {
    let args: Vec<_> = env::args().skip(1).collect();
    if args.len() != 3 {
        return Err("usage: executable_cursors MEDIA POLICY OUTPUT".into());
    }
    let mut fs = Disk {
        root: PathBuf::from("."),
    };
    let policy: Value = serde_json::from_slice(&fs.read(&args[1])?).map_err(|e| e.to_string())?;
    fs.mkdir_all(&format!("{}/images", args[2]))?;
    let mut movies = vec![json!({})];
    executable_cursor::bind(&mut fs, &args[0], &args[2], &policy, &mut movies)?;
    let mut records = Vec::new();
    for asset in movies[0]["cursorAssets"]
        .as_array()
        .ok_or("no selected cursor assets")?
    {
        let name = asset.as_str().ok_or("cursor asset name")?;
        let bytes = fs.read(&format!("{}/images/{name}", args[2]))?;
        let word = |at| u16::from_be_bytes([bytes[at], bytes[at + 1]]);
        records.push(json!({"asset":name,"sha256":sha256(&bytes),"width":word(4),
            "height":word(6),"hotspot":[word(8),word(10)],
            "outcomes":(0..4).map(|code|bytes[16..].iter().filter(|b|**b==code).count()).collect::<Vec<_>>() }));
    }
    fs.write(
        &format!("{}/verification.json", args[2]),
        serde_json::to_string_pretty(&json!({
        "kind":"pinned-source-cursor-recovery","source":policy["executable_cursors"],
        "records":records,"original_projector_evidence":false}))
        .unwrap()
        .as_bytes(),
    )?;
    println!("{}", serde_json::to_string(&records).unwrap());
    Ok(())
}

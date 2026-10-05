//! The converter's compiler stage as a library: lowering to bytecode, the
//! generated C units the console links, and the game package the browser
//! runtime loads. The `director64-aot` binary and the Wasm build share it.

pub mod ast;
pub mod bytecode;
pub mod compiler;
pub mod convert;
pub mod emit;
pub mod iso;
pub mod lingo;
pub mod names;
pub mod normalize;
pub mod package;
pub mod ports;
pub mod pyfmt;
pub mod scene;
pub mod zip;

/// Compiles a parsed program and its scene model into a package.
pub fn build_package(
    program_json: &str,
    model_json: &str,
    names_txt: &str,
    bytecode_header: &[u8],
    meta: &str,
) -> Result<Vec<u8>, String> {
    let program_value: serde_json::Value = serde_json::from_str(program_json).map_err(|e| format!("program: {e}"))?;
    let model: serde_json::Value = serde_json::from_str(model_json).map_err(|e| format!("model: {e}"))?;
    scene::validate_script_casts(&model, &program_value)?;
    let program = ast::Program::from_json(&program_value)?;
    let names = names::parse(names_txt)?;
    let compiled = emit::compile_program(&program, &names)?;
    let profile = package_profile(&model, meta)?;
    let scenes = scene::scenes(&model, profile)?;
    package::write(&compiled, &names, &scenes, profile, package::abi_digest(names_txt.as_bytes(), bytecode_header), meta)
}

/// The runtime family a package targets: the port's Director version from
/// the package metadata, and the model's extended-D6 flag.
pub fn package_profile(model: &serde_json::Value, meta: &str) -> Result<scene::Profile, String> {
    let meta: serde_json::Value = serde_json::from_str(meta).map_err(|e| format!("package meta: {e}"))?;
    let version = match meta.get("director_version").and_then(serde_json::Value::as_u64) {
        Some(v @ (500 | 600 | 700 | 800 | 1000)) => v as u16,
        Some(v) => return Err(format!("unsupported Director version {v}")),
        None => 600,
    };
    let extended = model.get("extendedD6").and_then(serde_json::Value::as_bool).unwrap_or(false);
    Ok(scene::Profile { version, extended })
}

#[cfg(test)]
mod tests {
    use super::*;

    const NAMES: &str = "go\nstartmovie\n";
    fn program() -> String {
        r#"{"files":[{"name":"X.DXR.lingo"}],"handlers":[{"movie":"X.DXR","name":"startmovie","member":1,
            "cast":"Internal","script_type":"MovieScript","parameters":[],"properties":[],
            "line":1,"body":[{"op":"global","line":2,"names":["gscore"]}]}]}"#
            .to_string()
    }
    fn model(extra: &str) -> String {
        format!(
            r#"{{"problems":[],{extra}"movies":[{{"name":"X.DXR","id":1,"tempo":30,
            "casts":[{{"name":"Internal","file":"X.DXR","number":1}}],
            "members":[{{"cast":1,"number":2,"type":1,"width":4,"height":2,"asset":"a.fdi","name":"Pic"}}],
            "score":{{"frames":[{{"channels":[{{"channel":6,"bytes":[16,8,0,0,0,1,0,2,0,0,0,0,0,10,0,20,0,2,0,4,0,0,0,0],
            "changed":[true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true],
            "behaviors":[]}}]}}],"labels":[{{"name":"start","frame":1}}]}}}}]}}"#
        )
    }

    #[test]
    fn a_package_carries_its_header_abi_and_sections() {
        let bytes = build_package(&program(), &model(""), NAMES, b"opcodes", "{}").unwrap();
        assert_eq!(&bytes[..4], b"D64P");
        assert_eq!(u16::from_le_bytes([bytes[4], bytes[5]]), package::FORMAT);
        assert_eq!(u16::from_le_bytes([bytes[6], bytes[7]]), 600);
        assert_eq!(&bytes[12..28], &package::abi_digest(NAMES.as_bytes(), b"opcodes"));
        let tags: Vec<&[u8]> = (0..6).map(|i| &bytes[32 + i * 12..36 + i * 12]).collect();
        assert_eq!(tags, [b"META", b"STRS", b"CODE", b"SYMB", b"GLOB", b"MOVI"].map(|t| &t[..]));
        // A different runtime vocabulary is a different ABI.
        let other = build_package(&program(), &model(""), "go\nstartmovie\nzzz\n", b"opcodes", "{}").unwrap();
        assert_ne!(&bytes[12..28], &other[12..28]);
    }

    #[test]
    fn the_header_names_the_runtime_family() {
        let meta = r#"{"director_version":600}"#;
        let bytes = build_package(&program(), &model(r#""extendedD6":true,"#), NAMES, b"", meta).unwrap();
        assert_eq!((u16::from_le_bytes([bytes[6], bytes[7]]), u16::from_le_bytes([bytes[8], bytes[9]])), (600, 1));
        let error = build_package(&program(), &model(""), NAMES, b"", r#"{"director_version":900}"#).unwrap_err();
        assert!(error.contains("unsupported Director version"), "{error}");
    }

    #[test]
    fn scene_deltas_follow_the_console_generator() {
        let model: serde_json::Value = serde_json::from_str(&model("")).unwrap();
        let scenes = scene::scenes(&model, scene::Profile { version: 600, extended: false }).unwrap();
        let delta = &scenes[0].deltas[0];
        assert_eq!(delta.channel, 6);
        assert_eq!(delta.mask & scene::DG_SPAN, scene::DG_SPAN);
        assert_eq!(delta.spec.member, 1 << 20 | 1 << 16 | 2);
        assert_eq!((delta.spec.x, delta.spec.y, delta.spec.width, delta.spec.height), (20, 10, 4, 2));
        assert_eq!((delta.spec.kind, delta.spec.ink, delta.spec.blend), (16, 8, 100));
        assert_eq!(scenes[0].labels, vec![("start".to_string(), 1)]);
        assert_eq!(scenes[0].member_index, vec![(emit::text_hash("Pic"), 0)]);
    }
}

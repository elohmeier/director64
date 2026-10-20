//! The converter's compiler stage as a library: lowering to bytecode, the
//! generated C units the console links, and the game package the browser
//! runtime loads. The `director64-aot` binary and the Wasm build share it.

pub mod ast;
pub mod bytecode;
pub mod capabilities;
pub mod compiler;
pub mod convert;
pub mod emit;
pub mod iso;
pub mod lingo;
pub mod names;
pub mod normalize;
pub mod package;
pub mod ports;
pub mod scene;
pub mod zip;

/// Recovered control-flow trees can exceed serde_json's default depth 128.
/// Bound nesting before deserialization, including malformed input, so the
/// relaxed parser never accepts an unbounded recursive allocation.
pub fn parse_json(source: &str) -> Result<serde_json::Value, String> {
    let (mut depth, mut quoted, mut escaped) = (0usize, false, false);
    for byte in source.bytes() {
        if escaped { escaped = false; continue }
        if quoted {
            if byte == b'\\' { escaped = true }
            else if byte == b'"' { quoted = false }
        } else if byte == b'"' { quoted = true }
        else if byte == b'[' || byte == b'{' {
            depth += 1;
            if depth > 1024 { return Err("JSON nesting exceeds 1024".into()) }
        } else if byte == b']' || byte == b'}' { depth = depth.saturating_sub(1) }
    }
    let mut parser = serde_json::Deserializer::from_str(source);
    parser.disable_recursion_limit();
    let mut values = parser.into_iter::<serde_json::Value>();
    let value = values.next().ok_or("empty JSON")?.map_err(|e| e.to_string())?;
    if values.next().is_some() { return Err("trailing JSON value".into()) }
    Ok(value)
}

/// Compiles a parsed program and its scene model into a package.
pub fn build_package(
    program_json: &str,
    model_json: &str,
    names_txt: &str,
    bytecode_header: &[u8],
    meta: &str,
) -> Result<Vec<u8>, String> {
    let program_value: serde_json::Value =
        parse_json(program_json).map_err(|e| format!("program: {e}"))?;
    let model: serde_json::Value =
        serde_json::from_str(model_json).map_err(|e| format!("model: {e}"))?;
    scene::validate_script_casts(&model, &program_value)?;
    let program = ast::Program::from_json(&program_value)?;
    let names = names::parse(names_txt)?;
    let compiled = emit::compile_program(&program, &names)?;
    let profile = package_profile(&model, meta)?;
    let mut metadata: serde_json::Value =
        serde_json::from_str(meta).map_err(|e| format!("package meta: {e}"))?;
    if !metadata.is_object() {
        return Err("package meta must be an object".into());
    }
    let target =
        capabilities::Target::parse(metadata["renderer_target"].as_str().unwrap_or("web"))?;
    let report = capabilities::audit(&model, target, Some(profile))?;
    let identity = format!(
        "{} source {} Director {}",
        metadata["game"]
            .as_str()
            .or(metadata["slug"].as_str())
            .unwrap_or("package"),
        metadata["source_sha256"].as_str().unwrap_or("unspecified"),
        metadata["director_version"]
            .as_u64()
            .unwrap_or(profile.version.into())
    );
    capabilities::require(&report, &identity)?;
    // Retain the exact scope and known conversion inventory with the package.
    // A passed static check never implies rendered-journey qualification.
    metadata["capability_preflight"] = report;
    let scenes = scene::scenes(&model, profile)?;
    package::write(
        &compiled,
        &names,
        &scenes,
        profile,
        package::abi_digest(names_txt.as_bytes(), bytecode_header),
        &metadata.to_string(),
    )
}

/// The runtime family a package targets: the port's Director version from
/// the package metadata, and the model's extended-D6 flag.
pub fn package_profile(model: &serde_json::Value, meta: &str) -> Result<scene::Profile, String> {
    let meta: serde_json::Value =
        serde_json::from_str(meta).map_err(|e| format!("package meta: {e}"))?;
    let version = match meta
        .get("director_version")
        .and_then(serde_json::Value::as_u64)
    {
        Some(v @ (500 | 600 | 700 | 800 | 1000)) => v as u16,
        // Director 8.5 movies use the D10 family's tables and services
        // (family.h); the header names the layout the runtime checks.
        Some(850) => 1000,
        Some(v) => return Err(format!("unsupported Director version {v}")),
        None => 600,
    };
    let extended = model
        .get("extendedD6")
        .and_then(serde_json::Value::as_bool)
        .unwrap_or(false);
    let full_engine = model["fullEngine"].as_bool().unwrap_or(false);
    if full_engine && !extended { return Err("full engine requires extended tables".into()) }
    Ok(scene::Profile { version, extended, full_engine })
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
        assert_eq!(
            &bytes[12..28],
            &package::abi_digest(NAMES.as_bytes(), b"opcodes")
        );
        let tags: Vec<&[u8]> = (0..6).map(|i| &bytes[32 + i * 12..36 + i * 12]).collect();
        assert_eq!(
            tags,
            [b"META", b"STRS", b"CODE", b"SYMB", b"GLOB", b"MOVI"].map(|t| &t[..])
        );
        // A different runtime vocabulary is a different ABI.
        let other = build_package(
            &program(),
            &model(""),
            "go\nstartmovie\nzzz\n",
            b"opcodes",
            "{}",
        )
        .unwrap();
        assert_ne!(&bytes[12..28], &other[12..28]);
    }

    #[test]
    fn the_header_names_the_runtime_family() {
        let meta = r#"{"director_version":600}"#;
        let bytes = build_package(
            &program(),
            &model(r#""extendedD6":true,"#),
            NAMES,
            b"",
            meta,
        )
        .unwrap();
        assert_eq!(
            (
                u16::from_le_bytes([bytes[6], bytes[7]]),
                u16::from_le_bytes([bytes[8], bytes[9]])
            ),
            (600, 1)
        );
        let bytes = build_package(
            &program(),
            &model(""),
            NAMES,
            b"",
            r#"{"director_version":850}"#,
        )
        .unwrap();
        assert_eq!(u16::from_le_bytes([bytes[6], bytes[7]]), 1000);
        let error = build_package(
            &program(),
            &model(""),
            NAMES,
            b"",
            r#"{"director_version":900}"#,
        )
        .unwrap_err();
        assert!(error.contains("unsupported Director version"), "{error}");
    }

    #[test]
    fn scene_deltas_follow_the_console_generator() {
        let model: serde_json::Value = serde_json::from_str(&model("")).unwrap();
        let scenes = scene::scenes(
            &model,
            scene::Profile {
                version: 600,
                extended: false,
                full_engine: false,
            },
        )
        .unwrap();
        let delta = &scenes[0].deltas[0];
        assert_eq!(delta.channel, 6);
        assert_eq!(delta.mask & scene::DG_SPAN, scene::DG_SPAN);
        assert_eq!(delta.spec.member, 1 << 21 | 1 << 16 | 2);
        assert_eq!(
            (
                delta.spec.x,
                delta.spec.y,
                delta.spec.width,
                delta.spec.height
            ),
            (20, 10, 4, 2)
        );
        assert_eq!(
            (delta.spec.kind, delta.spec.ink, delta.spec.blend),
            (16, 8, 100)
        );
        assert_eq!(scenes[0].labels, vec![("start".to_string(), 1)]);
        assert_eq!(scenes[0].member_index, vec![(emit::text_hash("Pic"), 0)]);
    }

    #[test]
    fn unsupported_ink_cannot_be_packaged() {
        let source = model("").replace("16,8,0,0", "16,40,0,0");
        let error =
            build_package(&program(), &source, NAMES, b"", r#"{"slug":"fixture"}"#).unwrap_err();
        assert!(error.contains("fixture source unspecified Director 600: renderer web capability unsupported-bitmap-ink 40"), "{error}");
        assert!(
            error.contains("X.DXR frame 1 sprite 1 cast 1 member 2"),
            "{error}"
        );
    }
}

//! `director64-aot <program.json> <output dir> [--names runtime/lingo/names.txt]`
//! `director64-aot iso-inventory <image.iso>` (JSON inventory)
//! `director64-aot iso <image.iso>` (inventory with file digests)
//! `director64-aot lingo-server` (JSON requests on stdin, for host tools)
//! `director64-aot lingo <recovered lingo dir> <program.json>`
//! `director64-aot package <program.json> <model.json> <output.d64p>
//!     [--names runtime/lingo/names.txt] [--bytecode runtime/lingo/lingo_bytecode.h] [--meta meta.json]`
//!
//! The converter's compiler stage (docs/roadmap.md, Track C). It reads the
//! parser's program.json and lowers every handler to the bytecode contract
//! of runtime/lingo/lingo_bytecode.h. The first form writes one C unit,
//! listing and manifest per movie for the console; `package` writes the
//! game package the browser runtime loads (docs/package-format.md).

use std::path::PathBuf;
use std::process::ExitCode;

use director64_aot::{ast, build_package, emit, names};

fn main() -> ExitCode {
    let mut positional = Vec::new();
    let mut names_path = PathBuf::from("runtime/lingo/names.txt");
    let mut bytecode_path = PathBuf::from("runtime/lingo/lingo_bytecode.h");
    let mut meta_path: Option<PathBuf> = None;
    let mut args = std::env::args_os().skip(1);
    while let Some(arg) = args.next() {
        let slot = if arg == "--names" {
            &mut names_path
        } else if arg == "--bytecode" {
            &mut bytecode_path
        } else if arg == "--meta" {
            meta_path = Some(PathBuf::new());
            meta_path.as_mut().unwrap()
        } else {
            positional.push(PathBuf::from(arg));
            continue;
        };
        match args.next() {
            Some(path) => *slot = PathBuf::from(path),
            None => return usage(),
        }
    }
    if positional.first().is_some_and(|p| p.as_os_str() == "director") {
        return match director(&positional[1..]) {
            Ok(summary) => {
                println!("{summary}");
                ExitCode::SUCCESS
            }
            Err(error) => {
                eprintln!("director64-aot: {error}");
                ExitCode::FAILURE
            }
        };
    }
    if positional.first().is_some_and(|p| p.as_os_str() == "lingo-server") {
        return lingo_server();
    }
    let result = if positional.first().is_some_and(|p| p.as_os_str() == "iso-inventory") {
        if positional.len() != 2 {
            return usage();
        }
        iso_inventory(&positional[1])
    } else if positional.first().is_some_and(|p| p.as_os_str() == "zip") {
        if positional.len() != 2 {
            return usage();
        }
        zip(&positional[1])
    } else if positional.first().is_some_and(|p| p.as_os_str() == "iso") {
        if positional.len() != 2 {
            return usage();
        }
        iso(&positional[1])
    } else if positional.first().is_some_and(|p| p.as_os_str() == "lingo") {
        if positional.len() != 3 {
            return usage();
        }
        lingo(&positional[1], &positional[2])
    } else if positional.first().is_some_and(|p| p.as_os_str() == "package") {
        if positional.len() != 4 {
            return usage();
        }
        package(&positional[1], &positional[2], &positional[3], &names_path, &bytecode_path, meta_path.as_ref())
    } else if positional.len() == 2 {
        run(&positional[0], &positional[1], &names_path)
    } else {
        return usage();
    };
    match result {
        Ok(summary) => {
            println!("{summary}");
            ExitCode::SUCCESS
        }
        Err(error) => {
            eprintln!("director64-aot: {error}");
            ExitCode::FAILURE
        }
    }
}

fn usage() -> ExitCode {
    eprintln!(
        "usage: director64-aot <program.json> <output dir> [--names names.txt]\n       \
         director64-aot package <program.json> <model.json> <output.d64p> [--names names.txt] \
         [--bytecode lingo_bytecode.h] [--meta meta.json]"
    );
    ExitCode::from(2)
}

/// The parser for host tools and tests: one JSON request per stdin line
/// ({"movie": [source, name]}, {"expression": text}, {"block": [[line, text], ...]}
/// or {"program": [[file, text], ...]}), one {"ok": ...} or {"error": ...} line back.
fn lingo_server() -> ExitCode {
    use director64_aot::lingo;
    use std::io::{BufRead, Write};
    let stdin = std::io::stdin();
    let mut stdout = std::io::stdout().lock();
    for line in stdin.lock().lines() {
        let Ok(line) = line else { return ExitCode::FAILURE };
        let reply = (|| -> Result<serde_json::Value, String> {
            let request: serde_json::Value = serde_json::from_str(&line).map_err(|e| e.to_string())?;
            let text = |v: &serde_json::Value| v.as_str().map(str::to_string).ok_or("expected text".to_string());
            let pairs = |v: &serde_json::Value| -> Result<Vec<(serde_json::Value, String)>, String> {
                v.as_array()
                    .ok_or("expected pairs")?
                    .iter()
                    .map(|p| Ok((p[0].clone(), text(&p[1])?)))
                    .collect()
            };
            if let Some(movie) = request.get("movie") {
                Ok(serde_json::Value::Array(lingo::parse_movie(&text(&movie[0])?, &text(&movie[1])?)?))
            } else if let Some(expression) = request.get("expression") {
                lingo::expression(&text(expression)?)
            } else if let Some(lines) = request.get("block") {
                let lines: Vec<(usize, String)> = pairs(lines)?
                    .into_iter()
                    .map(|(n, t)| (n.as_u64().unwrap_or(0) as usize, t))
                    .collect();
                Ok(serde_json::Value::Array(lingo::block(&lines)?))
            } else if let Some(files) = request.get("program") {
                let files: Vec<(String, String)> =
                    pairs(files)?.into_iter().map(|(n, t)| (n.as_str().unwrap_or("").to_string(), t)).collect();
                lingo::program(&files)
            } else {
                Err("unknown request".into())
            }
        })();
        let out = match reply {
            Ok(value) => serde_json::json!({"ok": value}),
            Err(error) => serde_json::json!({"error": error}),
        };
        if writeln!(stdout, "{out}").and_then(|_| stdout.flush()).is_err() {
            return ExitCode::FAILURE;
        }
    }
    ExitCode::SUCCESS
}

/// The asset converter's stages over the host file system, relative to the
/// working directory:
///   director plan <media> <policy.json> <dumps>
///   director analyze <media> <dumps> <analysis> <policy.json>
///   director audit <media> <dumps> <source> <policy.json> <parser wasm sha256>
///   director recover <source manifest> <score-recovery>
fn director(args: &[PathBuf]) -> Result<String, String> {
    use director64_aot::convert::{analyze, audit, compile, files::Disk, js, plan, recover};
    let text = |i: usize| -> Result<String, String> {
        args.get(i).and_then(|p| p.to_str()).map(str::to_string).ok_or_else(|| "missing argument".to_string())
    };
    let json_file = |i: usize| -> Result<serde_json::Value, String> {
        serde_json::from_str(&read(&args.get(i).cloned().ok_or("missing argument")?)?).map_err(|e| e.to_string())
    };
    let mut fs = Disk { root: std::env::current_dir().map_err(|e| e.to_string())? };
    match text(0)?.as_str() {
        "plan" => {
            let plan = plan::write_plan(&mut fs, &text(1)?, &json_file(2)?, &text(3)?)?;
            Ok(format!("Planned {} Director parses", plan["dumps"].as_array().map_or(0, Vec::len)))
        }
        "analyze" => {
            let now = std::process::Command::new("date").args(["-u", "+%Y-%m-%dT%H:%M:%S.000Z"]).output()
                .ok().and_then(|o| String::from_utf8(o.stdout).ok()).unwrap_or_default();
            let totals = analyze::analyze(&mut fs, &text(1)?, &text(2)?, &text(3)?, &json_file(4)?, now.trim())?;
            Ok(js::stringify_pretty(&totals))
        }
        "audit" => Ok(js::serialize(&audit::audit(&mut fs, &text(1)?, &text(2)?, &text(3)?, &json_file(4)?, &text(5)?)?)),
        "recover" => Ok(js::stringify_pretty(&recover::recover(&mut fs, &text(1)?, &text(2)?, &recover::provenance())?)),
        // director port-program PROGRAM SOURCE-MANIFEST SLUG SOURCE-SHA [LAUNCHER] [compat]
        "port-program" => {
            let port = director64_aot::ports::Port {
                slug: text(3)?,
                source_sha256: text(4)?,
                launcher_movie: text(5).ok().filter(|m| m != "-"),
                native_compatibility: text(6).is_ok_and(|c| c == "compat"),
            };
            let program = director64_aot::ports::executable_program(&json_file(1)?, &port, &json_file(2)?)?;
            Ok(program.to_string())
        }
        // director port-plan SLUG MODEL
        "port-plan" => Ok(director64_aot::ports::plan(&text(1)?, &json_file(2)?)?.to_string()),
        // director port-model MODEL SLUG SUBSTITUTE-TTF DATA-FILES [KEY=FONT64 ...] [external=RECORDS.json]
        "port-model" => {
            let mut model = json_file(1)?;
            let port = director64_aot::ports::Port { slug: text(2)?, ..Default::default() };
            let measured: Vec<(String, String)> = args[5..]
                .iter()
                .filter_map(|a| a.to_str()?.split_once('=').map(|(k, v)| (k.to_string(), v.to_string())))
                .collect();
            // external=RECORDS.json: the port's converted external media.
            let external_media = match measured.iter().find(|(k, _)| k == "external") {
                Some((_, path)) => serde_json::from_slice::<Vec<serde_json::Value>>(
                    &std::fs::read(path).map_err(|e| format!("{path}: {e}"))?,
                )
                .map_err(|e| format!("{path}: {e}"))?,
                None => Vec::new(),
            };
            let fonts = director64_aot::ports::PortFonts {
                droid_sans: Some(read_bytes(&args[3])?),
                data_files: text(4)?.parse().map_err(|_| "DATA-FILES is a count")?,
                external_media,
            };
            let mut measure = |key: &str| -> Result<Vec<u8>, String> {
                let (_, path) = measured.iter().find(|(k, _)| k == key).ok_or(format!("{key} not measured"))?;
                std::fs::read(path).map_err(|e| format!("{path}: {e}"))
            };
            director64_aot::ports::postprocess_model(&mut model, &port, &fonts, &mut measure)?;
            Ok(model.to_string())
        }
        // director stream-pack OUT.bin FOLDER=DIR...: the browser's streams
        // pack (platforms/web/import/pipeline.mjs streamPack) from disc
        // folders; prints its {name: [offset, length, milliseconds]} index.
        "stream-pack" => {
            let mut files = Vec::new();
            for i in 2..args.len() {
                let spec = text(i)?;
                let (folder, dir) = spec.split_once('=').ok_or("FOLDER=DIR")?;
                let mut names: Vec<_> = std::fs::read_dir(dir)
                    .map_err(|e| format!("{dir}: {e}"))?
                    .filter_map(|e| e.ok())
                    .filter(|e| e.path().is_file())
                    .collect();
                names.sort_by_key(|e| e.file_name());
                for entry in names {
                    let name = entry.file_name().to_string_lossy().to_string();
                    let stem = name.rsplit_once('.').map_or(name.as_str(), |(s, _)| s).to_lowercase();
                    files.push((format!("{folder}/{stem}"), entry.path()));
                }
            }
            files.sort_by(|a, b| a.0.cmp(&b.0));
            let (mut blob, mut index) = (Vec::new(), serde_json::Map::new());
            for (name, path) in files {
                if index.contains_key(&name) {
                    return Err(format!("two sound files are both {name}"));
                }
                let bytes = std::fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))?;
                let (stream, ms) = director64_aot::convert::stream::browser_stream(&bytes)
                    .map_err(|e| format!("{}: {e}", path.display()))?;
                index.insert(name, serde_json::json!([blob.len(), stream.len(), ms]));
                blob.extend(stream);
            }
            std::fs::write(text(1)?, &blob).map_err(|e| e.to_string())?;
            Ok(serde_json::Value::Object(index).to_string())
        }
        // director mov-info FILE: the movie's duration and tracks as JSON.
        "mov-info" => {
            let movie = director64_aot::convert::quicktime::parse(&read_bytes(&args[1])?)?;
            let tracks: Vec<serde_json::Value> = movie
                .tracks
                .iter()
                .map(|t| {
                    serde_json::json!({
                        "kind": format!("{:?}", t.kind).to_lowercase(), "codec": t.codec,
                        "timescale": t.timescale, "duration": t.duration,
                        "width": t.width, "height": t.height, "depth": t.depth,
                        "rate": t.rate, "channels": t.channels, "bits": t.bits,
                        "samples": t.samples.len(),
                        "keyframes": t.samples.iter().filter(|s| s.keyframe).count(),
                    })
                })
                .collect();
            Ok(serde_json::json!({"timescale": movie.timescale, "duration": movie.duration,
                "duration_seconds": movie.duration_seconds, "tracks": tracks}).to_string())
        }
        // director mov-audio FILE OUT.wav: the first sound track as s16le WAV.
        "mov-audio" => match director64_aot::convert::quicktime::audio_wav(&read_bytes(&args[1])?)? {
            Some(wav) => {
                std::fs::write(text(2)?, &wav).map_err(|e| e.to_string())?;
                Ok(format!("{}", wav.len()))
            }
            None => Ok("no sound track".into()),
        },
        // director aiff-wav FILE OUT.wav: an external AIFF as s16le WAV.
        "aiff-wav" => {
            let wav = director64_aot::convert::quicktime::aiff_wav(&read_bytes(&args[1])?)?;
            std::fs::write(text(2)?, &wav).map_err(|e| e.to_string())?;
            Ok(format!("{}", wav.len()))
        }
        // director cinepak-hash FILE [OUT]: the SHA-256 of every RGB24 frame the
        // video track outputs, one per line (ffmpeg -f framehash -hash sha256
        // -pix_fmt rgb24).
        "cinepak-hash" => {
            let bytes = read_bytes(&args[1])?;
            let movie = director64_aot::convert::quicktime::parse(&bytes)?;
            let track = movie.video().ok_or("movie without video")?;
            if track.codec != "cvid" {
                return Err(format!("video codec {:?}", track.codec));
            }
            let mut decoder = director64_aot::convert::cinepak::Decoder::new(track.width as usize, track.height as usize);
            let mut lines = Vec::new();
            let mut dump = Vec::new();
            // ffmpeg drops the samples the edit list ends before.
            for sample in track.samples.iter().filter(|s| track.edit_end.is_none_or(|end| s.time < end)) {
                let start = sample.offset as usize;
                let data = bytes.get(start..start + sample.size as usize).ok_or("sample outside the movie")?;
                if decoder.decode(data) {
                    use sha2::Digest;
                    let digest = sha2::Sha256::digest(decoder.frame());
                    lines.push(digest.iter().map(|b| format!("{b:02x}")).collect::<String>());
                    if args.len() > 2 {
                        dump.extend_from_slice(decoder.frame());
                    }
                }
            }
            // An optional OUT receives the frames themselves, concatenated.
            if args.len() > 2 {
                std::fs::write(text(2)?, &dump).map_err(|e| e.to_string())?;
            }
            Ok(lines.join("\n"))
        }
        // director print-pack OUT.bin MODEL PRINT-DIR: the print documents and
        // their artwork as the browser's print pack; prints its index.
        "print-pack" => {
            let documents = director64_aot::convert::print::documents(&json_file(2)?)?;
            let dir = text(3)?;
            let mut files = vec![("documents.json".to_string(), documents.to_string().into_bytes())];
            let mut pictures: Vec<_> = std::fs::read_dir(&dir)
                .map_err(|e| format!("{dir}: {e}"))?
                .filter_map(|e| e.ok())
                .filter(|e| e.file_name().to_string_lossy().ends_with(".PCT"))
                .collect();
            pictures.sort_by_key(|e| e.file_name());
            for entry in pictures {
                let name = entry.file_name().to_string_lossy().replace(".PCT", ".png");
                let (png, _, _) = director64_aot::convert::print::pict_png(&std::fs::read(entry.path()).map_err(|e| e.to_string())?)
                    .map_err(|e| format!("{name}: {e}"))?;
                files.push((name, png));
            }
            files.sort_by(|a, b| a.0.cmp(&b.0));
            let (mut blob, mut index) = (Vec::new(), serde_json::Map::new());
            for (name, bytes) in files {
                index.insert(name, serde_json::json!([blob.len(), bytes.len()]));
                blob.extend(bytes);
            }
            std::fs::write(text(1)?, &blob).map_err(|e| e.to_string())?;
            Ok(serde_json::Value::Object(index).to_string())
        }
        // director stream FILE...: each file's browser stream length and duration.
        "stream" => {
            let mut lines = Vec::new();
            for i in 1..args.len() {
                let (bytes, ms) = director64_aot::convert::stream::browser_stream(&read_bytes(&args[i])?)
                    .map_err(|e| format!("{}: {e}", text(i).unwrap_or_default()))?;
                lines.push(format!("{} {} {ms}", text(i)?, bytes.len()));
            }
            Ok(lines.join("\n"))
        }
        "glyph" => director64_aot::convert::pfr::glyph_json(&js::unhex(&text(1)?)?),
        "font" => {
            let aliases: Vec<String> = (3..args.len()).filter_map(|i| text(i).ok()).collect();
            Ok(js::stringify_pretty(&director64_aot::convert::font::recover(&mut fs, &text(1)?, &text(2)?, &aliases)?))
        }
        "compile" => {
            let flags: Vec<String> = (6..args.len()).filter_map(|i| text(i).ok()).collect();
            let policy = json_file(4)?;
            let (output, recovery, media, dumps) = (text(1)?, text(2)?, text(3)?, text(5)?);
            let options = compile::Options {
                output: &output,
                recovery: &recovery,
                media: &media,
                dumps: &dumps,
                policy: &policy,
                defer_video: flags.iter().any(|f| f == "--defer-video"),
            };
            let result = compile::compile(&mut fs, &options)?;
            if !result.fatal.is_empty() {
                let shown: Vec<_> = result.fatal.iter().take(25).cloned().collect();
                return Err(js::stringify_pretty(&serde_json::Value::Array(shown)));
            }
            Ok(format!("Model: {} files, {} unresolved resource conversions", result.movies, result.problems.len()))
        }
        other => Err(format!("unknown director stage {other}")),
    }
}

struct FileSource(std::fs::File, u64);
impl director64_aot::iso::Source for FileSource {
    fn size(&self) -> u64 {
        self.1
    }
    fn read_at(&mut self, offset: u64, buffer: &mut [u8]) -> Result<(), String> {
        use std::io::{Read, Seek, SeekFrom};
        self.0.seek(SeekFrom::Start(offset)).map_err(|e| e.to_string())?;
        self.0.read_exact(buffer).map_err(|e| e.to_string())
    }
}

/// The image's inventory as JSON [[path, offset, length], ...].
fn iso_inventory(image: &PathBuf) -> Result<String, String> {
    let handle = std::fs::File::open(image).map_err(|e| format!("{}: {e}", image.display()))?;
    let size = handle.metadata().map_err(|e| e.to_string())?.len();
    let entries = director64_aot::iso::inventory(&mut FileSource(handle, size))?;
    let rows: Vec<serde_json::Value> =
        entries.iter().map(|e| serde_json::json!([e.path, e.offset, e.length])).collect();
    Ok(serde_json::Value::Array(rows).to_string())
}

/// The image's inventory with each file's SHA-256, one JSON line per file.
fn iso(image: &PathBuf) -> Result<String, String> {
    use sha2::{Digest, Sha256};
    let handle = std::fs::File::open(image).map_err(|e| format!("{}: {e}", image.display()))?;
    let size = handle.metadata().map_err(|e| e.to_string())?.len();
    let mut source = FileSource(handle, size);
    let entries = director64_aot::iso::inventory(&mut source)?;
    let mut out = String::new();
    for entry in &entries {
        let data = director64_aot::iso::file(&mut source, entry)?;
        let digest: String = Sha256::digest(&data).iter().map(|b| format!("{b:02x}")).collect();
        out.push_str(&serde_json::json!([entry.path, entry.offset, entry.length, digest]).to_string());
        out.push('\n');
    }
    Ok(out.trim_end().to_string())
}

/// The archive's entries with each file's SHA-256, one JSON line per file.
fn zip(archive: &PathBuf) -> Result<String, String> {
    use sha2::{Digest, Sha256};
    let handle = std::fs::File::open(archive).map_err(|e| format!("{}: {e}", archive.display()))?;
    let size = handle.metadata().map_err(|e| e.to_string())?.len();
    let mut source = FileSource(handle, size);
    let entries = director64_aot::zip::inventory(&mut source)?;
    let mut out = String::new();
    for entry in &entries {
        let data = director64_aot::zip::file(&mut source, entry)?;
        let digest: String = Sha256::digest(&data).iter().map(|b| format!("{b:02x}")).collect();
        out.push_str(&serde_json::json!([entry.path, entry.length, digest]).to_string());
        out.push('\n');
    }
    Ok(out.trim_end().to_string())
}

/// `python -m director64.lingo <directory> <output>`, in Rust.
fn lingo(directory: &PathBuf, output: &PathBuf) -> Result<String, String> {
    let mut names: Vec<String> = std::fs::read_dir(directory)
        .map_err(|e| format!("{}: {e}", directory.display()))?
        .filter_map(|entry| entry.ok()?.file_name().into_string().ok())
        .filter(|name| name.ends_with(".lingo"))
        .collect();
    names.sort();
    let mut files = Vec::new();
    for name in names {
        let raw = read(&directory.join(&name))?;
        files.push((name, director64_aot::lingo::normalize_newlines(&raw)));
    }
    let program = director64_aot::lingo::program(&files)?;
    if let Some(parent) = output.parent() {
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    std::fs::write(output, director64_aot::lingo::dumps_compact(&program) + "\n").map_err(|e| e.to_string())?;
    Ok(format!(
        "Parsed {} files / {} handlers; native execution is a separate gate",
        files.len(),
        program["handlers"].as_array().map_or(0, Vec::len)
    ))
}

fn read(path: &PathBuf) -> Result<String, String> {
    std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))
}

fn run(program: &PathBuf, output: &PathBuf, names_path: &PathBuf) -> Result<String, String> {
    let text = read(program)?;
    let value: serde_json::Value = serde_json::from_str(&text).map_err(|e| format!("{}: {e}", program.display()))?;
    let program = ast::Program::from_json(&value)?;
    let table = names::load(names_path)?;
    let manifest = emit::generate(&program, output, &table)?;
    Ok(format!(
        "Generated {} bytecode movie units / {} bytes / {} globals",
        manifest.movies, manifest.bytecode_bytes, manifest.globals
    ))
}

fn package(
    program: &PathBuf,
    model: &PathBuf,
    output: &PathBuf,
    names_path: &PathBuf,
    bytecode_path: &PathBuf,
    meta_path: Option<&PathBuf>,
) -> Result<String, String> {
    let bytecode = std::fs::read(bytecode_path).map_err(|e| format!("{}: {e}", bytecode_path.display()))?;
    let meta = match meta_path {
        Some(path) => read(path)?,
        None => "{}".to_string(),
    };
    let bytes = build_package(&read(program)?, &read(model)?, &read(names_path)?, &bytecode, &meta)?;
    std::fs::write(output, &bytes).map_err(|e| format!("{}: {e}", output.display()))?;
    Ok(format!("Packaged {} bytes into {}", bytes.len(), output.display()))
}

fn read_bytes(path: &PathBuf) -> Result<Vec<u8>, String> {
    std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))
}

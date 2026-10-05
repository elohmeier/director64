//! The converter's Rust stages for the browser importer (platforms/web/
//! import/): ISO inventory over the page's File, streaming SHA-256, the
//! Director converter stages over the importer's files, the Lingo parser
//! and the game packager. A plain C ABI over linear memory:
//! the caller allocates inputs with `d64c_alloc`, a call returns 0 or 1 and
//! leaves its output (or error text) in the result buffer.

use director64_aot::convert::files::Files;
use director64_aot::iso::{self, Source};
use sha2::{Digest, Sha256};

static mut RESULT: Vec<u8> = Vec::new();
static mut HASHES: Vec<Option<Sha256>> = Vec::new();
/// The compile stage between its two calls ("compile" with
/// external_prescale, then "compile-finish").
static mut PENDING: Option<director64_aot::convert::compile::Pending> = None;
/// A pool worker's external-palette cache across its "compile-movie" calls.
static mut PALETTES: Option<std::collections::HashMap<String, Vec<u8>>> = None;

#[allow(static_mut_refs)]
fn set_result(bytes: Vec<u8>) {
    unsafe { RESULT = bytes }
}
fn finish(result: Result<Vec<u8>, String>) -> u32 {
    match result {
        Ok(bytes) => {
            set_result(bytes);
            0
        }
        Err(error) => {
            set_result(error.into_bytes());
            1
        }
    }
}
unsafe fn input<'a>(ptr: *const u8, len: usize) -> &'a [u8] {
    if len == 0 { &[] } else { unsafe { std::slice::from_raw_parts(ptr, len) } }
}
unsafe fn text<'a>(ptr: *const u8, len: usize) -> Result<&'a str, String> {
    std::str::from_utf8(unsafe { input(ptr, len) }).map_err(|_| "input is not UTF-8".to_string())
}

#[unsafe(no_mangle)]
pub extern "C" fn d64c_alloc(len: usize) -> *mut u8 {
    let mut buffer = Vec::<u8>::with_capacity(len.max(1));
    let ptr = buffer.as_mut_ptr();
    std::mem::forget(buffer);
    ptr
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn d64c_free(ptr: *mut u8, len: usize) {
    drop(unsafe { Vec::from_raw_parts(ptr, 0, len.max(1)) });
}
#[unsafe(no_mangle)]
#[allow(static_mut_refs)]
pub extern "C" fn d64c_result_ptr() -> *const u8 {
    unsafe { RESULT.as_ptr() }
}
#[unsafe(no_mangle)]
#[allow(static_mut_refs)]
pub extern "C" fn d64c_result_len() -> usize {
    unsafe { RESULT.len() }
}

// ---- SHA-256, streamed ----

#[unsafe(no_mangle)]
#[allow(static_mut_refs)]
pub extern "C" fn d64c_sha256_new() -> u32 {
    unsafe {
        HASHES.push(Some(Sha256::new()));
        HASHES.len() as u32 - 1
    }
}
#[unsafe(no_mangle)]
#[allow(static_mut_refs)]
pub unsafe extern "C" fn d64c_sha256_update(handle: u32, ptr: *const u8, len: usize) {
    let data = unsafe { input(ptr, len) };
    if let Some(Some(hash)) = unsafe { HASHES.get_mut(handle as usize) } {
        hash.update(data);
    }
}
/// Leaves the lower-case hex digest in the result buffer.
#[unsafe(no_mangle)]
#[allow(static_mut_refs)]
pub extern "C" fn d64c_sha256_finish(handle: u32) -> u32 {
    let hash = unsafe { HASHES.get_mut(handle as usize).and_then(Option::take) };
    finish(hash.ok_or_else(|| "unknown hash".to_string()).map(|h| {
        h.finalize().iter().map(|b| format!("{b:02x}")).collect::<String>().into_bytes()
    }))
}

// ---- ISO inventory over the embedder's synchronous reads ----

#[link(wasm_import_module = "env")]
unsafe extern "C" {
    /// Fills `len` bytes at `ptr` from image offset `offset`; 0 on success.
    fn d64c_read_at(offset: f64, ptr: *mut u8, len: usize) -> u32;
}
struct Embedder(u64);
impl Source for Embedder {
    fn size(&self) -> u64 {
        self.0
    }
    fn read_at(&mut self, offset: u64, buffer: &mut [u8]) -> Result<(), String> {
        match unsafe { d64c_read_at(offset as f64, buffer.as_mut_ptr(), buffer.len()) } {
            0 => Ok(()),
            _ => Err("read failed".into()),
        }
    }
}
/// The inventory as JSON [[path, offset, length], ...].
#[unsafe(no_mangle)]
pub extern "C" fn d64c_iso_inventory(size: f64) -> u32 {
    if !(0.0..=9.0e15).contains(&size) {
        return finish(Err("invalid image size".into()));
    }
    finish(iso::inventory(&mut Embedder(size as u64)).map(|entries| {
        let rows: Vec<serde_json::Value> =
            entries.iter().map(|e| serde_json::json!([e.path, e.offset, e.length])).collect();
        serde_json::Value::Array(rows).to_string().into_bytes()
    }))
}

// ---- ZIP game sources over the same reads ----

static mut ZIP_ENTRIES: Vec<director64_aot::zip::Entry> = Vec::new();

/// The archive's files as JSON [[path, length], ...]; the entries stay for
/// d64c_zip_file.
#[unsafe(no_mangle)]
#[allow(static_mut_refs)]
pub extern "C" fn d64c_zip_inventory(size: f64) -> u32 {
    if !(0.0..=9.0e15).contains(&size) {
        return finish(Err("invalid archive size".into()));
    }
    finish(director64_aot::zip::inventory(&mut Embedder(size as u64)).map(|entries| {
        let rows: Vec<serde_json::Value> = entries.iter().map(|e| serde_json::json!([e.path, e.length])).collect();
        unsafe { ZIP_ENTRIES = entries };
        serde_json::Value::Array(rows).to_string().into_bytes()
    }))
}
/// One inventoried file's bytes, inflated and CRC-checked.
#[unsafe(no_mangle)]
#[allow(static_mut_refs)]
pub extern "C" fn d64c_zip_file(size: f64, index: u32) -> u32 {
    let entry = unsafe { ZIP_ENTRIES.get(index as usize).cloned() };
    finish(match entry {
        Some(entry) => director64_aot::zip::file(&mut Embedder(size as u64), &entry),
        None => Err("unknown ZIP entry".into()),
    })
}

// ---- Deflated files ----

/// A deflated file's bytes (files::inflate); any other file as it is.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn d64c_inflate(ptr: *const u8, len: usize) -> u32 {
    finish(director64_aot::convert::files::inflate(unsafe { input(ptr, len) }))
}

/// One stored image of the compile stage's prescale queue at 4/5, for the
/// importer's worker pool: empty when the image keeps its size, else the
/// new name's length (one byte), the name, and the image as stored
/// (deflated, as files::DeflatingImages writes it).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn d64c_prescale(ptr: *const u8, len: usize) -> u32 {
    use director64_aot::convert::{compile, files};
    finish((|| {
        let plain = files::inflate(unsafe { input(ptr, len) })?;
        Ok(match compile::prescale_asset(&plain)? {
            None => Vec::new(),
            Some((name, scaled)) => {
                let mut out = vec![name.len() as u8];
                out.extend_from_slice(name.as_bytes());
                out.extend_from_slice(&files::deflate(&scaled));
                out
            }
        })
    })())
}

// ---- External sound streams ----

/// A disc sound file as the browser player streams it: the result is its
/// duration in milliseconds (u32, little-endian), then the stream's bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn d64c_stream(ptr: *const u8, len: usize) -> u32 {
    finish(director64_aot::convert::stream::browser_stream(unsafe { input(ptr, len) }).map(|(bytes, ms)| {
        let mut out = ms.to_le_bytes().to_vec();
        out.extend(bytes);
        out
    }))
}

// ---- Linked movies, external sounds and print documents (Löwenzahn) ----

/// A QuickTime movie's duration and video size: {duration_seconds, video,
/// width, height}.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn d64c_mov_info(ptr: *const u8, len: usize) -> u32 {
    finish((|| {
        let movie = director64_aot::convert::quicktime::parse(unsafe { input(ptr, len) })?;
        let video = movie.video();
        Ok(serde_json::json!({"duration_seconds": movie.duration_seconds, "video": video.is_some(),
            "width": video.map_or(0, |v| v.width), "height": video.map_or(0, |v| v.height)})
            .to_string()
            .into_bytes())
    })())
}
/// A movie's sound track as s16le WAV; empty without one.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn d64c_mov_audio(ptr: *const u8, len: usize) -> u32 {
    finish(director64_aot::convert::quicktime::audio_wav(unsafe { input(ptr, len) }).map(Option::unwrap_or_default))
}
/// An external AIFF as s16le WAV.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn d64c_aiff_wav(ptr: *const u8, len: usize) -> u32 {
    finish(director64_aot::convert::quicktime::aiff_wav(unsafe { input(ptr, len) }))
}

// One movie's Cinepak frames at a time: its bytes stay with the iterator.
static mut FRAMES: Option<(*mut [u8], director64_aot::convert::cinepak::Frames<'static>)> = None;
/// Opens a movie's frames; the result is {width, height}.
#[unsafe(no_mangle)]
#[allow(static_mut_refs)]
pub unsafe extern "C" fn d64c_frames_open(ptr: *const u8, len: usize) -> u32 {
    unsafe { d64c_frames_close() };
    let bytes: &'static mut [u8] = Box::leak(unsafe { input(ptr, len) }.to_vec().into_boxed_slice());
    let raw: *mut [u8] = bytes;
    match director64_aot::convert::cinepak::Frames::new(unsafe { &*raw }) {
        Ok(frames) => {
            let size = serde_json::json!({"width": frames.width(), "height": frames.height()});
            unsafe { FRAMES = Some((raw, frames)) };
            finish(Ok(size.to_string().into_bytes()))
        }
        Err(error) => {
            drop(unsafe { Box::from_raw(raw) });
            finish(Err(error))
        }
    }
}
/// The next frame: its time in microseconds (u64, little-endian) and RGBX
/// pixels; empty after the last.
#[unsafe(no_mangle)]
#[allow(static_mut_refs)]
pub extern "C" fn d64c_frames_next() -> u32 {
    let frames = unsafe { FRAMES.as_mut() };
    finish(match frames {
        None => Err("no movie open".into()),
        Some((_, frames)) => frames.next_frame().map(|frame| match frame {
            None => Vec::new(),
            Some((time, pixels)) => {
                let mut out = time.to_le_bytes().to_vec();
                out.extend(pixels);
                out
            }
        }),
    })
}
#[unsafe(no_mangle)]
#[allow(static_mut_refs)]
pub unsafe extern "C" fn d64c_frames_close() {
    if let Some((raw, frames)) = unsafe { FRAMES.take() } {
        drop(frames);
        drop(unsafe { Box::from_raw(raw) });
    }
}
/// A print PICT as PNG.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn d64c_pict_png(ptr: *const u8, len: usize) -> u32 {
    finish(director64_aot::convert::print::pict_png(unsafe { input(ptr, len) }).map(|(png, _, _)| png))
}
/// The print documents of a model (model.json in, documents JSON out).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn d64c_print_documents(ptr: *const u8, len: usize) -> u32 {
    finish((|| {
        let model: serde_json::Value =
            serde_json::from_slice(unsafe { input(ptr, len) }).map_err(|e| format!("model: {e}"))?;
        Ok(director64_aot::convert::print::documents(&model)?.to_string().into_bytes())
    })())
}

// ---- Sounds for the browser's Opus encoder ----

/// A converted PCM WAV at 48 kHz (convert/resample.rs): channels, frames,
/// source rate and source frames (u32 each, little-endian), then the planar
/// f32 samples.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn d64c_resample48(ptr: *const u8, len: usize) -> u32 {
    finish(director64_aot::convert::resample::wav_to_48k(unsafe { input(ptr, len) }).map(|r| {
        let mut out = Vec::with_capacity(16 + r.samples.len() * 4);
        for v in [r.channels as u32, r.frames as u32, r.source_rate, r.source_frames as u32] {
            out.extend_from_slice(&v.to_le_bytes());
        }
        for sample in &r.samples {
            out.extend_from_slice(&sample.to_le_bytes());
        }
        out
    }))
}

// ---- Lingo and packaging ----

/// Input: JSON [[file name, text], ...] sorted by name, texts with
/// universal newlines. Output: program.json exactly as the Python stage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn d64c_lingo_program(ptr: *const u8, len: usize) -> u32 {
    finish((|| {
        let files: Vec<(String, String)> =
            serde_json::from_str(unsafe { text(ptr, len) }?).map_err(|e| format!("lingo input: {e}"))?;
        let program = director64_aot::lingo::program(&files)?;
        Ok((director64_aot::lingo::dumps_compact(&program) + "\n").into_bytes())
    })())
}

#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn d64c_package(
    program: *const u8,
    program_len: usize,
    model: *const u8,
    model_len: usize,
    names: *const u8,
    names_len: usize,
    bytecode: *const u8,
    bytecode_len: usize,
    meta: *const u8,
    meta_len: usize,
) -> u32 {
    finish((|| unsafe {
        director64_aot::build_package(
            text(program, program_len)?,
            text(model, model_len)?,
            text(names, names_len)?,
            input(bytecode, bytecode_len),
            text(meta, meta_len)?,
        )
    })())
}

// ---- The Director converter stages over the embedder's files ----

#[link(wasm_import_module = "env")]
unsafe extern "C" {
    /// One operation on the embedder's file system (see `Op`): a status,
    /// a size, or the length of a reply staged for `d64c_fs_take`; negative
    /// when the path does not exist or the operation failed.
    fn d64c_fs(op: u32, path: *const u8, path_len: usize, data: *const u8, data_len: usize) -> f64;
    /// Copies the reply the last `d64c_fs` call staged to `ptr`.
    fn d64c_fs_take(ptr: *mut u8);
}

#[derive(Clone, Copy)]
enum Op {
    Read = 0,
    Write = 1,
    Exists = 2,
    IsFile = 3,
    List = 4,
    Mkdir = 5,
    Remove = 6,
    Size = 7,
}

/// The importer's in-memory file system, reached through the host.
struct HostFiles;

impl HostFiles {
    fn call(op: Op, path: &str, data: &[u8]) -> f64 {
        unsafe { d64c_fs(op as u32, path.as_ptr(), path.len(), data.as_ptr(), data.len()) }
    }
    fn fetch(op: Op, path: &str) -> Result<Vec<u8>, String> {
        let length = Self::call(op, path, &[]);
        if length < 0.0 {
            return Err(format!("{path}: no such file or directory"));
        }
        let mut out = vec![0u8; length as usize];
        unsafe { d64c_fs_take(out.as_mut_ptr()) };
        Ok(out)
    }
    fn status(op: Op, path: &str, data: &[u8]) -> Result<(), String> {
        if Self::call(op, path, data) < 0.0 { Err(format!("{path}: operation failed")) } else { Ok(()) }
    }
}

impl Files for HostFiles {
    fn read(&self, path: &str) -> Result<Vec<u8>, String> {
        Self::fetch(Op::Read, path)
    }
    fn write(&mut self, path: &str, data: &[u8]) -> Result<(), String> {
        Self::status(Op::Write, path, data)
    }
    fn exists(&self, path: &str) -> bool {
        Self::call(Op::Exists, path, &[]) > 0.0
    }
    fn is_file(&self, path: &str) -> bool {
        Self::call(Op::IsFile, path, &[]) > 0.0
    }
    fn list(&self, path: &str) -> Result<Vec<(String, bool)>, String> {
        let reply = Self::fetch(Op::List, path)?;
        serde_json::from_slice(&reply).map_err(|e| format!("{path}: {e}"))
    }
    fn mkdir_all(&mut self, path: &str) -> Result<(), String> {
        Self::status(Op::Mkdir, path, &[])
    }
    fn remove(&mut self, path: &str) -> Result<(), String> {
        Self::status(Op::Remove, path, &[])
    }
    fn size(&self, path: &str) -> Result<u64, String> {
        let size = Self::call(Op::Size, path, &[]);
        if size < 0.0 { Err(format!("{path}: no such file or directory")) } else { Ok(size as u64) }
    }
}

fn compile_options<'a>(
    request: &'a serde_json::Value,
    policy: &'a serde_json::Value,
) -> Result<director64_aot::convert::compile::Options<'a>, String> {
    let arg = |key: &str| request[key].as_str().ok_or_else(|| format!("director request without {key}"));
    Ok(director64_aot::convert::compile::Options {
        output: arg("output")?,
        recovery: arg("recovery")?,
        media: arg("media")?,
        dumps: arg("dumps")?,
        policy,
        defer_video: request["defer_video"].as_bool().unwrap_or(false),
    })
}

/// One converter stage, as `director64-aot director <stage>` runs it on the
/// host. Input: JSON {"stage": ..., stage arguments}; output: the stage's
/// JSON result.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn d64c_director(ptr: *const u8, len: usize) -> u32 {
    use director64_aot::convert::{analyze, audit, compile, plan, recover};
    use serde_json::{Value, json};
    finish((|| {
        let request: Value =
            serde_json::from_str(unsafe { text(ptr, len) }?).map_err(|e| format!("director request: {e}"))?;
        let arg = |key: &str| request[key].as_str().ok_or_else(|| format!("director request without {key}"));
        let mut host = HostFiles;
        // Converted images are kept deflated (files::DeflatingImages): the
        // stages read them back plain, the importer packs them as stored.
        let mut deflating = director64_aot::convert::files::DeflatingImages(&mut host);
        let fs: &mut dyn Files = &mut deflating;
        let policy = &request["policy"];
        let result = match arg("stage")? {
            "plan" => plan::write_plan(fs, arg("media")?, policy, arg("dumps")?)?,
            "analyze" => analyze::analyze(fs, arg("media")?, arg("dumps")?, arg("output")?, policy, arg("now")?)?,
            "audit" => audit::audit(fs, arg("media")?, arg("dumps")?, arg("output")?, policy, arg("parser")?)?,
            "recover" => recover::recover(fs, arg("manifest")?, arg("output")?, &recover::provenance())?,
            "compile" => {
                let options = compile_options(&request, policy)?;
                // With external_movies the importer converts each movie on its
                // worker pool ("compile-movie"), merges them ("compile-merge")
                // and resamples the prescale queue there too (d64c_prescale),
                // finishing with "compile-finish". external_prescale alone
                // converts the movies here.
                if request["external_movies"].as_bool().unwrap_or(false) {
                    let setup = compile::compile_setup(fs, &options)?;
                    let inputs: Vec<Vec<String>> = (0..setup.jobs.len()).map(|k| setup.job_inputs(&options, k)).collect();
                    json!({"movies": setup.jobs.len(), "names": setup.job_names(), "inputs": inputs})
                } else if request["external_prescale"].as_bool().unwrap_or(false) {
                    let pending = compile::compile_start(fs, &options)?;
                    let queue = json!({"prescale": pending.prescale});
                    unsafe { PENDING = Some(pending) };
                    queue
                } else {
                    let compiled = compile::compile(fs, &options)?;
                    json!({"movies": compiled.movies, "problems": compiled.problems, "fatal": compiled.fatal})
                }
            }
            // One movie, on a pool worker: its converted record.
            "compile-movie" => {
                let options = compile_options(&request, policy)?;
                let setup = compile::compile_setup(fs, &options)?;
                #[allow(static_mut_refs)]
                let palettes = unsafe { PALETTES.get_or_insert_with(Default::default) };
                let index = request["index"].as_u64().ok_or("compile-movie without an index")? as usize;
                compile::convert_job(fs, &options, &setup, palettes, index)?.to_json()
            }
            // The pool's movie records, in job order, from `results`.
            "compile-merge" => {
                let options = compile_options(&request, policy)?;
                let results = arg("results")?;
                let count = request["movies"].as_u64().ok_or("compile-merge without a movie count")? as usize;
                let mut converted = Vec::with_capacity(count);
                for k in 0..count {
                    let path = format!("{results}/{k}.json");
                    let value: Value = serde_json::from_slice(&fs.read(&path)?).map_err(|e| format!("{path}: {e}"))?;
                    converted.push(director64_aot::convert::movie::Converted::from_json(value)?);
                }
                let pending = compile::compile_merge(fs, &options, converted)?;
                let queue = json!({"prescale": pending.prescale});
                unsafe { PENDING = Some(pending) };
                queue
            }
            "compile-finish" => {
                let options = compile_options(&request, policy)?;
                #[allow(static_mut_refs)]
                let pending = unsafe { PENDING.take() }.ok_or("compile-finish without a pending compile")?;
                let renames: std::collections::HashMap<String, String> = request["renames"]
                    .as_object()
                    .into_iter()
                    .flatten()
                    .map(|(k, v)| (k.clone(), v.as_str().unwrap_or("").to_string()))
                    .collect();
                let compiled = compile::compile_finish(fs, &options, pending, &renames)?;
                json!({"movies": compiled.movies, "problems": compiled.problems, "fatal": compiled.fatal})
            }
            // The port's additions (compiler/src/ports.rs) over the importer's files.
            "port-plan" => {
                let path = arg("model")?;
                let model: Value = serde_json::from_slice(&fs.read(path)?).map_err(|e| format!("{path}: {e}"))?;
                director64_aot::ports::plan(arg("slug")?, &model)?
            }
            "port-model" => {
                let path = arg("model")?;
                let mut model: Value = serde_json::from_slice(&fs.read(path)?).map_err(|e| format!("{path}: {e}"))?;
                let port = port_of(&request)?;
                let substitute = request.get("substitute").and_then(Value::as_str).map(|p| fs.read(p)).transpose()?;
                // [[key, font64 path], ...], one per plan() measurement.
                let measured: Vec<(String, String)> = request["measured"]
                    .as_array()
                    .into_iter()
                    .flatten()
                    .map(|m| (m[0].as_str().unwrap_or("").to_string(), m[1].as_str().unwrap_or("").to_string()))
                    .collect();
                let mut measure = |key: &str| -> Result<Vec<u8>, String> {
                    let (_, path) = measured.iter().find(|(k, _)| k == key).ok_or(format!("{key} not measured"))?;
                    fs.read(path)
                };
                let fonts = director64_aot::ports::PortFonts {
                    droid_sans: substitute.clone(),
                    data_files: request["data_files"].as_u64().unwrap_or(0) as usize,
                    external_media: request["external_media"].as_array().cloned().unwrap_or_default(),
                };
                director64_aot::ports::postprocess_model(&mut model, &port, &fonts, &mut measure)?;
                // The model names its substitute as a converted font asset.
                if let (Some(asset), Some(bytes)) = (director64_aot::ports::substitute_asset(&port.slug), substitute) {
                    let output = arg("output")?;
                    fs.write(&format!("{output}/{asset}"), &bytes)?;
                }
                fs.write(path, (model.to_string() + "\n").as_bytes())?;
                json!({"fonts": model["fonts"].as_array().map_or(0, Vec::len)})
            }
            "port-program" => {
                let program: Value = serde_json::from_slice(&fs.read(arg("program")?)?).map_err(|e| e.to_string())?;
                let manifest: Value = serde_json::from_slice(&fs.read(arg("manifest")?)?).map_err(|e| e.to_string())?;
                let program = director64_aot::ports::executable_program(&program, &port_of(&request)?, &manifest)?;
                fs.write(arg("output")?, (program.to_string() + "\n").as_bytes())?;
                json!({"handlers": program["handlers"].as_array().map_or(0, Vec::len)})
            }
            other => return Err(format!("unknown director stage {other}")),
        };
        Ok(result.to_string().into_bytes())
    })())
}

/// A port's settings from a director request's "port" object.
fn port_of(request: &serde_json::Value) -> Result<director64_aot::ports::Port, String> {
    let port = &request["port"];
    Ok(director64_aot::ports::Port {
        slug: port["slug"].as_str().ok_or("director request without a port slug")?.to_string(),
        source_sha256: port["source_sha256"].as_str().unwrap_or("").to_string(),
        launcher_movie: port["launcher_movie"].as_str().map(str::to_string),
        native_compatibility: port["native_compatibility"].as_bool().unwrap_or(false),
    })
}

// From a local disc image to a playable game package, entirely in the
// browser (docs/web-roadmap.md, W4): verify the edition, extract the disc,
// parse the Director files, recover scripts and scores, convert the cast,
// parse the Lingo and build the package and asset packs. Each stage is the
// native pipeline's own code (ProjectorRays through tools/director/
// dump-format.mjs, then the Rust converter) run over the in-memory file
// system; nothing leaves the machine.
import {Vfs} from "./vfs.mjs";
import {dumpDirector} from "../../../tools/director/dump-format.mjs";
import {fontTools} from "./fonts.mjs";

const STAGES = ["verify", "extract", "parse", "analyze", "audit", "scores", "convert", "port", "compile", "package", "sound", "video"];
const decoder = new TextDecoder();

export class ImportError extends Error {}

function check(signal) {
  if (signal?.aborted) throw Object.assign(new Error("import cancelled"), {name: "AbortError"});
}

// Streams the whole image through SHA-256 in bounded pieces.
export function imageSha256(source, converter, onProgress, signal) {
  const hash = converter.sha256();
  const piece = 8 << 20;
  for (let at = 0; at < source.size; at += piece) {
    check(signal);
    hash.update(source.readAt(at, Math.min(piece, source.size - at)));
    onProgress?.(Math.min(1, (at + piece) / source.size));
  }
  return hash.hex();
}

// The profile whose pinned edition this image is, verified by its full
// digest; a size match only nominates candidates.
export function identify(source, profiles, converter, onProgress, signal) {
  const candidates = profiles.filter((p) => p.source.bytes === source.size);
  if (!candidates.length)
    throw new ImportError("This disc image is not a supported edition (no edition has its size).");
  const digest = imageSha256(source, converter, onProgress, signal);
  const profile = candidates.find((p) => p.source.sha256 === digest);
  if (!profile)
    throw new ImportError(`This disc image is not a supported edition (SHA-256 ${digest}).`);
  return {profile, digest};
}

function pack(vfs, directory, rename = (name) => name) {
  const names = vfs.exists(directory) ? vfs.list(directory).filter(([, kind]) => kind === "file") : [];
  return packFiles(names.map(([name]) => [rename(name), vfs.read(`${directory}/${name}`)]));
}

// Every file below a directory, as paths relative to it; none if it is absent.
function filesUnder(vfs, directory) {
  if (!vfs.exists(directory)) return [];
  return vfs.list(directory).flatMap(([name, kind]) => kind === "dir"
    ? filesUnder(vfs, `${directory}/${name}`).map((inner) => `${name}/${inner}`) : [name]);
}

// The folders' sound files as the browser streams them (converter.stream),
// into one blob with a name -> [offset, length, milliseconds] index. A
// stream is never longer than its source, so the blob is sized once and
// each disc file is read, converted and copied in a single pass.
function streamPack(vfs, folders, converter) {
  const files = folders.flatMap((folder) => filesUnder(vfs, `/work/extracted/${folder}`)
    .filter((name) => !name.includes("/"))
    .map((name) => [`${folder}/${name.replace(/\.[^.]*$/, "").toLowerCase()}`, `/work/extracted/${folder}/${name}`]));
  files.sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0));
  const blob = new Uint8Array(files.reduce((n, [, path]) => n + vfs.size(path), 0)), index = {};
  let at = 0;
  for (const [name, path] of files) {
    if (name in index) throw new ImportError(`two sound files are both ${name}`);
    const {bytes, ms} = converter.stream(vfs.read(path));
    blob.set(bytes, at);
    index[name] = [at, bytes.length, ms];
    at += bytes.length;
  }
  return {index, blob: blob.subarray(0, at)};
}

// Named files concatenated into one blob with a name -> [offset, length] index.
function packFiles(entries) {
  entries.sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0));
  const total = entries.reduce((n, [, data]) => n + data.length, 0);
  const blob = new Uint8Array(total), index = {};
  let at = 0;
  for (const [name, data] of entries) {
    blob.set(data, at);
    index[name] = [at, data.length];
    at += data.length;
  }
  return {index, blob};
}

/**
 * source: {size, readAt(offset, length) -> Uint8Array} (synchronous)
 * profile: a supported game's profile (profiles.json)
 * tools: {converter, DirectorFile, parserWasm, names, bytecodeHeader, meta,
 *         fonts: {createMkfont, createMksprite, mkfontWasm, mkspriteWasm}, substituteFont}
 */
export async function importGame({source, profile, tools, onProgress = () => {}, signal, verified = false}) {
  const vfs = new Vfs();
  const sha256 = (bytes) => {
    const hash = tools.converter.sha256();
    hash.update(bytes);
    return hash.hex();
  };
  const director = (stage, request) => tools.converter.director({stage, ...request}, vfs);
  const report = {stages: {}, profile: profile.slug, source: profile.source.id};
  const stage = async (name, work) => {
    check(signal);
    onProgress({stage: name, index: STAGES.indexOf(name), stages: STAGES.length, fraction: 0});
    const started = performance.now();
    const result = await work((fraction) =>
      onProgress({stage: name, index: STAGES.indexOf(name), stages: STAGES.length, fraction}));
    report.stages[name] = Math.round(performance.now() - started);
    return result;
  };

  await stage("verify", async (progress) => {
    if (verified) return;
    if (source.size !== profile.source.bytes ||
        imageSha256(source, tools.converter, progress, signal) !== profile.source.sha256)
      throw new ImportError("This disc image is not the edition this game profile supports.");
  });

  await stage("extract", async (progress) => {
    const read = (offset, length) => source.readAt(offset, length);
    // An edition is catalogued as a disc image or as a ZIP of the disc's files.
    const zip = profile.source.kind === "zip";
    const entries = zip ? tools.converter.zipInventory(source.size, read)
      : tools.converter.isoInventory(source.size, read);
    vfs.mkdir("/work/extracted", true);
    let done = 0;
    for (const entry of entries) {
      check(signal);
      const target = `/work/extracted/${entry.path}`;
      vfs.mkdir(target.slice(0, target.lastIndexOf("/")), true);
      // Disc files are read from the image when a stage asks. A ZIP entry is
      // inflated by the converter, which cannot run while one of its own
      // stages reads a file, so the files stages read (Director files and
      // projectors) are inflated now; only the rest waits.
      if (!zip)
        vfs.writeLazy(target, entry.length, () => source.readAt(entry.offset, entry.length));
      else if (/\.(dxr|cxt|cst|dir|exe)$/i.test(entry.path))
        vfs.write(target, tools.converter.zipFile(source.size, entry.index, read));
      else
        vfs.writeLazy(target, entry.length, () => tools.converter.zipFile(source.size, entry.index, read));
      progress(++done / entries.length);
    }
    report.files = entries.length;
  });

  const media = `/work/extracted/${profile.port.media_root}`;
  const policy = profile.policy;
  const dumps = "/work/analysis/dumps";
  // ProjectorRays parses each Director file once; every later stage reads
  // what it saw.
  await stage("parse", async (progress) => {
    const plan = director("plan", {media, policy, dumps});
    let done = 0;
    for (const entry of plan.dumps) {
      check(signal);
      const file = await tools.DirectorFile.read(vfs.read(entry.input));
      try {
        vfs.write(entry.output, dumpDirector(file));
      } finally {
        file.destroy();
      }
      progress(++done / plan.dumps.length);
    }
  });
  await stage("analyze", async () =>
    director("analyze", {media, dumps, output: "/work/analysis", policy, now: new Date().toISOString()}));
  await stage("audit", async () =>
    director("audit", {media, dumps, output: "/work/analysis/source", policy, parser: sha256(tools.parserWasm)}));
  await stage("scores", async () =>
    director("recover", {manifest: "/work/analysis/source/manifest.json", output: "/work/analysis/score-recovery"}));
  const converted = await stage("convert", async () => director("compile", {output: "/work/director",
    recovery: "/work/analysis/score-recovery", media, policy, dumps,
    defer_video: !!profile.port.asset_postprocessor}));
  if (converted.fatal.length)
    throw new ImportError(`${converted.fatal.length} cast members could not be converted, ` +
      `first: ${JSON.stringify(converted.fatal[0])}`);

  // The port's own additions (compiler/src/ports.rs): model corrections,
  // with its substitute font measured by the same mkfont the console uses.
  const port = {slug: profile.slug, source_sha256: profile.source.sha256,
    launcher_movie: profile.port.launcher_movie ?? null,
    native_compatibility: !!profile.port.native_compatibility};
  // Linked movies and external sounds (Löwenzahn), as the native pipeline's
  // host/full_media.py converts them with the same Rust code: each source's
  // record, its sound as a WAV named by content, and the movies whose video
  // the video stage encodes. The console's own video is named by its source,
  // so the browser's video of the same movie carries the same asset name.
  const videoSources = [];
  function convertExternalMedia({root, extensions}, progress) {
    const base = `/work/extracted/${root}`;
    const sources = filesUnder(vfs, base).filter((path) => extensions.some((e) => path.endsWith(`.${e}`)));
    const records = [];
    vfs.mkdir("/work/director/wav", true);
    for (const [i, path] of sources.entries()) {
      check(signal);
      const bytes = vfs.read(`${base}/${path}`);
      const source = sha256(bytes);
      const record = {source: path, source_sha256: source, source_bytes: bytes.length, video: "", audio: ""};
      let wav;
      if (path.endsWith(".MOV")) {
        const info = tools.converter.movInfo(bytes);
        record.duration = info.duration_seconds;
        if (info.video) {
          record.video = `${source.slice(0, 24)}.h264`;
          videoSources.push({path: `${base}/${path}`, name: record.video});
        }
        wav = tools.converter.movAudio(bytes);
      } else {
        wav = tools.converter.aiffWav(bytes);
      }
      if (wav) {
        const name = sha256(wav).slice(0, 24);
        vfs.write(`/work/director/wav/${name}.wav`, wav);
        const view = new DataView(wav.buffer, wav.byteOffset, wav.byteLength);
        record.channels = view.getUint16(22, true);
        record.rate = view.getUint32(24, true);
        record.frames = view.getUint32(40, true) / (2 * record.channels);
        if (path.endsWith(".AIF")) record.duration = record.frames / record.rate;
        record.audio = `${name}.wav64`;
      }
      records.push(record);
      progress((i + 1) / sources.length);
    }
    return records;
  }

  // What the port adds: model corrections measured by the console's own
  // mkfont, and the disc folders the runtime streams or reads as files.
  let plan = {model: false, measure: [], streams: [], data: []};
  await stage("port", async (progress) => {
    plan = director("port-plan", {slug: profile.slug, model: "/work/director/model.json"});
    if (!plan.model)
      return;
    vfs.mkdir("/work/port", true);
    vfs.write("/work/port/substitute.ttf", tools.substituteFont);
    const measured = [];
    if (plan.measure.length) {
      const mkfont = await fontTools(tools.fonts);
      for (const request of plan.measure) {
        check(signal);
        const substitute = request.font === "substitute";
        const source = substitute ? tools.substituteFont : vfs.read(`/work/director/${request.font}`);
        const name = substitute ? "substitute.ttf" : request.font.split("/").pop();
        const font = await mkfont(source, name, ["--size", String(request.size),
          ...request.ranges.flatMap((range) => ["--range", range]), "-c", "0"]);
        const path = `/work/port/measure-${measured.length}.font64`;
        vfs.write(path, font);
        measured.push([request.key, path]);
        progress(measured.length / plan.measure.length);
      }
    }
    const dataFiles = plan.data.reduce((n, folder) => n + filesUnder(vfs, `/work/extracted/${folder}`).length, 0);
    const externalMedia = plan.external_media ? convertExternalMedia(plan.external_media, progress) : [];
    director("port-model", {model: "/work/director/model.json", output: "/work/director", port,
      substitute: "/work/port/substitute.ttf", measured, data_files: dataFiles, external_media: externalMedia});
  });

  const program = await stage("compile", async () => {
    const files = vfs.list("/work/analysis/lingo")
      .filter(([name, kind]) => kind === "file" && name.endsWith(".lingo"))
      .map(([name]) => [name, decoder.decode(vfs.read(`/work/analysis/lingo/${name}`)).replace(/\r\n?/g, "\n")]);
    const text = tools.converter.lingoProgram(files);
    if (!port.launcher_movie && !port.native_compatibility)
      return text;
    // The executable program: compatibility fixes, then the embedded
    // projector's launcher handlers.
    vfs.mkdir("/work/aot", true);
    vfs.write("/work/aot/program.json", new TextEncoder().encode(text));
    director("port-program", {program: "/work/aot/program.json", manifest: "/work/analysis/source/manifest.json",
      output: "/work/aot/native-program.json", port});
    return decoder.decode(vfs.read("/work/aot/native-program.json"));
  });

  const result = await stage("package", async () => {
    const model = decoder.decode(vfs.read("/work/director/model.json"));
    const gamePackage = tools.converter.buildPackage({program, model, names: tools.names,
      bytecodeHeader: tools.bytecodeHeader, meta: tools.meta});
    // Sound members name the console's converted .wav64; the browser plays
    // the PCM WAV conversion produced.
    const images = pack(vfs, "/work/director/images");
    const audio = pack(vfs, "/work/director/wav", (name) => name.replace(/\.wav$/, ".wav64"));
    // The fonts the page draws text with (recovered originals and the port's
    // substitute, as OpenType/TrueType), named by the font number styles use.
    const fonts = packFiles(JSON.parse(model).fonts?.flatMap((font) =>
      /\.(otf|ttf)$/.test(font.asset) ? [[String(font.number), vfs.read(`/work/director/${font.asset}`)]] : []) ?? []);
    // Files the runtime reads by name: sound streams for playFile, by
    // "folder/stem" in lowercase as the runtime asks for them, with their
    // durations; and shipped data files by their path on the disc.
    const streams = streamPack(vfs, plan.streams, tools.converter);
    const data = packFiles(plan.data.flatMap((folder) => filesUnder(vfs, `/work/extracted/${folder}`)
      .map((relative) => [`${folder}/${relative}`, vfs.read(`/work/extracted/${folder}/${relative}`)])));
    // Print documents (Löwenzahn): the recovered text and the original
    // artwork, which the page lays out for the browser's print dialog.
    const printFiles = [];
    if (plan.print) {
      printFiles.push(["documents.json", new TextEncoder().encode(tools.converter.printDocuments(model))]);
      for (const name of filesUnder(vfs, `/work/extracted/${plan.print}`).filter((n) => n.endsWith(".PCT")))
        printFiles.push([name.replace(/\.PCT$/, ".png"), tools.converter.pictPng(vfs.read(`/work/extracted/${plan.print}/${name}`))]);
    }
    const print = packFiles(printFiles);
    report.streams = Object.keys(streams.index).length;
    report.data_files = Object.keys(data.index).length;
    report.package_bytes = gamePackage.length;
    report.images = Object.keys(images.index).length;
    report.sounds = Object.keys(audio.index).length;
    report.fonts = Object.keys(fonts.index).length;
    report.memory_bytes = vfs.bytes();
    return {package: gamePackage, images, audio, fonts, streams, data, print, program, report, model};
  });

  // The sounds and WAV streams as Opus, encoded by the embedder
  // (tools.encodeAudio: WebCodecs in the import worker) under the same names;
  // MP3 streams stay as they are. Without an encoder (the Node parity run)
  // the WAVs stay.
  await stage("sound", async (progress) => {
    if (!tools.encodeAudio) return;
    const total = Object.keys(result.audio.index).length + Object.keys(result.streams.index).length;
    let done = 0;
    for (const kind of ["audio", "streams"]) {
      const {index, blob} = result[kind];
      // Eight at a time: the browser runs each encoder on its own thread.
      const work = Object.entries(index);
      const entries = new Array(work.length);
      let next = 0;
      const lane = async () => {
        while (next < work.length) {
          const i = next++;
          const [name, [at, length, ...rest]] = work[i];
          check(signal);
          const bytes = blob.subarray(at, at + length);
          const wave = bytes[0] === 0x52 && bytes[1] === 0x49 && bytes[2] === 0x46 && bytes[3] === 0x46;
          entries[i] = [name, wave ? await tools.encodeAudio(bytes) : bytes.slice(), rest];
          progress(++done / total);
        }
      };
      await Promise.all(Array.from({length: 8}, lane));
      const packed = packFiles(entries.map(([name, bytes]) => [name, bytes]));
      for (const [name, , rest] of entries) packed.index[name].push(...rest);
      result[kind] = packed;
    }
  });

  // The linked movies' video, encoded by the embedder (tools.encodeVideo:
  // WebCodecs in the import worker) from the Cinepak frames the converter
  // decodes. Without an encoder (the Node parity run) there is none.
  result.video = await stage("video", async (progress) => {
    const entries = [];
    if (tools.encodeVideo) {
      for (const [i, {path, name}] of videoSources.entries()) {
        check(signal);
        const frames = tools.converter.frames(vfs.read(path));
        try {
          entries.push([name, await tools.encodeVideo(frames)]);
        } finally {
          frames.close();
        }
        progress((i + 1) / videoSources.length);
      }
    }
    result.report.videos = entries.length;
    return packFiles(entries);
  });
  return result;
}

// The importer's worker (bundled to importer.js by build.mjs). The page posts
// one import; this reads the chosen disc image through FileReaderSync, so
// every read is bounded and synchronous, and posts progress, then the game
// package and asset packs, or an error. Cancelling terminates the worker:
// nothing is stored until the page receives a finished result.
import {identify, importGame, ImportError} from "./pipeline.mjs";
import {loadConverter} from "./convert.mjs";
import {DirectorFile} from "projectorrays/web";

async function fetchBytes(url) {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
  return new Uint8Array(await response.arrayBuffer());
}
async function fetchText(url) {
  return new TextDecoder().decode(await fetchBytes(url));
}

self.onmessage = async ({data}) => {
  if (data.type !== "import") return;
  const post = (message, transfer) => self.postMessage(message, transfer ?? []);
  try {
    const {file, profiles, tools: urls, meta} = data;
    post({type: "progress", stage: "tools", index: 0, stages: 1, fraction: 0});
    const [converterWasm, parserWasm, names, bytecodeHeader, mkfontWasm, mkspriteWasm, substituteFont] =
      await Promise.all([
        fetchBytes(urls.converter), fetchBytes(urls.parserWasm), fetchText(urls.names),
        fetchBytes(urls.bytecodeHeader), fetchBytes(urls.mkfontWasm), fetchBytes(urls.mkspriteWasm),
        fetchBytes(urls.substituteFont),
      ]);
    // libdragon's font tools, as classic scripts defining their factories.
    importScripts(urls.mkfont, urls.mksprite);
    const converter = await loadConverter(converterWasm);
    // A classic worker runs the parser glue with importScripts, which the
    // loader chooses only for its script-tag mode.
    await DirectorFile.loadModule({glueUrl: urls.parserGlue, wasmBinary: parserWasm, useScriptTag: true});
    const reader = new FileReaderSync();
    const source = {
      size: file.size,
      readAt: (offset, length) => new Uint8Array(reader.readAsArrayBuffer(file.slice(offset, offset + length))),
    };
    const progress = (update) => post({type: "progress", ...update});
    const {profile, digest} = identify(source, profiles, converter,
      (fraction) => progress({stage: "verify", index: 0, stages: 10, fraction}));
    post({type: "identified", slug: profile.slug, digest});
    const result = await importGame({
      source, profile, verified: true, onProgress: progress,
      tools: {converter, DirectorFile, parserWasm, names, bytecodeHeader, substituteFont, encodeVideo,
        encodeAudio: (wav) => encodeAudio(converter, wav),
        fonts: {createMkfont: self.createMkfont, createMksprite: self.createMksprite, mkfontWasm, mkspriteWasm},
        meta: JSON.stringify({...meta, game: profile.slug, source_sha256: profile.source.sha256,
          director_version: profile.port.director_version}, Object.keys({
          converter: 0, director_version: 0, game: 0, schema_version: 0, source_sha256: 0}).sort()) + "\n"},
    });
    const packs = {images: result.images, audio: result.audio, fonts: result.fonts,
      streams: result.streams, data: result.data, video: result.video, print: result.print};
    post({type: "done", slug: profile.slug, package: result.package, packs, report: result.report},
      [result.package.buffer, ...Object.values(packs).map((pack) => pack.blob.buffer)]);
  } catch (error) {
    post({type: "error", message: String(error?.message ?? error),
      kind: error instanceof ImportError ? "unsupported" : error?.name ?? "error"});
  }
};

// A linked movie's video for the page's player (platforms/web/site/video.js):
// the converter's decoded frames through the browser's VP8 encoder, into the
// player's container, a keyframe every two seconds of video.
async function encodeVideo(frames) {
  const chunks = [];
  let failure = null;
  const encoder = new VideoEncoder({
    output: (chunk) => {
      const data = new Uint8Array(chunk.byteLength);
      chunk.copyTo(data);
      chunks.push([chunk.timestamp, chunk.type === "key" ? 1 : 0, data]);
    },
    error: (error) => { failure = error; },
  });
  encoder.configure({codec: "vp8", width: frames.width, height: frames.height,
    bitrate: 400_000, framerate: 15, latencyMode: "quality"});
  let lastKey = -Infinity;
  for (let frame; (frame = frames.next());) {
    const picture = new VideoFrame(frame.rgbx, {format: "RGBX", codedWidth: frames.width,
      codedHeight: frames.height, timestamp: frame.microseconds});
    const keyFrame = frame.microseconds - lastKey >= 2_000_000;
    if (keyFrame) lastKey = frame.microseconds;
    encoder.encode(picture, {keyFrame});
    picture.close();
    // Keep the queue short: the converter's frames are large.
    while (encoder.encodeQueueSize > 8) await new Promise((resolve) => setTimeout(resolve, 0));
    if (failure) throw failure;
  }
  await encoder.flush();
  encoder.close();
  if (failure) throw failure;
  chunks.sort((a, b) => a[0] - b[0]);
  let offset = 0;
  const header = {codec: "vp8", width: frames.width, height: frames.height,
    frames: chunks.map(([timestamp, key, data]) => {
      const entry = [timestamp, key, offset, data.length];
      offset += data.length;
      return entry;
    })};
  const text = new TextEncoder().encode(JSON.stringify(header));
  const out = new Uint8Array(4 + text.length + offset);
  new DataView(out.buffer).setUint32(0, text.length, true);
  out.set(text, 4);
  let at = 4 + text.length;
  for (const [, , data] of chunks) {
    out.set(data, at);
    at += data.length;
  }
  return out;
}

// A converted sound for the page's player (platforms/web/site/audio.js
// readSound): the PCM at 48 kHz from the converter, through the browser's
// Opus encoder, in the player's "D64A" container. The input is padded by two
// Opus frames so the whole sound survives; the header's frame count trims it.
async function encodeAudio(converter, wav) {
  const pcm = converter.resample48(wav);
  const pad = 1920, length = pcm.frames + pad;
  const planar = new Float32Array(length * pcm.channels);
  for (let c = 0; c < pcm.channels; c++)
    planar.set(pcm.samples.subarray(c * pcm.frames, (c + 1) * pcm.frames), c * length);
  const chunks = [];
  let config = null, failure = null;
  const encoder = new AudioEncoder({
    output: (chunk, meta) => {
      const data = new Uint8Array(chunk.byteLength);
      chunk.copyTo(data);
      chunks.push([chunk.timestamp, data]);
      if (meta?.decoderConfig) config = meta.decoderConfig;
    },
    error: (error) => { failure = error; },
  });
  encoder.configure({codec: "opus", sampleRate: 48000, numberOfChannels: pcm.channels,
    bitrate: pcm.channels > 1 ? 48000 : 32000});
  encoder.encode(new AudioData({format: "f32-planar", sampleRate: 48000, numberOfChannels: pcm.channels,
    numberOfFrames: length, timestamp: 0, data: planar}));
  await encoder.flush();
  encoder.close();
  if (failure) throw failure;
  let offset = 0;
  const description = config?.description ? new Uint8Array(config.description.buffer ?? config.description) : new Uint8Array();
  const header = {codec: "opus", sampleRate: 48000, channels: pcm.channels, frames: pcm.frames,
    sourceRate: pcm.sourceRate, sourceFrames: pcm.sourceFrames,
    description: btoa(String.fromCharCode(...description)),
    chunks: chunks.map(([timestamp, data]) => {
      const entry = [timestamp, offset, data.length];
      offset += data.length;
      return entry;
    })};
  const text = new TextEncoder().encode(JSON.stringify(header));
  const out = new Uint8Array(8 + text.length + offset);
  out.set([0x44, 0x36, 0x34, 0x41]); // "D64A"
  new DataView(out.buffer).setUint32(4, text.length, true);
  out.set(text, 8);
  let at = 8 + text.length;
  for (const [, data] of chunks) {
    out.set(data, at);
    at += data.length;
  }
  return out;
}

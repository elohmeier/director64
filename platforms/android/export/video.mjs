// A linked movie's video for the TV player, encoded by the host's FFmpeg
// (libvpx VP9, which Android TV decodes in hardware; about a quarter of VP8's
// size at the same quality) from the converter's decoded frames, into the
// same container the browser's encoder writes (platforms/web/import/vp8.mjs): a u32 header
// length, a JSON header {codec, width, height, frames: [[microseconds, key,
// offset, size]]} and the frames. The Android player decodes it with
// MediaCodec (platforms/android/jni/video.c).
import {spawn} from "node:child_process";

// VP9 profile 0 at constrained quality: CRF 30 up to 600 kbit/s, which on
// Löwenzahn's noisiest MPEG-1 clip scores above the VP8 at 900 kbit/s it
// replaces (SSIM 0.960 against 0.957) at 61% of its size. One coded frame
// per input frame (no hidden alternate references); the TV decoder's codec
// string.
const CODEC = "vp09.00.10.08";
const VP9 = ["-c:v", "libvpx-vp9", "-crf", "30", "-b:v", "600k", "-row-mt", "1", "-deadline", "good",
  "-cpu-used", "2", "-auto-alt-ref", "0", "-lag-in-frames", "0"];
// A VP9 frame header: marker 2, profile 0, then show_existing_frame and
// frame_type, which is 0 on a keyframe.
function keyframe(data) {
  return (data[0] & 0xc0) === 0x80 && (data[0] & 0x08) === 0 && (data[0] & 0x04) === 0;
}

// IVF: a 32-byte file header, then per frame a 12-byte header (u32 size,
// u64 timestamp) and the frame.
function ivfFrames(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (bytes.length < 32 || String.fromCharCode(...bytes.subarray(0, 4)) !== "DKIF")
    throw new Error("ffmpeg wrote no IVF stream");
  const frames = [];
  for (let at = view.getUint16(6, true); at + 12 <= bytes.length;) {
    const size = view.getUint32(at, true);
    frames.push(bytes.subarray(at + 12, at + 12 + size));
    at += 12 + size;
  }
  return frames;
}

export async function encodeVideo(frames) {
  const ffmpeg = spawn(process.env.FFMPEG ?? "ffmpeg", [
    "-hide_banner", "-loglevel", "error",
    "-f", "rawvideo", "-pix_fmt", "rgb0", "-s", `${frames.width}x${frames.height}`, "-r", "15",
    "-i", "pipe:0",
    // One output frame per input frame, a keyframe at least every two
    // seconds at 15 fps, as the browser encoder's.
    ...VP9, "-g", "30", "-fps_mode", "passthrough",
    "-f", "ivf", "pipe:1",
  ], {stdio: ["pipe", "pipe", "inherit"]});
  const output = [];
  ffmpeg.stdout.on("data", (chunk) => output.push(chunk));
  const exited = new Promise((resolve, reject) => {
    ffmpeg.on("error", reject);
    ffmpeg.on("close", (code) => (code === 0 ? resolve() : reject(new Error(`ffmpeg exited with ${code}`))));
  });
  const times = [];
  for (let frame; (frame = frames.next());) {
    times.push(frame.microseconds);
    if (!ffmpeg.stdin.write(frame.rgbx))
      await new Promise((resolve) => ffmpeg.stdin.once("drain", resolve));
  }
  ffmpeg.stdin.end();
  await exited;
  const encoded = ivfFrames(new Uint8Array(Buffer.concat(output)));
  if (encoded.length !== times.length)
    throw new Error(`ffmpeg wrote ${encoded.length} frames for ${times.length}`);
  let offset = 0;
  const header = {codec: CODEC, width: frames.width, height: frames.height,
    frames: encoded.map((data, i) => {
      const entry = [times[i], keyframe(data) ? 1 : 0, offset, data.length];
      offset += data.length;
      return entry;
    })};
  const text = new TextEncoder().encode(JSON.stringify(header));
  const out = new Uint8Array(4 + text.length + offset);
  new DataView(out.buffer).setUint32(0, text.length, true);
  out.set(text, 4);
  let at = 4 + text.length;
  for (const data of encoded) {
    out.set(data, at);
    at += data.length;
  }
  return out;
}

// MPEG-1 movies (DirectMedia's, Löwenzahn 6 onwards) go to FFmpeg whole:
// their sound as a 16-bit WAV, their picture as VP8 at the source frame
// rate, in the container above with the IVF timestamps as frame times.
// A sound of OPUS_SECONDS or more (the movies' soundtracks, long speech) goes
// to Ogg Opus, which the TV player decodes in the background (decode.c
// decode_media); shorter ones stay WAV so they start at once.
const OPUS_SECONDS = 8;
// (converter, wav): the import pool's encoder signature (jobs.mjs serve).
export async function encodeAudio(converter, wav) {
  const view = new DataView(wav.buffer, wav.byteOffset, wav.byteLength);
  if (wav.length < 44 || String.fromCharCode(...wav.subarray(0, 4)) !== "RIFF") return wav;
  const channels = view.getUint16(22, true), bytesPerSecond = view.getUint32(28, true);
  if (!bytesPerSecond || (wav.length - 44) / bytesPerSecond < OPUS_SECONDS) return wav;
  return run(["-f", "wav", "-i", "pipe:0", "-c:a", "libopus", "-b:a", channels > 1 ? "96k" : "64k",
    "-vbr", "on", "-application", "audio", "-f", "ogg", "pipe:1"], wav);
}
function run(args, input) {
  const ffmpeg = spawn(process.env.FFMPEG ?? "ffmpeg", ["-hide_banner", "-loglevel", "error", ...args],
    {stdio: ["pipe", "pipe", "inherit"]});
  const output = [];
  ffmpeg.stdout.on("data", (chunk) => output.push(chunk));
  const exited = new Promise((resolve, reject) => {
    ffmpeg.on("error", reject);
    ffmpeg.on("close", (code) => (code === 0 ? resolve() : reject(new Error(`ffmpeg exited with ${code}`))));
  });
  // A movie without the stream asked for closes stdin early.
  ffmpeg.stdin.on("error", () => {});
  ffmpeg.stdin.end(input);
  // A copy of its own: a pooled Buffer must not cross to another thread.
  return exited.then(() => {
    const joined = Buffer.concat(output);
    const out = new Uint8Array(joined.length);
    out.set(joined);
    return out;
  });
}

// The MPEG's sound, or null without one. A piped WAV leaves its sizes open.
export async function mpegAudio(bytes) {
  let wav;
  try {
    wav = await run(["-i", "pipe:0", "-vn", "-map", "0:a:0", "-c:a", "pcm_s16le", "-f", "wav", "pipe:1"], bytes);
  } catch {
    // A silent movie: FFmpeg has no stream to write.
    return null;
  }
  if (wav.length <= 44) return null;
  const view = new DataView(wav.buffer, wav.byteOffset, wav.byteLength);
  let at = 12;
  while (at + 8 <= wav.length && String.fromCharCode(...wav.subarray(at, at + 4)) !== "data")
    at += 8 + view.getUint32(at + 4, true);
  if (at + 8 > wav.length) throw new Error("ffmpeg wrote no WAV data");
  // The pipeline reads a canonical 44-byte header (fmt at 12, data at 36).
  const fmt = wav.subarray(12, 36);
  const data = wav.subarray(at + 8);
  const out = new Uint8Array(44 + data.length);
  const o = new DataView(out.buffer);
  out.set(new TextEncoder().encode("RIFF"), 0);
  o.setUint32(4, 36 + data.length, true);
  out.set(new TextEncoder().encode("WAVE"), 8);
  out.set(fmt, 12);
  o.setUint32(16, 16, true);
  out.set(new TextEncoder().encode("data"), 36);
  o.setUint32(40, data.length, true);
  out.set(data, 44);
  return out;
}

export async function encodeMpeg(bytes) {
  const ivf = await run(["-i", "pipe:0", "-an", ...VP9, "-g", "50", "-f", "ivf", "pipe:1"], bytes);
  const view = new DataView(ivf.buffer, ivf.byteOffset, ivf.byteLength);
  if (ivf.length < 32 || String.fromCharCode(...ivf.subarray(0, 4)) !== "DKIF")
    throw new Error("ffmpeg wrote no IVF stream");
  const width = view.getUint16(12, true), height = view.getUint16(14, true);
  const rate = view.getUint32(16, true), scale = view.getUint32(20, true);
  const frames = [];
  for (let at = view.getUint16(6, true); at + 12 <= ivf.length;) {
    const size = view.getUint32(at, true);
    const pts = Number(view.getBigUint64(at + 4, true));
    frames.push([Math.round(pts * scale * 1e6 / rate), ivf.subarray(at + 12, at + 12 + size)]);
    at += 12 + size;
  }
  let offset = 0;
  const header = {codec: CODEC, width, height,
    frames: frames.map(([time, data]) => {
      const entry = [time, keyframe(data) ? 1 : 0, offset, data.length];
      offset += data.length;
      return entry;
    })};
  const text = new TextEncoder().encode(JSON.stringify(header));
  const out = new Uint8Array(4 + text.length + offset);
  new DataView(out.buffer).setUint32(0, text.length, true);
  out.set(text, 4);
  let at = 4 + text.length;
  for (const [, data] of frames) {
    out.set(data, at);
    at += data.length;
  }
  return out;
}

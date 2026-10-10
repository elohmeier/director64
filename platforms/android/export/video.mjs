// A linked movie's video for the TV player, encoded by the host's FFmpeg
// (libvpx VP8) from the converter's decoded frames, into the same container
// the browser's encoder writes (platforms/web/import/vp8.mjs): a u32 header
// length, a JSON header {codec, width, height, frames: [[microseconds, key,
// offset, size]]} and the frames. The Android player decodes it with
// MediaCodec (platforms/android/jni/video.c).
import {spawn} from "node:child_process";

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
    "-c:v", "libvpx", "-b:v", "600k", "-crf", "8", "-g", "30", "-auto-alt-ref", "0",
    "-lag-in-frames", "0", "-drop-threshold", "0", "-fps_mode", "passthrough",
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
  const header = {codec: "vp8", width: frames.width, height: frames.height,
    frames: encoded.map((data, i) => {
      // A VP8 frame tag's lowest bit is clear on a keyframe.
      const entry = [times[i], (data[0] & 1) === 0 ? 1 : 0, offset, data.length];
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

// Synchronous pixel decoding for rendered RPC probes. Decode only the GOP
// through the requested frame; keep one picture per active video slot.
import {spawnSync} from "node:child_process";
import {parseVideo, frameAt} from "../site/video.js";

function ivf(video, begin, end) {
  const codec = video.codec === "vp8" ? "VP80" : video.codec.startsWith("vp09") ? "VP90" : null;
  if (!codec) throw new Error(`probe video codec ${video.codec} is unsupported`);
  const frames = video.frames.slice(begin, end + 1);
  const out = new Uint8Array(32 + frames.reduce((n, f) => n + 12 + f[3], 0));
  const view = new DataView(out.buffer);
  out.set(new TextEncoder().encode("DKIF"));
  view.setUint16(6, 32, true);
  out.set(new TextEncoder().encode(codec), 8);
  view.setUint16(12, video.width, true); view.setUint16(14, video.height, true);
  view.setUint32(16, 1000000, true); view.setUint32(20, 1, true);
  view.setUint32(24, frames.length, true);
  let at = 32;
  for (const [time, , offset, length] of frames) {
    view.setUint32(at, length, true);
    view.setBigUint64(at + 4, BigInt(time - frames[0][0]), true);
    out.set(video.data.subarray(offset, offset + length), at + 12);
    at += 12 + length;
  }
  return out;
}

function encoded(video, begin, end) {
  if (!/^avc1\.[0-9a-f]{6}$/i.test(video.codec))
    return {format: "ivf", bytes: ivf(video, begin, end)};
  // Native packs contain Annex B NAL units, including parameter sets at
  // each keyframe. Dimensions are zero until the SPS has been decoded.
  const chunks = video.frames.slice(begin, end + 1).map(([, , offset, length]) =>
    video.data.subarray(offset, offset + length));
  return {format: "h264", bytes: Buffer.concat(chunks)};
}

function picture(bytes) {
  const end = bytes.indexOf("ENDHDR\n");
  if (end < 0 || end > 1024 || bytes.subarray(0, 3).toString() !== "P7\n")
    throw new Error("decoder returned no PAM picture");
  const header = bytes.subarray(0, end).toString();
  const number = (name) => Number(header.match(new RegExp(`^${name} (\\d+)$`, "m"))?.[1]);
  const width = number("WIDTH"), height = number("HEIGHT");
  if (!width || !height || width > 4096 || height > 4096 ||
      number("DEPTH") !== 4 || number("MAXVAL") !== 255)
    throw new Error("decoder returned invalid RGBA dimensions");
  const pixels = bytes.subarray(end + 7);
  if (pixels.length !== width * height * 4)
    throw new Error("decoder returned a truncated RGBA picture");
  return {pixels, width, height};
}

export function videoProbe(lookup) {
  const slots = new Map();
  let revision = 0;
  return {
    update(slot, asset, sound, time600) {
      if (!asset) { if (slots.delete(slot)) revision++; return; }
      let entry = slots.get(slot);
      if (!entry || entry.asset !== asset) {
        const bytes = lookup(asset);
        if (!bytes) throw new Error(`missing probe video ${asset}`);
        entry = {asset, video: parseVideo(bytes), index: -1, pixels: null};
        slots.set(slot, entry);
      }
      const {video} = entry;
      const index = frameAt(video.frames, Math.round(time600 * 1e6 / 600));
      if (index < 0 || index === entry.index) return;
      let begin = index;
      while (begin > 0 && !video.frames[begin][1]) begin--;
      const input = encoded(video, begin, index);
      const result = spawnSync(process.env.FFMPEG ?? "ffmpeg", ["-hide_banner", "-loglevel", "error",
        "-xerror", "-err_detect", "explode", "-f", input.format, "-i", "pipe:0", "-vf", `select=eq(n\\,${index - begin})`,
        "-frames:v", "1", "-pix_fmt", "rgba", "-c:v", "pam", "-f", "image2pipe", "pipe:1"],
      {input: input.bytes, maxBuffer: 4096 * 4096 * 4 + 1024, timeout: 30000});
      if (result.error || result.status !== 0)
        throw new Error(`probe video ${asset}: ${result.error?.message ?? result.stderr?.toString() ?? "decode failed"}`);
      const decoded = picture(result.stdout);
      if (video.width && (decoded.width !== video.width || decoded.height !== video.height))
        throw new Error(`probe video ${asset}: decoded dimensions disagree with its index`);
      video.width = decoded.width; video.height = decoded.height;
      entry.pixels = decoded.pixels; entry.index = index; revision++;
    },
    frame(slot) {
      const entry = slots.get(slot);
      return entry?.pixels ? {pixels: entry.pixels, width: entry.video.width, height: entry.video.height} : null;
    },
    revision: () => revision,
  };
}

// A linked movie's video for the page's player (platforms/web/site/video.js):
// the converter's decoded frames through the browser's VP8 encoder, into the
// player's container, a keyframe every two seconds of video.
export async function encodeVideo(frames) {
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

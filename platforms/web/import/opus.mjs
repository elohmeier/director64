// A converted sound for the page's player (platforms/web/site/audio.js
// readSound): the PCM at 48 kHz from the converter, through the browser's
// Opus encoder, in the player's "D64A" container. The input is padded by two
// Opus frames so the whole sound survives; the header's frame count trims it.
export async function encodeAudio(converter, wav) {
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

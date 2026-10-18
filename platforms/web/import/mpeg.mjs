// MPEG-1/MP2 decoding is shared by browser import and its Node diagnostics.
// Metadata parsing independently checks geometry and decoded frame counts:
// PL_MPEG itself deliberately skips malformed packets without an error API.
export async function mpegTools(createMpeg, wasmBinary, converter, encodeVideo) {
  const module = await createMpeg({wasmBinary});
  function open(bytes, audio) {
    const ptr = module._malloc(bytes.length);
    if (!ptr) throw new Error("MPEG source allocation failed");
    module.HEAPU8.set(bytes, ptr);
    let handle;
    try {
      handle = module._d64m_open(ptr, bytes.length, audio ? 1 : 0);
      if (!handle) throw new Error("MPEG decoder could not read stream headers");
    } catch (error) {
      module._free(ptr);
      throw error;
    }
    return {handle, close() { module._d64m_close(handle); module._free(ptr); }};
  }
  return {
    async encodeMpeg(bytes) {
      const info = converter.mpegInfo(bytes);
      const decoder = open(converter.mpegStreams(bytes).video, false);
      try {
        const {handle} = decoder;
        const width = module._d64m_width(handle), height = module._d64m_height(handle);
        if (width !== info.width || height !== info.height) throw new Error("MPEG geometry mismatch");
        let count = 0, emitted = 0;
        const result = await encodeVideo({width, height, rate: info.rate / info.scale,
          next() {
            for (;;) {
              const ptr = module._d64m_frame(handle);
              const corrupt = module._d64m_corrupt(handle);
              if (corrupt) throw new Error(`MPEG contains ${corrupt} corrupt slices; picture recovery is unqualified`);
              if (!ptr) return null;
              count++;
              if (count > info.frames) throw new Error("MPEG decoded extra pictures");
              // A leading open GOP can contain B pictures referring to a
              // previous GOP outside this file. Do not display missing refs.
              if (count <= (info.skipped_initial_frames ?? 0)) continue;
              return {microseconds: Math.round(emitted++ * info.scale / info.rate * 1e6),
                rgbx: module.HEAPU8.subarray(ptr, ptr + width * height * 4)};
            }
          }});
        if (count !== info.frames) throw new Error(`MPEG decoded ${count} of ${info.frames} pictures`);
        if (emitted !== (info.decoded_frames ?? info.frames)) throw new Error("MPEG presentation frame count mismatch");
        return result;
      } finally {
        decoder.close();
      }
    },
    async mpegAudio(bytes) {
      const info = converter.mpegInfo(bytes);
      const audio = converter.mpegStreams(bytes).audio;
      if (!audio.length) return null;
      const decoder = open(audio, true);
      try {
        const {handle} = decoder;
        if (!module._d64m_audio_streams(handle)) return null;
        const rate = module._d64m_sample_rate(handle);
        if (!rate) throw new Error("MPEG audio stream is not decodable MP2");
        const chunks = [];
        const lead = Math.round((info.audio_lead_seconds ?? 0) * rate);
        // Audio and video decode on independent clocks. Match their first
        // presentation timestamps by trimming early audio or adding silence.
        let trim = Math.max(0, lead) * 4;
        let size = Math.max(0, -lead) * 4, decoded = 0;
        if (size) chunks.push(new Uint8Array(size));
        for (let ptr; (ptr = module._d64m_samples(handle));) {
          const skip = Math.min(trim, 1152 * 2 * 2);
          trim -= skip;
          decoded++;
          const chunk = module.HEAPU8.slice(ptr + skip, ptr + 1152 * 2 * 2);
          chunks.push(chunk);
          size += chunk.length;
        }
        if (!decoded) throw new Error("MPEG audio stream decoded no samples");
        if (info.audio_frames !== undefined && decoded !== info.audio_frames)
          throw new Error(`MP2 decoded ${decoded} of ${info.audio_frames} frames`);
        const wav = new Uint8Array(44 + size), view = new DataView(wav.buffer);
        wav.set(new TextEncoder().encode("RIFF"), 0);
        view.setUint32(4, 36 + size, true);
        wav.set(new TextEncoder().encode("WAVEfmt "), 8);
        view.setUint32(16, 16, true);
        view.setUint16(20, 1, true);
        view.setUint16(22, 2, true);
        view.setUint32(24, rate, true);
        view.setUint32(28, rate * 4, true);
        view.setUint16(32, 4, true);
        view.setUint16(34, 16, true);
        wav.set(new TextEncoder().encode("data"), 36);
        view.setUint32(40, size, true);
        let offset = 44;
        for (const chunk of chunks) { wav.set(chunk, offset); offset += chunk.length; }
        return wav;
      } finally {
        decoder.close();
      }
    },
  };
}

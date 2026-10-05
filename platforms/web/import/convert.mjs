// The converter's Rust stages as WebAssembly (compiler/wasm): ISO inventory,
// streamed SHA-256, the Director converter stages, the Lingo parser and the
// packager. Synchronous calls: the importer runs them in a worker, never on
// the page's thread.
const encoder = new TextEncoder(), decoder = new TextDecoder();

// The operations compiler/wasm's HostFiles asks of the embedder's files
// (its `Op` numbering), each answering a status, a size or a staged reply.
function fileOperations(current, memory) {
  let staged = null;
  const stage = (bytes) => {
    staged = bytes;
    return bytes.length;
  };
  return {
    d64c_fs: (op, pathPointer, pathLength, dataPointer, dataLength) => {
      const files = current();
      if (!files) return -1;
      const path = decoder.decode(memory().subarray(pathPointer, pathPointer + pathLength));
      try {
        switch (op) {
          case 0: return stage(files.read(path));
          case 1: files.write(path, memory().slice(dataPointer, dataPointer + dataLength)); return 0;
          case 2: return files.exists(path) ? 1 : 0;
          case 3: return files.isFile(path) ? 1 : 0;
          case 4: return stage(encoder.encode(JSON.stringify(files.list(path).map(([name, kind]) => [name, kind === "dir"]))));
          case 5: files.mkdir(path, true); return 0;
          case 6: files.remove(path); return 0;
          case 7: return files.size(path);
          default: return -1;
        }
      } catch {
        return -1;
      }
    },
    d64c_fs_take: (pointer) => {
      memory().set(staged, pointer);
      staged = null;
    },
  };
}

export async function loadConverter(bytes) {
  let readAt = null, files = null;
  const host = fileOperations(() => files, () => new Uint8Array(instance.exports.memory.buffer));
  const {instance} = await WebAssembly.instantiate(bytes, {
    env: {
      ...host,
      d64c_read_at: (offset, pointer, length) => {
        try {
          const data = readAt(offset, length);
          if (data.length !== length) return 1;
          new Uint8Array(instance.exports.memory.buffer, pointer, length).set(data);
          return 0;
        } catch {
          return 1;
        }
      },
    },
  });
  const x = instance.exports;
  const memory = () => new Uint8Array(x.memory.buffer);
  function put(data) {
    const bytes = typeof data === "string" ? encoder.encode(data) : data;
    const pointer = x.d64c_alloc(bytes.length);
    memory().set(bytes, pointer);
    return [pointer, bytes.length];
  }
  function release(...inputs) {
    for (const [pointer, length] of inputs) x.d64c_free(pointer, length);
  }
  function result() {
    return memory().slice(x.d64c_result_ptr(), x.d64c_result_ptr() + x.d64c_result_len());
  }
  function call(status) {
    const out = result();
    if (status) throw new Error(decoder.decode(out));
    return out;
  }
  return {
    sha256() {
      const handle = x.d64c_sha256_new();
      return {
        update(data) {
          // Large inputs cross in bounded pieces.
          for (let at = 0; at < data.length; at += 1 << 22) {
            const input = put(data.subarray(at, Math.min(data.length, at + (1 << 22))));
            x.d64c_sha256_update(handle, ...input);
            release(input);
          }
        },
        hex: () => decoder.decode(call(x.d64c_sha256_finish(handle))),
      };
    },
    // readAtSync(offset, length) -> Uint8Array: bounded synchronous reads
    // (FileReaderSync in a worker, a file handle in Node).
    isoInventory(size, readAtSync) {
      readAt = readAtSync;
      try {
        return JSON.parse(decoder.decode(call(x.d64c_iso_inventory(size))))
          .map(([path, offset, length]) => ({path, offset, length}));
      } finally {
        readAt = null;
      }
    },
    // One converter stage (`director64-aot director <stage>`) over `vfs`
    // (the importer's Vfs); returns the stage's JSON result.
    director(request, vfs) {
      const input = put(JSON.stringify(request));
      files = vfs;
      try {
        return JSON.parse(decoder.decode(call(x.d64c_director(...input))));
      } finally {
        files = null;
        release(input);
      }
    },
    // A ZIP game source over the same bounded reads: its files'
    // [{path, length}], then each one's bytes.
    zipInventory(size, readAtSync) {
      readAt = readAtSync;
      try {
        return JSON.parse(decoder.decode(call(x.d64c_zip_inventory(size))))
          .map(([path, length], index) => ({path, length, index}));
      } finally {
        readAt = null;
      }
    },
    zipFile(size, index, readAtSync) {
      readAt = readAtSync;
      try {
        return call(x.d64c_zip_file(size, index));
      } finally {
        readAt = null;
      }
    },
    // A stored file's plain bytes: a deflated one ("D64Z") inflated.
    inflate(data) {
      if (data.length < 8 || data[0] !== 0x44 || data[1] !== 0x36 || data[2] !== 0x34 || data[3] !== 0x5a)
        return data;
      const input = put(data);
      try {
        return call(x.d64c_inflate(...input));
      } finally {
        release(input);
      }
    },
    // Löwenzahn's linked movies and external sounds (convert/quicktime.rs).
    movInfo(data) {
      const input = put(data);
      try {
        return JSON.parse(decoder.decode(call(x.d64c_mov_info(...input))));
      } finally {
        release(input);
      }
    },
    movAudio(data) {
      const input = put(data);
      try {
        const wav = call(x.d64c_mov_audio(...input));
        return wav.length ? wav : null;
      } finally {
        release(input);
      }
    },
    aiffWav(data) {
      const input = put(data);
      try {
        return call(x.d64c_aiff_wav(...input));
      } finally {
        release(input);
      }
    },
    // A movie's Cinepak frames, one at a time: {width, height, next()}, where
    // next() gives {microseconds, rgbx} or null after the last.
    frames(data) {
      const input = put(data);
      let size;
      try {
        size = JSON.parse(decoder.decode(call(x.d64c_frames_open(...input))));
      } finally {
        release(input);
      }
      return {
        ...size,
        next() {
          const out = call(x.d64c_frames_next());
          if (!out.length) return null;
          const view = new DataView(out.buffer, out.byteOffset);
          return {microseconds: Number(view.getBigUint64(0, true)), rgbx: out.subarray(8)};
        },
        close: () => x.d64c_frames_close(),
      };
    },
    pictPng(data) {
      const input = put(data);
      try {
        return call(x.d64c_pict_png(...input));
      } finally {
        release(input);
      }
    },
    printDocuments(modelText) {
      const input = put(modelText);
      try {
        return decoder.decode(call(x.d64c_print_documents(...input)));
      } finally {
        release(input);
      }
    },
    // A PCM WAV at 48 kHz for the Opus encoder: {channels, frames,
    // sourceRate, sourceFrames, samples (planar Float32Array)}.
    resample48(wav) {
      const input = put(wav);
      try {
        const out = call(x.d64c_resample48(...input));
        const view = new DataView(out.buffer, out.byteOffset);
        const [channels, frames, sourceRate, sourceFrames] = [0, 4, 8, 12].map((at) => view.getUint32(at, true));
        return {channels, frames, sourceRate, sourceFrames,
          samples: new Float32Array(out.buffer.slice(out.byteOffset + 16, out.byteOffset + out.length))};
      } finally {
        release(input);
      }
    },
    // A disc sound file for playFile: {bytes, ms}, AIFF rewritten as WAV.
    stream(data) {
      const input = put(data);
      try {
        const out = call(x.d64c_stream(...input));
        return {ms: new DataView(out.buffer, out.byteOffset).getUint32(0, true), bytes: out.subarray(4)};
      } finally {
        release(input);
      }
    },
    // files: [[name, text], ...] sorted by name; returns program.json text.
    lingoProgram(files) {
      const input = put(JSON.stringify(files));
      try {
        return decoder.decode(call(x.d64c_lingo_program(...input)));
      } finally {
        release(input);
      }
    },
    buildPackage({program, model, names, bytecodeHeader, meta}) {
      const inputs = [put(program), put(model), put(names), put(bytecodeHeader), put(meta)];
      try {
        return call(x.d64c_package(...inputs.flat()));
      } finally {
        release(...inputs);
      }
    },
  };
}

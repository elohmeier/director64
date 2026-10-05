// Imports of the Wasm runtime. Each forwards to the embedder's host object,
// passed as `director64Host` when the module is instantiated: the page
// (site/app.js) or the Node probe harness (node/probe.mjs).
addToLibrary({
  d64_host_asset__deps: ['$UTF8ToString'],
  d64_host_asset: (name, out, cap) => {
    const bytes = Module.director64Host.asset(UTF8ToString(name));
    if (!bytes) return 0;
    if (out && cap >= bytes.length) HEAPU8.set(bytes, out);
    return bytes.length;
  },
  d64_host_text__deps: ['$UTF8ToString'],
  d64_host_text: (text, width, height, font, size, align, ascent, lineHeight, coverage) => {
    const style = size ? {font, size, align, ascent, lineHeight} : null;
    const plane = Module.director64Host.text?.(UTF8ToString(text), width, height, style);
    if (!plane || plane.length !== width * height) return 0;
    HEAPU8.set(plane, coverage);
    return 1;
  },
  d64_host_sound__deps: ['$UTF8ToString'],
  d64_host_sound: (channel, asset, looping, loopStart, loopEnd) => {
    Module.director64Host.sound?.(channel, asset ? UTF8ToString(asset) : null,
      !!looping, loopStart >>> 0, loopEnd >>> 0);
  },
  d64_host_gain: (channel, gain) => Module.director64Host.gain?.(channel, gain),
  d64_host_save_commit: (ok) => Module.director64Host.saveCommit?.(!!ok),
  d64_host_trace__deps: ['$UTF8ToString'],
  d64_host_trace: (text) => Module.director64Host.trace?.(UTF8ToString(text)),
  d64_host_stream__deps: ['$UTF8ToString'],
  d64_host_stream: (channel, name) =>
    (Module.director64Host.stream?.(channel, name ? UTF8ToString(name) : null) ?? 0) >>> 0,
  d64_host_video__deps: ['$UTF8ToString'],
  d64_host_video: (sprite, video, audio, time, rate, serial, gain) =>
    Module.director64Host.video?.(sprite, video ? UTF8ToString(video) : null,
      audio ? UTF8ToString(audio) : "", time, rate, serial >>> 0, gain),
  d64_host_video_frame: (sprite, out, cap, width, height) => {
    const frame = Module.director64Host.videoFrame?.(sprite);
    if (!frame || frame.width * frame.height > cap) return 0;
    HEAPU8.set(frame.pixels, out);
    HEAPU32[width >> 2] = frame.width;
    HEAPU32[height >> 2] = frame.height;
    return 1;
  },
  d64_host_video_revision: () => (Module.director64Host.videoRevision?.() ?? 0) >>> 0,
  d64_host_data__deps: ['$UTF8ToString'],
  d64_host_data: (name, out, cap) => {
    const bytes = Module.director64Host.data?.(UTF8ToString(name));
    if (!bytes) return -1;
    if (out && cap >= bytes.length) HEAPU8.set(bytes, out);
    return bytes.length;
  },
});

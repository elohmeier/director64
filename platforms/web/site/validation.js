// Explicit test mode for the real browser player. No script calls, movie jumps,
// global assignment or save injection are accepted by this input adapter.
export function inputCommand(command, width, height) {
  if (typeof command !== "string" || /[\r\n]/.test(command)) throw new Error("one input command required");
  if (command === "state" || command === "reboot") return {command};
  let match = command.match(/^(step|pad) (\d+) (-?\d+) (-?\d+) (\d+)$/);
  if (match) {
    const [, kind, n, x, y, buttons] = match;
    if (+n < 1 || +n > 100000 || +buttons > 65535 ||
        (kind === "step" ? +x < 0 || +x >= width || +y < 0 || +y >= height
          : +x < -128 || +x > 127 || +y < -128 || +y > 127))
      throw new Error("input outside the stage or controller range");
    return {command, ticks: +n, single: `${kind} 1 ${x} ${y} ${buttons}`};
  }
  match = command.match(/^key (\d+) (\d+) ([01])$/);
  if (match && +match[1] <= 255 && +match[2] <= 0x10ffff) return {command};
  match = command.match(/^text (\d+) ([^\r\n]{1,63})$/u);
  if (match && +match[1] > 0) return {command};
  throw new Error("validation accepts player input only");
}

export function stateReply(reply) {
  const line = reply?.split("\n").findLast(line => line.startsWith("{"));
  if (!line) throw new Error("runtime returned no state");
  const state = JSON.parse(line);
  if (state.error || state.script_errors || state.quit)
    throw new Error(state.error || state.last_script_error || "unexpected game exit");
  return state;
}

export function attachValidation({module, audio, videos, present, width, height, seed,serviceRequests=()=>false}) {
  module._d64_probe_render(1);
  let busy = false, audioBlocks = [], audioSamples = 0, realtimeDeadline = null;
  const timing = {realtime_ticks: 0, accelerated_ticks: 0, max_lateness_ms: 0};
  // Capture final PCM after authored gains, master volume and output clamp.
  // The tap's output is silent; the original connection still plays normally.
  const tap = audio.context.createScriptProcessor(2048, 2, 2);
  let recording = false;
  tap.onaudioprocess = event => {
    if (!recording) return;
    const input = event.inputBuffer;
    const block = new Float32Array(input.length * 2);
    for (let i = 0; i < input.length; i++) {
      block[i * 2] = input.getChannelData(0)[i];
      block[i * 2 + 1] = input.getChannelData(Math.min(1, input.numberOfChannels - 1))[i];
    }
    // A single observation is bounded to one minute. The runner saves each
    // observation before proceeding to the next.
    if (audioSamples + block.length > audio.context.sampleRate * 2 * 60)
      throw new Error("validation audio observation exceeded one minute");
    audioSamples += block.length;
    audioBlocks.push(block);
  };
  (audio.output ?? audio.master).connect(tap);
  tap.connect(audio.context.destination);
  const rpc = command => {
    const state = stateReply(module.ccall("d64_rpc", "string", ["string"], [command]));
    serviceRequests();
    const failure = audio.errors[0] || videos.errors[0];
    if (failure) throw new Error(failure);
    return state;
  };
  const render = () => present(module._d64_render(0, 0, 0));
  return {
    width, height, seed, clock: "controlled-60hz", fonts: "actual-browser-fonts",
    state: () => rpc("state"),
    timing: () => ({...timing}),
    async advance(command, realtime = true) {
      const parsed = inputCommand(command, width, height);
      if (busy) throw new Error("concurrent validation input");
      busy = true;
      try {
        let state;
        if (!realtime) realtimeDeadline = null;
        else if (realtimeDeadline === null) realtimeDeadline = performance.now();
        if (command === "reboot") { audio.stopAll(); videos.closeAll(); }
        if (parsed.ticks) {
          for (let i = 0; i < parsed.ticks; i++) {
            state = rpc(parsed.single);
            if (realtime) {
              timing.realtime_ticks++;
              realtimeDeadline += 1000 / 60;
              const delay = realtimeDeadline - performance.now();
              timing.max_lateness_ms = Math.max(timing.max_lateness_ms, -delay);
              if (delay > 0) await new Promise(resolve => setTimeout(resolve, delay));
            } else {
              timing.accelerated_ticks++;
              if (i % 12 === 11) await new Promise(resolve => setTimeout(resolve, 0));
            }
          }
        } else state = rpc(command);
        render();
        return state;
      } finally { busy = false; }
    },
    capture() {
      render();
      const bytes = module.HEAPU8.slice(module._d64_pixels(), module._d64_pixels() + width * height * 4);
      // Chunk conversion avoids a browser argument-stack overflow at 800x600.
      let binary = "";
      for (let i = 0; i < bytes.length; i += 8192)
        binary += String.fromCharCode(...bytes.subarray(i, i + 8192));
      const canvas = document.createElement("canvas");
      canvas.width = width; canvas.height = height;
      canvas.getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(bytes), width, height), 0, 0);
      return {state: rpc("state"), rgba: btoa(binary), png: canvas.toDataURL("image/png").split(",")[1]};
    },
    async startAudio() {
      audioBlocks = []; audioSamples = 0;
      await audio.resume();
      recording = true;
    },
    takeAudio() {
      recording = false;
      const pcm = new Float32Array(audioSamples);
      let at = 0;
      for (const block of audioBlocks) { pcm.set(block, at); at += block.length; }
      const bytes = new Uint8Array(pcm.buffer);
      let binary = "";
      for (let i = 0; i < bytes.length; i += 8192)
        binary += String.fromCharCode(...bytes.subarray(i, i + 8192));
      audioBlocks = []; audioSamples = 0;
      return {pcm: btoa(binary), rate: audio.context.sampleRate, channels: 2, format: "f32le",
        context_state: audio.context.state, master_gain: audio.master.gain.value,
        active_channels: audio.voices?.filter(Boolean).length ?? 0};
    },
  };
}

// The probe line protocol over the web build's Wasm runtime, which the
// fuzzer, the parity tool and the games' journey scripts drive:
//   probe.mjs SITE MOVIE rpc   then one command per line on stdin.
// Only the game's entry movie can start.
import {existsSync, readFileSync, writeFileSync} from "node:fs";
import {inflateRawSync} from "node:zlib";
import {createInterface} from "node:readline";
import {createHash} from "node:crypto";
import {join} from "node:path";
import {pathToFileURL} from "node:url";
import {videoProbe} from "./video.mjs";
import {decodeWav, isOpusSound, readOpusHeader, isOggOpus, oggOpusInfo} from "../site/audio.js";
import {validateFlashSound} from "../site/flash-audio.js";
import {formatText} from "../site/rich-text.js";
import {probeMetrics} from "./metrics.mjs";

const metrics = probeMetrics();
const startup = metrics.start();

const [site, movie = "START", mode, ...options] = process.argv.slice(2);
if (!site || mode !== "rpc" || options.some(option => option !== "--state-only")) {
  process.stderr.write("usage: probe.mjs SITE MOVIE rpc [--state-only]\n");
  process.exit(2);
}
// Render by default: a state-only RPC replay cannot detect compositor errors.
// The opt-out is explicit for investigations of script-state parity alone.
const render = !options.includes("--state-only");
const observations = process.env.DIRECTOR64_PROBE_OBSERVATIONS === "1";
const cacheOff = process.env.DIRECTOR64_PROBE_CACHE === "off";
const audioEvents = [];
const audioEvent = (type, ...args) => {
  if (observations) audioEvents.push([type, ...args]);
};
if (observations && !render) throw new Error("pixel observations require rendering");
const build = JSON.parse(readFileSync(join(site, "build.json"), "utf8"));
if (movie.toUpperCase() !== build.game.entry_movie.toUpperCase()) {
  process.stderr.write(`the browser runtime starts at ${build.game.entry_movie} only\n`);
  process.exit(2);
}
function pack(kind) {
  if (!existsSync(join(site, "assets", `${kind}.json`))) return () => null;
  const index = JSON.parse(readFileSync(join(site, "assets", `${kind}.json`), "utf8"));
  const blob = readFileSync(join(site, "assets", `${kind}.bin`));
  return (name) => {
    const entry = index[name];
    if (!entry) return null;
    const bytes = blob.subarray(entry[0], entry[0] + entry[1]);
    // "D64Z" + raw length + raw deflate, as the player inflates on load
    // (platforms/web/site/player.js); WebP images ("D64W") go to the runtime.
    if (bytes.length >= 8 && bytes[0] === 0x44 && bytes[1] === 0x36 && bytes[2] === 0x34 && bytes[3] === 0x5a)
      return new Uint8Array(inflateRawSync(bytes.subarray(8)));
    return bytes;
  };
}
const images = pack("images");
const audio = pack("audio"), checkedSounds = new Set();
const data = pack("data");
const streamIndexPath = join(site,"assets","streams.json");
const streams = new Map(existsSync(streamIndexPath)
  ? Object.entries(JSON.parse(readFileSync(streamIndexPath,"utf8"))).map(([name,entry]) => [name.toLowerCase(),entry]) : []);
const videos = videoProbe(pack("video"));
const layout = (text, width, height, style) => {
  const advance = style ? Math.max(1, Math.round(style.size * 0.55)) : 7;
  const step = style ? (style.lineHeight > 0 ? style.lineHeight : Math.round(style.size * 1.2)) : 13;
  const boxes = new Int16Array(text.length * 4);
  let x = 0, y = 0;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c === "\r" || c === "\n") {
      if (c === "\r" && text[i + 1] === "\n") i++;
      x = 0;
      y += step;
      continue;
    }
    if (c !== " " && x > 0 && style) {
      let end = i;
      while (end < text.length && !" \r\n".includes(text[end])) end++;
      if ((i === 0 || text[i - 1] === " ") && x + (end - i) * advance > width) {
        x = 0;
        y += step;
      }
    }
    boxes.set([x, y, advance, step], i * 4);
    x += advance;
  }
  return boxes;
};
const {default: createDirector64} = await import(pathToFileURL(join(site, "director64.mjs")).href);
const module = await createDirector64({
  director64Host: {
    // The service clock models channel lifetime; this probe emits no sound.
    sound: (...args) => audioEvent("sound", ...args),
    soundPause: (...args) => audioEvent("pause", ...args),
    soundBreakLoop: (...args) => audioEvent("breakLoop", ...args),
    gain: (...args) => audioEvent("gain", ...args),
    flashSound(id, sound) {
      audioEvent("flash", id, sound);
      if (!sound) return;
      validateFlashSound(sound);
      if (!checkedSounds.has(sound.name)) {
        const bytes = audio(sound.name);
        if (!bytes) throw new Error(`missing Flash event sound ${sound.name}`);
        const decoded = isOpusSound(bytes) ? readOpusHeader(bytes) :
          isOggOpus(bytes) ? oggOpusInfo(bytes) : decodeWav(bytes);
        if ((decoded.sourceRate ?? decoded.rate) !== sound.rate ||
            (decoded.sourceFrames ?? decoded.frames) !== sound.frames)
          throw new Error(`Flash event sound metadata mismatch ${sound.name}`);
        checkedSounds.add(sound.name);
      }
    },
    asset: images,
    data,
    video: (...args) => { if (render) videos.update(...args); },
    videoFrame: (slot) => videos.frame(slot),
    videoRevision: () => videos.revision(),
    // Use the converted source duration so waits and cues follow real audio.
    stream: (channel, name) => {
      audioEvent("stream", channel, name);
      if (!name) return 0;
      const entry = streams.get(name.toLowerCase());
      if (!entry) throw new Error(`missing streamed sound ${name}`);
      return entry[2];
    },
    // No fonts here: text lays out on a nominal fixed advance, wrapped as the
    // page wraps it, so text hit tests (pointToWord, hyperlinks) can be driven.
    layout,
    richLayout: (text,width,height,formats) => formatText(text,width,formats,
      (value,style) => value.length*Math.max(1,Math.round((style.size||12)*.55))).boxes,
    richText: (text,width,height,formats) => {
      const boxes=formatText(text,width,formats,(value,style)=>value.length*Math.max(1,Math.round((style.size||12)*.55))).boxes;
      const pixels=new Uint8Array(width*height*4);let run=0;
      for(let i=0;i<text.length;i++) {
        while(run+1<formats.length&&formats[run+1].offset<=i)run++;
        if(/\s/.test(text[i]))continue;
        const [x,y,w,h]=boxes.subarray(i*4,i*4+4),rgb=formats[run].color;
        for(let row=Math.max(0,y);row<Math.min(height,y+h-1);row++)for(let col=Math.max(0,x);col<Math.min(width,x+w-1);col++) {
          const at=(row*width+col)*4;pixels[at]=rgb>>>16;pixels[at+1]=rgb>>>8;pixels[at+2]=rgb;pixels[at+3]=255;
        }
      }
      return pixels;
    },
    // Nominal glyph coverage exercises live text and compositor paths. This
    // probe is a render-error gate; projector/font pixel comparisons use the
    // browser with its actual fonts and explicit image checkpoints.
    text: (text, width, height, style) => {
      const boxes = layout(text, width, height, style);
      const plane = new Uint8Array(width * height);
      for (let i = 0; i < text.length; i++) {
        if (/\s/.test(text[i])) continue;
        const [x, y, w, h] = boxes.subarray(i * 4, i * 4 + 4);
        if (x >= width || y >= height) continue;
        for (let row = Math.max(0, y); row < Math.min(height, y + h - 1); row++)
          plane.fill(255, row * width + Math.max(0, x), row * width + Math.min(width, x + w - 1));
      }
      return plane;
    },
    trace: (text) => process.stderr.write(`NATIVE_TRACE ${text}\n`),
  },
  print: (text) => process.stderr.write(text + "\n"),
  printErr: (text) => process.stderr.write(text + "\n"),
});
if (build.package) {
  const bytes = readFileSync(join(site, build.package.file));
  const pointer = module._malloc(bytes.length);
  module.HEAPU8.set(bytes, pointer);
  const loaded = module._d64_load_package(pointer, bytes.length);
  module._free(pointer);
  if (!loaded) {
    process.stderr.write(`package rejected: ${module.UTF8ToString(module._d64_package_error())}\n`);
    process.exit(3);
  }
}
// A session starts from an erased save image, as a fresh install does.
module.HEAPU8.fill(255, module._d64_flash(), module._d64_flash() + module._d64_flash_bytes());
const out = (text) => new Promise((resolve) => process.stdout.write(text, resolve));
if (render && typeof module._d64_probe_render !== "function") {
  process.stderr.write("runtime lacks rendered RPC support; rebuild the browser runtime\n");
  process.exit(2);
}
module._d64_probe_render?.(render ? 1 : 0);
function checkpoint(context) {
  if (cacheOff) {
    if (typeof module._d64_render_cache !== "function")
      throw new Error("runtime lacks cache comparison support; rebuild the web runtime");
    module._d64_render_cache(0);
  }
  if (render) module._d64_render(0, 0, 0);
  const error = module.UTF8ToString(module._d64_error());
  if (error) process.stderr.write(`runtime failure after ${context}: ${error}\n`);
  const alerts = process.env.DIRECTOR64_PROBE_STRICT && module._d64_script_errors?.();
  if (alerts) process.stderr.write(`script alert after ${context}: ${
    module.UTF8ToString(module._d64_last_script_error())}\n`);
  return !error && !alerts;
}
function observed(reply) {
  // Document operations are host I/O, just like sound completion. Expose only
  // pending-operation metadata; completing one cannot alter arbitrary Lingo.
  const kind = module._d64_user_request_kind();
  const userRequest = kind ? {
    id: module._d64_user_request_id(), kind,
    name: module.UTF8ToString(module._d64_user_name()),
    size: module._d64_user_size(),
  } : null;
  if (userRequest && (kind === 3 || kind === 4)) {
    const pointer = module._d64_user_data();
    const bytes = module.HEAPU8.subarray(pointer, pointer + userRequest.size);
    userRequest.sha256 = createHash("sha256").update(bytes).digest("hex");
    if (bytes[0] === 66 && bytes[1] === 77 && bytes.length >= 54) {
      const dib = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      userRequest.width = dib.getInt32(18, true);
      userRequest.height = dib.getInt32(22, true);
    }
  }
  reply = reply.split("\n").map(line => line.startsWith("{")
    ? JSON.stringify({...JSON.parse(line), user_request: userRequest}) : line).join("\n");
  if (!observations) return reply;
  const width = module._d64_stage_width(), height = module._d64_stage_height();
  const pixels = module.HEAPU8.subarray(module._d64_pixels(),
    module._d64_pixels() + width * height * 4);
  const frameHash = createHash("sha256").update(pixels).digest("hex");
  const trace = [];
  if (typeof module._d64_probe_trace_count !== "function")
    throw new Error("runtime lacks per-tick observations; rebuild the web runtime");
  const count = module._d64_probe_trace_count(), pointer = module._d64_probe_trace_data();
  const view = new DataView(module.HEAPU8.buffer);
  for (let i = 0; i < count; i++)
    trace.push(view.getBigUint64(pointer + i * 8, true).toString(16).padStart(16, "0"));
  return reply.split("\n").map(line => line.startsWith("{")
    ? JSON.stringify({...JSON.parse(line), frame_sha256: frameHash,
      frame_trace: trace, audio_events: audioEvents})
    : line).join("\n");
}
const boot = module.ccall("d64_boot_rpc", "string", ["number"], [42]);
const bootOK = checkpoint("boot");
metrics.end(startup, "boot", module);
if (!bootOK) process.exit(1);
await out(observed(boot));
let status = 0;
for await (const line of createInterface({input: process.stdin, crlfDelay: Infinity})) {
  // The journeys end a session with quit, or with quit-export to keep its
  // save image (DIRECTOR64_FLASH_EXPORT names the file).
  const command = line.trim();
  if (command === "quit" || command === "quit-export") {
    if (command === "quit-export" && process.env.DIRECTOR64_FLASH_EXPORT)
      writeFileSync(process.env.DIRECTOR64_FLASH_EXPORT,
        module.HEAPU8.slice(module._d64_flash(), module._d64_flash() + module._d64_flash_bytes()));
    break;
  }
  const started = metrics.start(line.trimEnd());
  audioEvents.length = 0;
  if (observations) {
    if (typeof module._d64_probe_trace !== "function")
      throw new Error("runtime lacks per-tick observations; rebuild the web runtime");
    module._d64_probe_trace(1);
  }
  let rpc = line.trimEnd();
  if (command === "document cancel" || command === "document complete") {
    const id = module._d64_user_request_id();
    const accepted = command === "document cancel"
      ? module._d64_user_cancel(id) : module._d64_user_complete(id, 1);
    if (!accepted) throw new Error("no matching player document operation is pending");
    rpc = "step 1 0 0 0";
  }
  const reply = module.ccall("d64_rpc", "string", ["string"], [rpc]);
  const ok = reply !== "" && checkpoint(JSON.stringify(line.trimEnd()));
  metrics.end(started, line.trimEnd(), module);
  if (reply === "") { status = 2; break; }
  if (!ok) { status = 1; break; }
  await out(observed(reply));
}
if (!status && module.UTF8ToString(module._d64_error())) status = 1;
process.exit(status);

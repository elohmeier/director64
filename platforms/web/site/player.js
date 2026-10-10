// The player: runs a game package on the shared engine's Wasm runtime at the
// console's 60 Hz service clock, presents its composited stage, plays its
// sounds, reads the mouse, keyboard and controllers, and keeps its saves
// durable in the origin-private file system.
import {AudioEngine} from "./audio.js";
import {PORTS} from "./gamepad.js";
import {OnScreenKeyboard} from "./keyboard.js";
import {FLASH_BYTES, SaveStore, plausibleFlash} from "./saves.js";
import {Presenter, WakeLock} from "./screen.js";
import {t} from "./strings.js";
import {$, ask, hideOverlay, notify, overlay, toast} from "./ui.js";
import {VideoPlayers} from "./video.js";

const WIDTH = 640, HEIGHT = 480;
// Macintosh virtual key codes (Events.h kVK_*) by KeyboardEvent.code.
const MAC_KEYS = {
  KeyA: 0, KeyS: 1, KeyD: 2, KeyF: 3, KeyH: 4, KeyG: 5, KeyZ: 6, KeyX: 7, KeyC: 8, KeyV: 9,
  KeyB: 11, KeyQ: 12, KeyW: 13, KeyE: 14, KeyR: 15, KeyY: 16, KeyT: 17, Digit1: 18, Digit2: 19,
  Digit3: 20, Digit4: 21, Digit6: 22, Digit5: 23, Equal: 24, Digit9: 25, Digit7: 26, Minus: 27,
  Digit8: 28, Digit0: 29, BracketRight: 30, KeyO: 31, KeyU: 32, BracketLeft: 33, KeyI: 34,
  KeyP: 35, Enter: 36, KeyL: 37, KeyJ: 38, Quote: 39, KeyK: 40, Semicolon: 41, Backslash: 42,
  Comma: 43, Slash: 44, KeyN: 45, KeyM: 46, Period: 47, Tab: 48, Space: 49, Backquote: 50,
  Backspace: 51, Escape: 53, NumpadEnter: 76, Home: 115, PageUp: 116, Delete: 117, End: 119,
  PageDown: 121, ArrowLeft: 123, ArrowRight: 124, ArrowDown: 125, ArrowUp: 126,
};
// The console's catch-up rule (platforms/n64/director_main.c): at most four
// ticks per pass and twelve ticks of backlog; time beyond that is dropped
// rather than rushed through.
const MAX_STEPS = 4, BACKLOG_TICKS = 12;

function packLookup({blob, index}) {
  return (name) => {
    const entry = index[name];
    return entry ? blob.subarray(entry[0], entry[0] + entry[1]) : null;
  };
}
// Imported images are stored deflated ("D64Z", compiler/src/convert/files.rs)
// and inflated as the runtime loads them. It asks for an image's length and
// then its bytes, so the last one inflated is kept.
function inflatingLookup(lookup, converter) {
  let last = null, lastBytes = null;
  return (name) => {
    if (name === last) return lastBytes;
    const bytes = lookup(name);
    last = name;
    lastBytes = bytes && converter ? converter.inflate(bytes) : bytes;
    return lastBytes;
  };
}
function deflated(pack) {
  const first = Object.values(pack.index)[0];
  const b = first && pack.blob.subarray(first[0], first[0] + 4);
  return !!b && b[0] === 0x44 && b[1] === 0x36 && b[2] === 0x34 && b[3] === 0x5a;
}

// Text without an authored style prints in the console's builtin monospace
// font, monogram (CC0), at the size whose ascent the console's atlas records
// (11 px, 13 px lines). A styled member (D7+ and extended D6) sets in the
// game's own font, recovered from the disc or the port's substitute, at its
// authored point size, alignment, ascent and line height.
async function textRasterizer(fonts) {
  const face = new FontFace("monogram", "url(monogram.ttf)");
  await face.load();
  document.fonts.add(face);
  const families = new Map();
  for (const name of Object.keys(fonts.index)) {
    const [at, length] = fonts.index[name];
    const family = `d64-font-${name}`;
    try {
      const loaded = await new FontFace(family, fonts.blob.slice(at, at + length)).load();
      document.fonts.add(loaded);
      families.set(Number(name), family);
    } catch (error) {
      console.warn(`font ${name} could not be loaded`, error);
    }
  }
  const scratch = document.createElement("canvas").getContext("2d", {willReadFrequently: true});
  scratch.font = "16px monogram";
  const ascent16 = scratch.measureText("H").fontBoundingBoxAscent || 16;
  const monogram = `${Math.max(1, Math.round((16 * 11) / ascent16))}px monogram`;

  // Director wraps a text member's paragraphs at its box width, on spaces.
  function lines(text, width) {
    const out = [];
    for (const paragraph of text.split(/\r\n|\r|\n/)) {
      let line = "";
      for (const word of paragraph.split(/(?<= )/)) {
        if (line && scratch.measureText(line + word.trimEnd()).width > width) {
          out.push(line);
          line = word;
        } else line += word;
      }
      out.push(line);
    }
    return out;
  }
  return (text, width, height, style) => {
    const surface = scratch.canvas;
    surface.width = width;
    surface.height = height;
    scratch.clearRect(0, 0, width, height);
    scratch.fillStyle = "#fff";
    scratch.textBaseline = "alphabetic";
    const family = style && families.get(style.font);
    if (!style || !family) {
      scratch.font = monogram;
      text.split(/\r\n|\r|\n/).forEach((line, i) => scratch.fillText(line, 0, 12 + i * 13));
    } else {
      scratch.font = `${style.size}px "${family}"`;
      const ascent = style.ascent > 0 ? style.ascent : Math.round(style.size * 0.8);
      const lineHeight = style.lineHeight > 0 ? style.lineHeight : Math.round(style.size * 1.2);
      scratch.textAlign = ["left", "center", "right"][style.align] ?? "left";
      const x = style.align === 1 ? width / 2 : style.align === 2 ? width : 0;
      lines(text, width).forEach((line, i) => scratch.fillText(line.trimEnd(), x, ascent + i * lineHeight));
      scratch.textAlign = "left";
    }
    const pixels = scratch.getImageData(0, 0, width, height).data;
    const coverage = new Uint8Array(width * height);
    if (!style || !family) {
      // A pixel font: coverage is on or off, as the console's 1-bit atlas is.
      for (let i = 0; i < coverage.length; i++) coverage[i] = pixels[i * 4 + 3] >= 128 ? 255 : 0;
    } else {
      for (let i = 0; i < coverage.length; i++) coverage[i] = pixels[i * 4 + 3];
    }
    return coverage;
  };
}

function saveStatus(state) {
  const element = $("save-state");
  element.className = `save-state ${state.kind}`;
  element.textContent = {
    idle: "",
    saving: t("save.saving"),
    saved: state.at ? t("save.saved", {time: state.at.toLocaleTimeString()}) : t("save.saved", {time: ""}).trim(),
    failed: t("save.failed", {message: state.message}),
    unavailable: t("save.unavailable"),
  }[state.kind] ?? "";
  element.title = state.message ?? "";
  // The header is out of sight in fullscreen; a failure must not be.
  if (state.kind === "failed") toast(element.textContent, {error: true, ms: 8000});
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/**
 * profile: the game's profile (profiles.json); runtime: its module URL;
 * game: {package, images: {index, blob}, audio: {index, blob}, fonts: {index, blob}, ...};
 * pads: the page's Gamepads; settings: its Settings; capturesPads(): whether a
 * menu, dialog or the on-screen keyboard reads the pads instead of the game.
 * Returns the running session (see the end of this function).
 */
export async function play({profile, runtime, game, pads, settings, capturesPads}) {
  const canvas = $("screen");
  const presenter = new Presenter(canvas, settings.get("scaling"));
  const wake = new WakeLock();
  const saves = new SaveStore(`director64/${profile.slug}`);
  saves.addEventListener("state", (event) => saveStatus(event.detail));
  const durable = await saves.open();
  const stored = durable ? await saves.read() : null;
  const backup = durable ? await saves.read("flash.bak") : null;
  const audio = new AudioEngine(packLookup(game.audio), 8);
  audio.setVolume(settings.get("volume"), settings.get("muted"));
  // The click or button press that chose to play unlocks audio; without one
  // (a reload straight into play) the context waits for the next.
  const audioLocked = !(await Promise.race([audio.resume().then(() => true), sleep(300).then(() => false)]));
  settings.addEventListener("change", ({detail}) => {
    if (detail.name === "scaling") presenter.setScaling(detail.value);
    if (detail.name === "volume" || detail.name === "muted")
      audio.setVolume(settings.get("volume"), settings.get("muted"));
  });
  // The converter's own inflate, from the importer's module.
  const converter = deflated(game.images)
    ? await (await import("./import/convert.mjs")).loadConverter(
      new Uint8Array(await (await fetch("import/convert.wasm")).arrayBuffer()))
    : null;
  const images = inflatingLookup(packLookup(game.images), converter);
  const streams = game.streams ?? {index: {}, blob: new Uint8Array()};
  const videos = new VideoPlayers(packLookup(game.video ?? {index: {}, blob: new Uint8Array()}), audio);
  const printing = packLookup(game.print ?? {index: {}, blob: new Uint8Array()});
  const data = packLookup(game.data ?? {index: {}, blob: new Uint8Array()});
  const text = await textRasterizer(game.fonts ?? {index: {}, blob: new Uint8Array()});

  const {default: createDirector64} = await import(runtime);
  let module;
  const flash = () => module.HEAPU8.subarray(module._d64_flash(), module._d64_flash() + FLASH_BYTES);
  module = await createDirector64({
    director64Host: {
      asset: images,
      text,
      sound: (channel, name, looping, start, end) => audio.play(channel, name, looping, start, end),
      gain: (channel, gain) => audio.gain(channel, gain),
      // playFile: plays the stream and answers its length in milliseconds.
      stream: (channel, name) => {
        const entry = name && streams.index[name];
        audio.playStream(channel, entry ? streams.blob.subarray(entry[0], entry[0] + entry[1]) : null);
        if (name && !entry) console.warn(`missing stream ${name}`);
        return entry ? entry[2] : 0;
      },
      data,
      video: (sprite, asset, sound, time, rate, serial, gain) =>
        videos.update(sprite, asset, sound, time, rate, serial, gain),
      videoFrame: (sprite) => videos.frame(sprite),
      videoRevision: () => videos.revision,
      saveCommit: (ok) => {
        if (ok) saves.commit(flash().slice());
        else saves.report({kind: "failed", message: t("save.wrote")});
      },
      trace: (line) => console.debug(line),
    },
  });
  if (module._d64_flash_bytes() !== FLASH_BYTES) throw new Error("runtime save device size");
  const pointer = module._malloc(game.package.length);
  module.HEAPU8.set(game.package, pointer);
  const loaded = module._d64_load_package(pointer, game.package.length);
  module._free(pointer);
  if (!loaded) throw new Error(t("game.rejected", {error: module.UTF8ToString(module._d64_package_error())}));
  if (stored) flash().set(stored);
  else flash().fill(255);

  function exportSave() {
    const bytes = flash().slice();
    const link = document.createElement("a");
    link.href = URL.createObjectURL(new Blob([bytes], {type: "application/octet-stream"}));
    link.download = `${profile.slug}.d64save`;
    link.click();
    setTimeout(() => URL.revokeObjectURL(link.href), 10000);
  }
  async function importSave(file) {
    const bytes = new Uint8Array(await file.arrayBuffer());
    if (!plausibleFlash(bytes)) return notify(t("save.invalid"));
    if (!durable) return notify(t("save.noStorage"));
    if (!await ask({title: t("save.replaceTitle"), text: t("save.replaceText"),
      confirm: t("button.replace"), cancel: t("button.cancel")})) return;
    await saves.replace(bytes);
    location.reload();
  }
  $("restore").onclick = async () => {
    await saves.restoreBackup();
    location.reload();
  };
  window.addEventListener("beforeunload", (event) => {
    if (saves.busy) event.preventDefault();
  });

  // ---- Holds: game time and audio stop together while anything holds them
  // (the page hidden, the menu, a print dialog) and resume without replaying
  // the time they were away ----
  const holds = new Set();
  function hold(reason, on) {
    const before = holds.size > 0;
    if (on) holds.add(reason);
    else holds.delete(reason);
    const after = holds.size > 0;
    if (before === after) return;
    mouse.buttons = mouse.pressed = 0;
    releaseKeys();
    if (after) audio.suspend();
    else {
      last = performance.now();
      phase = 0;
      audio.resume();
      pads.swallow(); // the press that closed the menu is not the game's
    }
    wake.want(running && !after);
  }

  // ---- Input: the mouse maps through the letterboxed canvas to the stage ----
  // Buttons as input.h bits: the left button is A, the right one B.
  const mouse = {x: WIDTH / 2, y: HEIGHT / 2, buttons: 0, pressed: 0, inside: false};
  const BUTTON = {0: 1, 2: 2};
  // Controllers drive the pointer from their first use until the mouse moves.
  let usingPads = false;
  function locate(event) {
    const rect = canvas.getBoundingClientRect();
    const scale = Math.min(rect.width / WIDTH, rect.height / HEIGHT);
    const left = rect.left + (rect.width - WIDTH * scale) / 2;
    const top = rect.top + (rect.height - HEIGHT * scale) / 2;
    const x = Math.floor((event.clientX - left) / scale);
    const y = Math.floor((event.clientY - top) / scale);
    mouse.inside = x >= 0 && y >= 0 && x < WIDTH && y < HEIGHT;
    mouse.x = Math.min(WIDTH - 1, Math.max(0, x));
    mouse.y = Math.min(HEIGHT - 1, Math.max(0, y));
  }
  function useMouse(event) {
    // A press that a controller started finishes before the mouse takes over.
    if (usingPads && pads.active) return false;
    usingPads = false;
    locate(event);
    return true;
  }
  canvas.addEventListener("pointermove", (event) => {
    if (event.movementX || event.movementY) useMouse(event);
  });
  canvas.addEventListener("pointerdown", (event) => {
    const bit = BUTTON[event.button];
    if (!bit || !useMouse(event)) return;
    // A click while typing commits the entry and is not read by the game.
    if (entry.sprite) return commitEntry();
    if (bit === 1 && openEntry(false)) {
      event.preventDefault();
      return;
    }
    canvas.setPointerCapture(event.pointerId);
    mouse.buttons |= bit;
    mouse.pressed |= bit; // a click shorter than a tick still reaches the score
  });
  canvas.addEventListener("pointerup", (event) => {
    const bit = BUTTON[event.button];
    if (!bit) return;
    if (!usingPads) locate(event);
    mouse.buttons &= ~bit;
  });
  canvas.addEventListener("lostpointercapture", () => { mouse.buttons = 0; });
  canvas.addEventListener("pointerleave", () => { if (!mouse.buttons && !usingPads) mouse.inside = false; });
  canvas.addEventListener("contextmenu", (event) => event.preventDefault());
  window.addEventListener("blur", () => { mouse.buttons = 0; });
  // Every port the runtime knows of, connected or not.
  function feedPads(state) {
    for (let i = 0; i < PORTS; i++) {
      const port = state.ports[i];
      module._d64_pad(i, port ? 1 : 0, port?.buttons ?? 0, port?.stickX ?? 0, port?.stickY ?? 0);
    }
  }

  // ---- Text entry: a box over the editable field the player pressed ----
  // The console opens its on-screen keyboard there and pauses the game until
  // Start commits the text (platforms/n64/text_input.c). The page does the
  // same with the real keyboard, and shows its own on-screen keyboard to a
  // controller. Field text is windows-1252: ASCII everywhere, German letters
  // on Director 10 (dg_edit_text, which the extended families have).
  const box = $("text-entry");
  const german = profile.port.director_version >= 1000;
  const allowed = german ? /^[\x20-\x21\x23-\x7eÄÖÜßäöü]*$/ : /^[\x20-\x21\x23-\x7e]*$/;
  const entry = {sprite: 0};
  const osk = new OnScreenKeyboard($("osk"), {german, box, onDone: () => commitEntry(), onCancel: () => closeEntry()});
  function cString(pointer) {
    const bytes = module.HEAPU8;
    let end = pointer;
    while (bytes[end]) end++;
    return new TextDecoder("windows-1252").decode(bytes.subarray(pointer, end));
  }
  function openEntry(fromPad) {
    const sprite = module._d64_text_field(mouse.x, mouse.y);
    if (!sprite) return false;
    const rect = module.HEAP32.subarray(module._d64_text_field_rect() / 4, module._d64_text_field_rect() / 4 + 4);
    const bounds = canvas.getBoundingClientRect(), stage = canvas.parentElement.getBoundingClientRect();
    const scale = Math.min(bounds.width / WIDTH, bounds.height / HEIGHT);
    const left = bounds.left - stage.left + (bounds.width - WIDTH * scale) / 2;
    const top = bounds.top - stage.top + (bounds.height - HEIGHT * scale) / 2;
    const height = Math.max(18, (rect[3] - rect[1]) * scale);
    Object.assign(box.style, {
      left: `${left + rect[0] * scale}px`, top: `${top + rect[1] * scale}px`,
      width: `${Math.max(80, (rect[2] - rect[0]) * scale)}px`, height: `${height}px`,
      fontSize: `${Math.min(height * 0.7, 28)}px`,
    });
    entry.sprite = sprite;
    entry.below = (rect[1] + rect[3]) / 2 < HEIGHT / 2;
    box.value = cString(module._d64_text_value(sprite));
    box.setCustomValidity("");
    box.hidden = false;
    box.focus();
    box.select();
    mouse.buttons = mouse.pressed = 0;
    if (fromPad) showKeyboard();
    return true;
  }
  function showKeyboard() {
    // The keyboard appends; a selection would make the first key replace it.
    box.setSelectionRange(box.value.length, box.value.length);
    osk.show(entry.below);
    pads.swallow();
  }
  function closeEntry() {
    entry.sprite = 0;
    box.hidden = true;
    osk.hide();
    pads.swallow();
    canvas.focus?.();
    // The game resumes where it paused, without the time spent typing.
    last = performance.now();
    phase = 0;
  }
  function refuse(message) {
    box.setCustomValidity(message);
    box.reportValidity();
    if (osk.open) toast(message, {error: true});
  }
  function commitEntry() {
    const text = box.value;
    if (!allowed.test(text) || text.length > 20) return refuse(t("text.badCharacter"));
    const bytes = Uint8Array.from(text, (c) => c.charCodeAt(0)); // Latin-1 = windows-1252 here
    const pointer = module._malloc(bytes.length + 1);
    module.HEAPU8.set(bytes, pointer);
    module.HEAPU8[pointer + bytes.length] = 0;
    const accepted = module._d64_edit_text(entry.sprite, pointer);
    module._free(pointer);
    if (accepted) closeEntry();
    else refuse(t("text.refused"));
  }
  box.addEventListener("input", () => box.setCustomValidity(""));
  box.addEventListener("keydown", (event) => {
    event.stopPropagation();
    if (event.key === "Enter") {
      event.preventDefault();
      commitEntry();
    } else if (event.key === "Escape") {
      event.preventDefault();
      closeEntry();
    }
  });

  // ---- The keyboard, for families whose scripts handle keys (D7 and later) ----
  // A desktop projector receives every key: the Macintosh virtual key code of
  // its position and the character the layout types (dg_key). Key codes
  // follow the physical key, as on a Mac; the runtime ignores them in
  // families without keyboard events.
  const held = new Map();
  function character(event) {
    const special = {Enter: 13, Backspace: 8, Tab: 9, Escape: 27, ArrowLeft: 28, ArrowRight: 29,
      ArrowUp: 30, ArrowDown: 31, Delete: 127};
    if (event.key in special) return special[event.key];
    return event.key.length === 1 && event.key.charCodeAt(0) <= 255 ? event.key.charCodeAt(0) : 0;
  }
  function forward(event, down) {
    if (!running || holds.size || entry.sprite || event.ctrlKey || event.metaKey || event.altKey) return;
    if (event.target instanceof HTMLInputElement || event.target.closest?.("dialog, .menu-panel")) return;
    const code = MAC_KEYS[event.code];
    if (code === undefined) return;
    event.preventDefault(); // arrows and space would scroll the page
    if (down) {
      if (event.repeat || held.has(code)) return;
      held.set(code, character(event));
      module._d64_key(code, held.get(code), 1);
    } else if (held.has(code)) {
      module._d64_key(code, held.get(code), 0);
      held.delete(code);
    }
  }
  function releaseKeys() {
    for (const [code, char] of held) module._d64_key(code, char, 0);
    held.clear();
  }
  window.addEventListener("keydown", (event) => forward(event, true));
  window.addEventListener("keyup", (event) => forward(event, false));
  window.addEventListener("blur", releaseKeys);
  // A browser that held audio back until a gesture gets one here; Chrome
  // counts a controller's button press as one too.
  function unlockAudio() {
    if (!holds.size && audio.context.state === "suspended") audio.resume();
  }
  window.addEventListener("pointerdown", unlockAudio);
  window.addEventListener("keydown", unlockAudio);

  // ---- Printing: the browser's print dialog for the game's print pages ----
  // The console shows a QR link to the document and resumes on B
  // (games/loewenzahn-1/runtime/printing.c); the page prints the document
  // itself and then gives the same dismissal: a neutral sample, B, neutral.
  let printed = 0;
  const injected = [];
  const artwork = new Map();
  function imageUrl(name) {
    if (!artwork.has(name)) {
      const bytes = printing(name);
      artwork.set(name, bytes ? URL.createObjectURL(new Blob([bytes], {type: "image/png"})) : "");
    }
    return artwork.get(name);
  }
  function printDocument(index) {
    const documents = JSON.parse(new TextDecoder().decode(printing("documents.json") ?? new Uint8Array()) || "[]");
    const doc = documents[index];
    if (!doc) return Promise.resolve();
    return new Promise((resolve) => {
      const frame = document.createElement("iframe");
      frame.className = "print-frame";
      frame.src = "print.html";
      frame.onload = () => {
        const page = frame.contentDocument;
        const logo = page.getElementById("logo");
        logo.src = imageUrl(doc.logo);
        logo.style.width = `${doc.logo_width}pt`;
        page.getElementById("title").textContent = doc.title;
        page.title = doc.title;
        const body = page.getElementById("body");
        for (const paragraph of doc.body.split("\r\r")) {
          const p = page.createElement("p");
          paragraph.split("\r").forEach((line, i) => {
            if (i) p.append(page.createElement("br"));
            p.append(line);
          });
          body.append(p);
        }
        const illustration = page.getElementById("illustration");
        illustration.hidden = !doc.illustration;
        if (doc.illustration) {
          illustration.src = imageUrl(doc.illustration);
          illustration.style.marginLeft = `${doc.illustration_x}pt`;
        }
        // Print once the artwork has loaded; the dialog blocks until closed.
        Promise.all([...page.images].filter((i) => i.src).map((i) => i.decode().catch(() => {})))
          .then(() => {
            frame.contentWindow.print();
            frame.remove();
            resolve();
          });
      };
      document.body.append(frame);
    });
  }


  // ---- The service loop ----
  let running = false, last = 0, phase = 0, dropped = 0;
  let renders = 0, steps = 0, windowStart = performance.now();
  function present(pointer) {
    if (!pointer) return;
    presenter.present(module.HEAPU8.subarray(pointer, pointer + WIDTH * HEIGHT * 4));
    renders++;
  }
  function stop(status) {
    running = false;
    wake.want(false);
    videos.closeAll();
    audio.stopAll();
    if (status === 2) {
      overlay({heading: t("game.ended"), buttons: ["library"]});
    } else {
      overlay({heading: t("game.stopped"), text: module.UTF8ToString(module._d64_error()),
        detail: profile.save_files.length ? t("game.savesKept") : "", buttons: ["library"], error: true});
    }
  }
  function tick(now) {
    if (!running) return;
    const state = pads.poll(now);
    if (state.actions.length) unlockAudio();
    if (!capturesPads() && !holds.size && !entry.sprite) {
      feedPads(state);
      if (state.active && !usingPads) {
        module._d64_pointer_warp(mouse.x, mouse.y);
        usingPads = true;
      }
    }
    if (usingPads && !state.connected) usingPads = false;
    // A controller reaching a box the mouse opened brings up its keyboard.
    if (entry.sprite && !osk.open && state.actions.length) showKeyboard();
    if (!holds.size && !entry.sprite) {
      // Service time accrues in millionths of a tick, as dg_clock_advance's.
      phase += Math.max(0, now - last) * 1000 * 60;
      last = now;
      if (phase > BACKLOG_TICKS * 1e6) {
        dropped += (phase - BACKLOG_TICKS * 1e6) / 60;
        phase = BACKLOG_TICKS * 1e6;
      }
      const owed = Math.min(MAX_STEPS, Math.floor(phase / 1e6));
      phase -= owed * 1e6;
      for (let i = 0; i < owed; i++) {
        let status;
        if (injected.length) {
          status = module._d64_step(mouse.x, mouse.y, injected.shift());
        } else if (usingPads) {
          status = module._d64_step_pads();
          mouse.x = module._d64_pointer_x();
          mouse.y = module._d64_pointer_y();
          mouse.inside = true;
          if (status === 3) {
            openEntry(true);
            break;
          }
        } else {
          status = module._d64_step(mouse.x, mouse.y, mouse.buttons | mouse.pressed);
        }
        mouse.pressed = 0;
        const request = module._d64_print_request();
        if (request && request !== printed && !injected.length) {
          printed = request;
          hold("print", true);
          printDocument(request - 1).then(() => {
            injected.push(0, 2, 0);
            hold("print", false);
          });
          break;
        } else if (!request) printed = 0;
        steps++;
        if (status) return stop(status);
      }
      present(usingPads ? module._d64_render_pads()
        : module._d64_render(mouse.x, mouse.y, mouse.inside ? 1 : 0));
      if (module.UTF8ToString(module._d64_error())) return stop(1);
    }
    if (now - windowStart >= 1000) {
      const seconds = (now - windowStart) / 1000;
      $("diagnostics").textContent =
        `${module.UTF8ToString(module._d64_movie())} frame ${module._d64_frame()} · ` +
        `${(steps / seconds).toFixed(0)} ticks/s · ${(renders / seconds).toFixed(0)} redraws/s · ` +
        `images ${(module._d64_image_bytes() / 1048576).toFixed(0)} MB · ` +
        `dropped ${(dropped / 1e6).toFixed(1)} s · alerts ${module._d64_script_errors()}` +
        (usingPads ? ` · pad ${module._d64_pointer_player() + 1}/${state.connected}` : "");
      steps = renders = 0;
      windowStart = now;
    }
    requestAnimationFrame(tick);
  }
  document.addEventListener("visibilitychange", () => {
    if (running) hold("hidden", document.hidden);
  });

  const seed = crypto.getRandomValues(new Uint32Array(1))[0];
  const status = module._d64_boot(seed);
  const error = module.UTF8ToString(module._d64_error());
  if (error) {
    overlay({heading: t("game.stopped"), text: error, error: true,
      buttons: backup ? ["restore", "library"] : ["library"]});
    return null;
  }
  if (backup) await saves.discardBackup(); // the imported save loaded
  console.info(`save status ${status}, generation ${module._d64_save_generation()}`);
  hideOverlay();
  // The Play button keeps focus otherwise, and its Space and Enter with it.
  if (document.activeElement instanceof HTMLElement) document.activeElement.blur();
  pads.swallow(); // the button that chose Play
  running = true;
  wake.want(!document.hidden);
  if (document.hidden) hold("hidden", true);
  last = performance.now();
  phase = 0;
  present(module._d64_render(mouse.x, mouse.y, 0));
  requestAnimationFrame(tick);
  if (audioLocked) toast(t("audio.locked"), {ms: 6000});
  window.director64 = {...window.director64, module, audio, saves,
    rpc: (command) => module.ccall("d64_rpc", "string", ["string"], [command])};

  // What the page's menu works with.
  return {
    hasSaves: profile.save_files.length > 0,
    durable,
    hold,
    exportSave,
    importSave,
    get running() { return running; },
    get typing() { return osk.open; },
    keyboard: osk,
    // Waits for a save still being written.
    async settle() {
      while (saves.busy) await sleep(50);
    },
  };
}

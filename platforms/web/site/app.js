// The standalone player's shell: one game, converted from the user's own disc
// by `director64 web`, served beside this page. It loads the game's package
// and asset packs (build.json names them), then plays it (player.js).
import {Gamepads} from "./gamepad.js";
import {play} from "./player.js";
import {toggleFullscreen} from "./screen.js";
import {Settings} from "./settings.js";
import {t, translatePage} from "./strings.js";
import {$, navigate, overlay, toast, trackInputDevice} from "./ui.js";

async function json(url) {
  const response = await fetch(url, {cache: "no-cache"});
  if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
  return response.json();
}
async function bytes(url) {
  const response = await fetch(url, {cache: "no-cache"});
  if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
  return new Uint8Array(await response.arrayBuffer());
}

const settings = new Settings();
const pads = new Gamepads();
let session = null; // the running game (player.js), once one plays

function setHeader(profile) {
  document.title = profile.title;
  $("title").textContent = profile.title;
  $("edition").textContent = `${profile.source} · D${profile.port.director_version / 100}`;
}
function showDiagnostics() {
  const on = settings.diagnostics;
  $("diagnostics").hidden = !on;
  $("edition").hidden = !on;
  $("diagnostics-toggle").checked = on;
}

// What playing needs; checked up front so an unsupported browser is told so.
function missingFeatures() {
  const checks = [
    ["feature.opfs", typeof navigator.storage?.getDirectory === "function"],
    ["feature.writable", typeof FileSystemFileHandle === "function"
      && "createWritable" in FileSystemFileHandle.prototype],
    ["feature.wasm", typeof WebAssembly === "object"],
  ];
  return checks.filter(([, ok]) => !ok).map(([key]) => t(key));
}

// ---- The menu: sound, display, saves and controls, over the stage ----
const menu = {
  get open() { return !$("menu").hidden; },
  show() {
    if (this.open) return;
    const playing = !!session?.running;
    $("menu-game").hidden = !playing;
    $("menu-saves").hidden = !(playing && session.hasSaves);
    $("export").disabled = $("save-import").disabled = !(playing && session.hasSaves);
    session?.hold("menu", true);
    $("menu").hidden = false;
    $("menu-button").setAttribute("aria-expanded", "true");
    this.returnFocus = document.activeElement;
    (playing ? $("menu-resume") : $("volume")).focus();
    pads.swallow();
    // Report whether the browser may evict saves under storage pressure.
    navigator.storage?.persisted?.().then((persisted) => { $("saves-evictable").hidden = persisted; })
      .catch(() => {});
  },
  close() {
    if (!this.open) return;
    $("menu").hidden = true;
    $("menu-button").setAttribute("aria-expanded", "false");
    if (this.returnFocus instanceof HTMLElement && this.returnFocus !== document.body) this.returnFocus.focus();
    else document.activeElement?.blur?.();
    session?.hold("menu", false);
    pads.swallow();
  },
  toggle() { this.open ? this.close() : this.show(); },
};

function setupMenu() {
  const volume = $("volume"), mute = $("mute");
  volume.value = String(Math.round(settings.get("volume") * 100));
  mute.checked = settings.get("muted");
  volume.addEventListener("input", () => {
    settings.set("volume", Number(volume.value) / 100);
    if (settings.get("muted") && Number(volume.value) > 0) {
      mute.checked = false;
      settings.set("muted", false);
    }
  });
  mute.addEventListener("change", () => settings.set("muted", mute.checked));
  const scaling = () => {
    $("scaling-sharp").setAttribute("aria-pressed", String(settings.get("scaling") === "sharp"));
    $("scaling-smooth").setAttribute("aria-pressed", String(settings.get("scaling") === "smooth"));
  };
  $("scaling-sharp").onclick = () => { settings.set("scaling", "sharp"); scaling(); };
  $("scaling-smooth").onclick = () => { settings.set("scaling", "smooth"); scaling(); };
  scaling();
  $("diagnostics-toggle").addEventListener("change", (event) => {
    settings.set("diagnostics", event.target.checked);
    showDiagnostics();
  });
  showDiagnostics();
  $("menu-resume").onclick = () => menu.close();
  $("export").onclick = () => session?.exportSave();
  $("save-import").onchange = (event) => {
    const file = event.target.files[0];
    event.target.value = "";
    if (file && session) session.importSave(file).catch(fail);
  };
  $("menu-button").onclick = () => menu.toggle();
  $("stage-menu").onclick = () => menu.show();
  // A click on the dimmed stage around the menu closes it.
  $("menu").addEventListener("click", (event) => { if (event.target === $("menu")) menu.close(); });
  window.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && menu.open) {
      // The game, listening after this, does not get the key that closed it.
      event.preventDefault();
      event.stopImmediatePropagation();
      menu.close();
    }
  });
}

// ---- Fullscreen ----
async function fullscreen() {
  try {
    await toggleFullscreen($("stage"));
  } catch {
    toast(t("fullscreen.unavailable"), {error: true});
  }
}
function setupFullscreen() {
  $("fullscreen").onclick = fullscreen;
  $("menu-fullscreen").onclick = fullscreen;
  let idle = 0;
  const label = () => {
    const on = !!document.fullscreenElement;
    for (const id of ["fullscreen", "menu-fullscreen"]) {
      $(id).textContent = t(on ? "button.exitFullscreen" : "button.fullscreen");
      $(id).setAttribute("aria-pressed", String(on));
    }
    $("stage-menu").hidden = true;
  };
  document.addEventListener("fullscreenchange", label);
  // In fullscreen the header is gone; a menu button shows while the mouse moves.
  $("stage").addEventListener("pointermove", () => {
    if (!document.fullscreenElement) return;
    $("stage-menu").hidden = false;
    clearTimeout(idle);
    idle = setTimeout(() => { $("stage-menu").hidden = true; }, 2500);
  });
  label();
}

// ---- Controllers: menus read them; the running game reads them otherwise ----
function topLayer() {
  if ($("dialog").open) return {element: $("dialog"), back: () => $("dialog").close("cancel")};
  if (session?.typing) return {keyboard: session.keyboard};
  if (menu.open) return {element: $("menu"), back: () => menu.close()};
  if (!$("overlay").hidden) return {element: $("overlay"), back: null};
  return null;
}
const capturesPads = () => topLayer() !== null;
function padLoop(now) {
  const state = pads.poll(now);
  const layer = topLayer();
  const markPad = state.actions.length > 0;
  for (const action of state.actions) {
    if (layer?.keyboard) {
      layer.keyboard.action(action);
    } else if (layer) {
      if (action === "back" || action === "menu") layer.back?.();
      else navigate(layer.element, action === "start" ? "accept" : action);
    } else if (action === "menu" && session?.running) {
      menu.show();
    }
    if (topLayer() !== layer) break; // what opened or closed takes the rest
  }
  if (markPad) usePad();
  requestAnimationFrame(padLoop);
}
let usePad = () => {};

async function main() {
  translatePage();
  usePad = trackInputDevice();
  setupMenu();
  setupFullscreen();
  pads.addEventListener("connected", () => toast(t("pad.connected")));
  pads.addEventListener("disconnected", () => toast(t("pad.disconnected")));
  requestAnimationFrame(padLoop);
  $("reload").onclick = () => location.reload();
  const missing = missingFeatures();
  if (missing.length) {
    overlay({heading: t("unsupported.heading"), error: true,
      text: t("unsupported.text", {features: missing.join(", ")})});
    return;
  }
  const build = await json("build.json");
  const profile = build.profile;
  setHeader(profile);
  overlay({heading: profile.title, text: t("loading"), progress: 0});
  const kinds = Object.keys(build.packs);
  let loaded = 0;
  const packs = await Promise.all(kinds.map(async (kind) => {
    const [blob, index] = await Promise.all([bytes(build.packs[kind].file), json(build.packs[kind].index)]);
    overlay({heading: profile.title, text: t("loading"), progress: ++loaded / kinds.length});
    return [kind, {blob, index}];
  }));
  const game = {package: await bytes(build.package.file), ...Object.fromEntries(packs)};
  // Audio starts with a click or a button press: the game waits for one.
  $("start").onclick = async () => {
    try {
      overlay({heading: profile.title, text: t("starting")});
      session = await play({profile, runtime: new URL("director64.mjs", location.href).href, game,
        pads, settings, capturesPads});
    } catch (error) {
      fail(error);
    }
  };
  overlay({heading: profile.title, buttons: ["start"]});
  window.director64 = {build, settings, pads};
}

function fail(error) {
  console.error(error);
  overlay({heading: t("fail.heading"), text: String(error?.message ?? error), error: true,
    buttons: ["reload"]});
}

main().catch((error) => {
  console.error(error);
  overlay({heading: t("start.failed"), text: String(error.message || error),
    buttons: ["reload"], error: true});
});

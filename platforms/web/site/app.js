// The browser player's shell. A game is never downloaded: the player imports
// it from the user's own disc image, converting it locally in a worker
// (import/importer.js), and keeps the result in the origin-private file
// system so later visits start at once. Nothing of the disc leaves the
// machine; this site serves only the engine, the converter and profiles.
import {PackageCache} from "./cache.js";
import {Gamepads} from "./gamepad.js";
import {play} from "./player.js";
import {toggleFullscreen} from "./screen.js";
import {Settings} from "./settings.js";
import {t, translatePage} from "./strings.js";
import {$, ask, megabytes, navigate, overlay, toast, trackInputDevice} from "./ui.js";

// The importer's stages, as the player hears of them.
const STAGE_TEXT = {
  tools: "stage.check", verify: "stage.check",
  extract: "stage.convert", parse: "stage.convert", analyze: "stage.convert", audit: "stage.convert",
  scores: "stage.convert", port: "stage.convert", compile: "stage.convert", package: "stage.convert",
  convert: "stage.media",
  sound: "stage.encode", video: "stage.encode",
};

async function json(url) {
  const response = await fetch(url, {cache: "no-cache"});
  if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
  return response.json();
}
async function bytes(url) {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
  return new Uint8Array(await response.arrayBuffer());
}
const absolute = (path) => new URL(path, location.href).href;

const settings = new Settings();
const pads = new Gamepads();
let session = null; // the running game (player.js), once one plays

function setHeader(profile) {
  document.title = profile ? `${profile.title} — Director64` : "Director64";
  $("title").textContent = profile ? profile.title : "";
  $("edition").textContent = profile
    ? `${profile.source.id} · D${profile.port.director_version / 100}` : "";
}
function showDiagnostics() {
  const on = settings.diagnostics;
  $("diagnostics").hidden = !on;
  $("edition").hidden = !on;
  $("diagnostics-toggle").checked = on;
}

// What importing and playing need; checked up front so an unsupported
// browser is told so instead of failing halfway through an import.
function missingFeatures() {
  const checks = [
    ["feature.audioEncoder", typeof AudioEncoder === "function"],
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
    $("menu-library").hidden = !playing;
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
  $("menu-library").onclick = backToLibrary;
  $("library-button").onclick = backToLibrary;
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

async function backToLibrary() {
  await session?.settle();
  location.reload();
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
  const missing = missingFeatures();
  if (missing.length) {
    overlay({heading: t("unsupported.heading"), error: true,
      text: t("unsupported.text", {features: missing.join(", ")})});
    return;
  }
  const [build, profiles] = await Promise.all([json("build.json"), json("profiles.json")]);
  // A development server also offers the games built by the native pipeline;
  // it marks its responses, so a static host is never asked for the list.
  const devServer = await fetch("build.json", {method: "HEAD", cache: "no-cache"})
    .then((r) => r.headers.has("X-Director64-Local")).catch(() => false);
  const localBuild = devServer
    ? await fetch("local/local.json", {cache: "no-cache"})
      .then((r) => (r.ok ? r.json() : null)).catch(() => null)
    : null;
  $("reload").onclick = () => location.reload();
  // One cache per game: a converted game is keyed by its edition and the
  // converter revision that made it.
  const caches = new Map();
  let durable = true;
  for (const profile of profiles) {
    const cache = new PackageCache(profile.slug, `${profile.source.id}-${build.import.revision.slice(0, 16)}`);
    durable = (await cache.open()) && durable;
    await cache.prune().catch(() => {});
    caches.set(profile.slug, cache);
  }
  const runtimeOf = (profile) => absolute(build.runtime.profiles[profile.slug].module);

  async function start(profile, game) {
    setHeader(profile);
    overlay({heading: profile.title, text: t("starting")});
    session = await play({profile, runtime: runtimeOf(profile), game, pads, settings, capturesPads});
  }

  function button(label, action, primary = false) {
    const element = document.createElement("button");
    element.type = "button";
    element.textContent = label;
    if (primary) element.className = "primary";
    element.onclick = () => action().catch(fail);
    return element;
  }

  // Every supported game: play what was converted, or choose a disc.
  async function home() {
    setHeader(null);
    const rows = [];
    for (const profile of profiles) {
      const cache = caches.get(profile.slug);
      const cached = durable ? await cache.load().catch(() => null) : null;
      const local = localBuild?.[profile.slug];
      const row = document.createElement("li");
      const name = document.createElement("span");
      name.className = "game";
      name.textContent = profile.title;
      const status = document.createElement("span");
      status.className = "status";
      status.textContent = cached
        ? t("library.ready", {date: new Date(cached.manifest.created).toLocaleDateString()})
        : t("library.needs", {file: profile.source.file, size: megabytes(profile.source.bytes)});
      const actions = document.createElement("span");
      actions.className = "actions";
      if (cached) {
        actions.append(button(t("button.play"), () => start(profile, cached), true));
        actions.append(button(t("button.remove"), async () => {
          if (!await ask({title: t("remove.title", {title: profile.title}), text: t("remove.text"),
            confirm: t("button.remove"), cancel: t("button.cancel")})) return;
          await cache.remove();
          await home();
        }));
      }
      if (local)
        actions.append(button(t("button.local"), async () => {
          overlay({heading: profile.title, text: t("loadingLocal")});
          const packs = await Promise.all(Object.entries(local.packs).map(async ([kind, [blob, index]]) =>
            [kind, {blob: await bytes(blob), index: await json(index)}]));
          await start(profile, {package: await bytes(local.package), ...Object.fromEntries(packs)});
        }));
      row.append(name, status, actions);
      rows.push(row);
    }
    $("library").replaceChildren(...rows);
    overlay({
      heading: "Director64",
      text: t("home.text"),
      detail: durable ? "" : t("home.noStorage"),
      buttons: ["choose"],
      library: true,
    });
  }

  function editions() {
    return [t("import.editions"),
      ...profiles.map((p) => `• ${p.title}: ${p.source.file} (${megabytes(p.source.bytes)})`)].join("\n");
  }

  function importDisc(file) {
    const worker = new Worker("import/importer.js");
    const started = performance.now();
    let profile = null;
    const seconds = () => ((performance.now() - started) / 1000).toFixed(0);
    $("cancel").onclick = () => {
      worker.terminate(); // nothing was stored; the cache only takes finished imports
      home().catch(fail);
    };
    worker.onmessage = async ({data}) => {
      if (data.type === "identified") {
        profile = profiles.find((p) => p.slug === data.slug);
        setHeader(profile);
      } else if (data.type === "progress") {
        const fraction = data.stages > 1 ? (data.index + (data.fraction ?? 0)) / data.stages : null;
        overlay({heading: profile ? t("import.heading", {title: profile.title}) : t("import.headingUnknown"),
          text: t(STAGE_TEXT[data.stage] ?? "stage.convert"),
          progress: fraction, buttons: ["cancel"], detail: `${seconds()} s`});
      } else if (data.type === "error") {
        worker.terminate();
        if (data.reason === "edition") {
          overlay({heading: t("import.wrongDisc"), text: editions(), detail: data.message, error: true,
            buttons: ["choose", "reload"]});
        } else {
          overlay({heading: t("import.failed"), text: data.message, error: true, buttons: ["choose", "reload"]});
        }
      } else if (data.type === "done") {
        worker.terminate();
        const game = {package: data.package, ...data.packs};
        console.info("import report", data.report);
        overlay({heading: t("import.heading", {title: profile.title}), text: t("stage.store"), progress: 1,
          detail: `${seconds()} s`});
        let note = "";
        try {
          await caches.get(profile.slug).store(game, data.report);
          // Kept games and saves should survive storage pressure.
          navigator.storage.persist?.().catch(() => {});
        } catch (error) {
          note = error?.name === "QuotaExceededError"
            ? t("import.quota") : t("import.notKept", {error: error?.message ?? error});
        }
        $("start").onclick = () => start(profile, game).catch(fail);
        overlay({heading: profile.title, text: t("import.ready", {seconds: seconds()}), detail: note,
          buttons: ["start"]});
      }
    };
    worker.onerror = (event) => {
      worker.terminate();
      fail(new Error(event.message || t("import.stopped")));
    };
    worker.postMessage({
      type: "import",
      file,
      profiles,
      tools: Object.fromEntries(Object.entries(build.import.tools).map(([key, path]) => [key, absolute(path)])),
      meta: {schema_version: 1, converter: build.revision},
    });
  }

  $("disc-input").onchange = (event) => {
    const file = event.target.files[0];
    event.target.value = "";
    if (file) importDisc(file);
  };
  window.director64 = {build, profiles, caches, settings, pads};
  await home();
}

function fail(error) {
  console.error(error);
  overlay({heading: t("fail.heading"), text: String(error?.message ?? error), error: true,
    buttons: session?.running ? ["library"] : ["choose", "reload"]});
}

main().catch((error) => {
  console.error(error);
  overlay({heading: t("start.failed"), text: String(error.message || error),
    buttons: ["reload"], error: true});
});

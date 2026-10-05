// The browser player's shell. A game is never downloaded: the player imports
// it from the user's own disc image, converting it locally in a worker
// (import/importer.js), and keeps the result in the origin-private file
// system so later visits start at once. Nothing of the disc leaves the
// machine; this site serves only the engine, the converter and profiles.
import {PackageCache} from "./cache.js";
import {play} from "./player.js";
import {$, megabytes, overlay} from "./ui.js";

const STAGE_TEXT = {
  tools: "Loading the converter",
  verify: "Checking the disc image",
  extract: "Reading the disc",
  parse: "Parsing the Director files",
  analyze: "Recovering scripts and media",
  audit: "Accounting for every source file",
  scores: "Recovering the scores",
  convert: "Converting images and sounds",
  port: "Applying the port's corrections",
  compile: "Compiling the scripts",
  package: "Building the game package",
  sound: "Encoding the sound",
  video: "Encoding the video",
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

function setHeader(profile) {
  document.title = profile ? `${profile.title} — Director64` : "Director64";
  $("title").textContent = profile ? profile.title : "";
  $("edition").textContent = profile
    ? `${profile.source.id} · D${profile.port.director_version / 100}` : "";
}

// What importing and playing need; checked up front so an unsupported
// browser is told so instead of failing halfway through an import.
function missingFeatures() {
  const checks = [
    ["WebCodecs audio encoding", typeof AudioEncoder === "function"],
    ["the origin-private file system", typeof navigator.storage?.getDirectory === "function"],
    ["writable file handles", typeof FileSystemFileHandle === "function"
      && "createWritable" in FileSystemFileHandle.prototype],
    ["WebAssembly", typeof WebAssembly === "object"],
  ];
  return checks.filter(([, ok]) => !ok).map(([name]) => name);
}

async function main() {
  const missing = missingFeatures();
  if (missing.length) {
    overlay({heading: "This browser is not supported", error: true,
      text: `Director64 needs ${missing.join(", ")}. Use a current desktop Chrome or Edge.`});
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
  $("fullscreen").onclick = () => $("stage").requestFullscreen?.();
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
    overlay({heading: profile.title, text: "Starting…", progress: null});
    await play({profile, runtime: runtimeOf(profile), game});
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
        ? `converted ${new Date(cached.manifest.created).toLocaleDateString()}`
        : `needs ${profile.source.file} (${megabytes(profile.source.bytes)})`;
      const actions = document.createElement("span");
      actions.className = "actions";
      if (cached) {
        actions.append(button("Play", () => start(profile, cached), true));
        actions.append(button("Remove", async () => {
          if (!confirm(`Remove the converted ${profile.title} from this browser? Its saves stay; importing the disc again restores it.`)) return;
          await cache.remove();
          await home();
        }));
      }
      if (local)
        actions.append(button("Local build", async () => {
          overlay({heading: profile.title, text: "Loading the local build…", progress: null});
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
      text: "Choose a disc image of a supported game. It is converted here, in this browser; nothing is uploaded.",
      detail: durable ? "" : "This browser keeps no durable storage: a game is converted again on every visit, and saves are lost.",
      buttons: ["choose"],
      library: true,
    });
  }

  function fail(error) {
    console.error(error);
    overlay({heading: "Something went wrong", text: String(error?.message ?? error), error: true,
      buttons: ["choose", "reload"]});
  }

  function importDisc(file) {
    const worker = new Worker("import/importer.js");
    const started = performance.now();
    let profile = null;
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
        overlay({heading: profile ? `Importing ${profile.title}` : "Importing", text: STAGE_TEXT[data.stage] ?? data.stage,
          progress: fraction, buttons: ["cancel"],
          detail: `${((performance.now() - started) / 1000).toFixed(0)} s`});
      } else if (data.type === "error") {
        worker.terminate();
        overlay({heading: data.kind === "unsupported" ? "This disc image is not supported" : "The import failed",
          text: data.message, error: true, buttons: ["choose", "reload"]});
      } else if (data.type === "done") {
        worker.terminate();
        const game = {package: data.package, ...data.packs};
        const seconds = ((performance.now() - started) / 1000).toFixed(0);
        console.info("import report", data.report);
        overlay({heading: `Importing ${profile.title}`, text: "Saving for next time", progress: 1,
          detail: `${seconds} s`});
        let note = "";
        try {
          await caches.get(profile.slug).store(game, data.report);
        } catch (error) {
          note = error?.name === "QuotaExceededError"
            ? "Not enough browser storage to keep the converted game; it plays from memory this time."
            : `The converted game could not be kept (${error?.message ?? error}); it plays from memory this time.`;
        }
        $("start").onclick = () => start(profile, game).catch(fail);
        overlay({heading: profile.title, text: `Converted in ${seconds} s. Ready to play.`, detail: note,
          buttons: ["start"]});
      }
    };
    worker.onerror = (event) => {
      worker.terminate();
      fail(new Error(event.message || "the importer stopped"));
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
  window.director64 = {build, profiles, caches};
  await home();
}

main().catch((error) => {
  console.error(error);
  overlay({heading: "The player could not start", text: String(error.message || error),
    buttons: ["reload"], error: true});
});

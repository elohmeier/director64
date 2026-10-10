// The player's preferences, kept per browser: volume, scaling and whether the
// diagnostics line shows. `?debug` shows diagnostics for one visit.
const KEY = "director64/settings";
export const DEFAULTS = {volume: 1, muted: false, scaling: "sharp", diagnostics: false};

export function readSettings(storage = globalThis.localStorage) {
  let stored = {};
  try {
    stored = JSON.parse(storage?.getItem(KEY) ?? "{}") ?? {};
  } catch {
    stored = {};
  }
  const settings = {...DEFAULTS};
  if (Number.isFinite(stored.volume)) settings.volume = Math.min(1, Math.max(0, stored.volume));
  if (typeof stored.muted === "boolean") settings.muted = stored.muted;
  if (stored.scaling === "smooth" || stored.scaling === "sharp") settings.scaling = stored.scaling;
  if (typeof stored.diagnostics === "boolean") settings.diagnostics = stored.diagnostics;
  return settings;
}

export class Settings extends EventTarget {
  constructor(storage = globalThis.localStorage) {
    super();
    this.storage = storage;
    this.values = readSettings(storage);
    this.debug = new URLSearchParams(globalThis.location?.search ?? "").has("debug");
  }
  get(name) { return this.values[name]; }
  get diagnostics() { return this.debug || this.values.diagnostics; }
  set(name, value) {
    this.values[name] = value;
    try {
      this.storage?.setItem(KEY, JSON.stringify(this.values));
    } catch {
      // Storage may be unavailable; the setting still holds for this visit.
    }
    this.dispatchEvent(new CustomEvent("change", {detail: {name, value}}));
  }
}

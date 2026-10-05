// The page's shared surface: element lookup and the stage overlay, which
// shows every state that is not the running game.
export const $ = (id) => document.getElementById(id);

const BUTTONS = ["start", "choose", "cancel", "reload", "restore"];

// `library` shows the list of supported games (app.js fills it).
export function overlay({heading, text = "", progress = null, buttons = [], error = false, detail = "",
  library = false}) {
  $("overlay").hidden = false;
  $("overlay").classList.toggle("error", error);
  $("overlay-heading").textContent = heading;
  $("overlay-text").textContent = text;
  $("overlay-detail").textContent = detail;
  $("progress").hidden = progress === null;
  if (progress !== null) $("progress").value = progress;
  for (const id of BUTTONS) $(id).hidden = !buttons.includes(id);
  $("library").hidden = !library;
}

export function hideOverlay() {
  $("overlay").hidden = true;
}

export function megabytes(bytes) {
  return `${(bytes / 1048576).toFixed(0)} MB`;
}

// The page's shared surface: element lookup, the stage overlay that shows
// every state that is not the running game, short notices, the in-page
// dialog, and moving focus with a controller.
import {t} from "./strings.js";

export const $ = (id) => document.getElementById(id);

// Overlay buttons by name; "library" goes back to the list of games.
const BUTTONS = {start: "start", choose: "choose", cancel: "cancel", reload: "reload",
  library: "library-button", restore: "restore"};

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
  for (const [name, id] of Object.entries(BUTTONS)) $(id).hidden = !buttons.includes(name);
  $("library").hidden = !library;
  // A controller starts on the first choice offered.
  if (document.body.classList.contains("pad") && !$("overlay").contains(document.activeElement))
    focusables($("overlay"))[0]?.focus();
}

export function hideOverlay() {
  $("overlay").hidden = true;
}

export function megabytes(bytes) {
  return `${(bytes / 1048576).toFixed(0)} MB`;
}

// A short notice over the stage, visible in fullscreen too.
let toastTimer = 0;
export function toast(text, {error = false, ms = 4000} = {}) {
  const element = $("toast");
  element.textContent = text;
  element.classList.toggle("error", error);
  element.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { element.hidden = true; }, ms);
}

// A question or notice in the page rather than the browser's own dialog,
// which would end fullscreen. Resolves true for the confirming choice.
export function ask({title, text = "", confirm = t("button.ok"), cancel = null}) {
  const dialog = $("dialog");
  $("dialog-title").textContent = title;
  $("dialog-text").textContent = text;
  $("dialog-confirm").textContent = confirm;
  $("dialog-cancel").hidden = cancel === null;
  if (cancel !== null) $("dialog-cancel").textContent = cancel;
  dialog.returnValue = "";
  $("dialog-confirm").onclick = () => dialog.close("confirm");
  $("dialog-cancel").onclick = () => dialog.close("cancel");
  dialog.showModal();
  (cancel === null ? $("dialog-confirm") : $("dialog-cancel")).focus();
  return new Promise((resolve) => {
    dialog.addEventListener("close", () => resolve(dialog.returnValue === "confirm"), {once: true});
  });
}
export const notify = (title, text = "") => ask({title, text});

// ---- Controller focus ----
const FOCUSABLE = "button, input, label.file, [data-pad]";
const shown = (element) => element.getClientRects().length > 0 && !element.closest("[hidden]");

export function focusables(container) {
  return [...container.querySelectorAll(FOCUSABLE)].filter((element) =>
    !element.disabled && shown(element) && !(element.type === "file"));
}

// One navigation action within a container: directions move focus in
// document order (left and right adjust a slider), accept activates.
export function navigate(container, action) {
  const items = focusables(container);
  if (!items.length) return false;
  const index = items.indexOf(document.activeElement);
  const current = items[index];
  if (index < 0) {
    // The first press lands on the first choice; accept also takes it.
    items[0].focus();
    if (action === "accept") items[0].click();
    return true;
  }
  if (current?.type === "range" && (action === "left" || action === "right")) {
    current.value = Number(current.value) + (action === "left" ? -1 : 1) * Number(current.step || 1);
    current.dispatchEvent(new Event("input", {bubbles: true}));
    return true;
  }
  if (action === "up" || action === "left") {
    items[(Math.max(index, 0) + items.length - 1) % items.length].focus();
    return true;
  }
  if (action === "down" || action === "right") {
    items[(index + 1) % items.length].focus();
    return true;
  }
  if (action === "accept" && current) {
    current.click();
    return true;
  }
  return false;
}

// Focus rings follow whichever device was used last.
export function trackInputDevice() {
  const body = document.body;
  window.addEventListener("pointermove", () => body.classList.remove("pad"), {passive: true});
  window.addEventListener("pointerdown", () => body.classList.remove("pad"), {passive: true});
  window.addEventListener("keydown", () => body.classList.remove("pad"), {passive: true});
  return () => body.classList.add("pad");
}

// The on-screen keyboard a controller types with, as the console's
// (platforms/n64/text_input.c): it fills the page's text box over the field,
// and the box commits as it does for a real keyboard. Typing on a real
// keyboard keeps working while it shows.
import {t} from "./strings.js";

const COLUMNS = 10;

// Rows of keys; each key spans columns [col, col + span).
export function keyRows(german) {
  const chars = (text) => [...text].map((c) => ({char: c}));
  const third = german ? chars("UVWXYZÄÖÜß") : chars("UVWXYZ_!?'");
  const rows = [chars("ABCDEFGHIJ"), chars("KLMNOPQRST"), third, chars("1234567890"),
    [{action: "shift"}, {char: "-"}, {char: "."}, {action: "space", span: 3}, {action: "delete", span: 2},
      {action: "done", span: 2}]];
  for (const row of rows) {
    let col = 0;
    for (const key of row) {
      key.span ??= 1;
      key.col = col;
      col += key.span;
    }
    if (col !== COLUMNS) throw new Error("keyboard row width");
  }
  return rows;
}

// The key a move lands on: left and right wrap within the row; up and down
// keep the column under the current key's centre.
export function moveKey(rows, [row, index], direction) {
  if (direction === "left" || direction === "right") {
    const count = rows[row].length;
    return [row, (index + (direction === "left" ? count - 1 : 1)) % count];
  }
  const key = rows[row][index];
  const centre = key.col + key.span / 2;
  const next = (row + (direction === "up" ? rows.length - 1 : 1)) % rows.length;
  const target = rows[next].findIndex((k) => centre >= k.col && centre < k.col + k.span);
  return [next, target < 0 ? rows[next].length - 1 : target];
}

const caseOf = (char, upper) => (char === "ß" ? char : upper ? char.toUpperCase() : char.toLowerCase());

export class OnScreenKeyboard {
  constructor(element, {german, box, maxLength = 20, onDone, onCancel}) {
    this.element = element;
    this.box = box;
    this.maxLength = maxLength;
    this.onDone = onDone;
    this.onCancel = onCancel;
    this.rows = keyRows(german);
    this.at = [0, 0];
    this.upper = true;
    this.buttons = [];
    element.replaceChildren();
    const help = document.createElement("p");
    help.className = "osk-help";
    help.textContent = t("osk.help");
    const grid = document.createElement("div");
    grid.className = "osk-grid";
    this.rows.forEach((row, r) => {
      this.buttons[r] = row.map((key, i) => {
        const button = document.createElement("button");
        button.type = "button";
        button.tabIndex = -1;
        button.style.gridColumn = `${key.col + 1} / span ${key.span}`;
        button.style.gridRow = String(r + 1);
        // The box keeps focus, so a real keyboard still types.
        button.addEventListener("pointerdown", (event) => event.preventDefault());
        button.addEventListener("click", () => {
          this.at = [r, i];
          this.press(key);
        });
        grid.append(button);
        return button;
      });
    });
    element.append(grid, help);
    this.label();
  }
  get open() { return !this.element.hidden; }
  show(below) {
    this.at = [0, 0];
    this.upper = this.box.value.length === 0;
    this.element.classList.toggle("top", !below);
    this.element.hidden = false;
    this.label();
  }
  hide() { this.element.hidden = true; }
  label() {
    this.rows.forEach((row, r) => row.forEach((key, i) => {
      const button = this.buttons[r][i];
      button.textContent = key.char ? caseOf(key.char, this.upper)
        : {shift: "⇧", space: t("osk.space"), delete: "⌫", done: t("button.ok")}[key.action];
      button.classList.toggle("current", r === this.at[0] && i === this.at[1]);
      if (key.action === "shift") button.setAttribute("aria-pressed", String(this.upper));
    }));
  }
  type(text) {
    if (this.box.value.length + text.length > this.maxLength) return;
    this.box.value += text;
    this.box.dispatchEvent(new Event("input", {bubbles: true}));
  }
  press(key) {
    if (key.char) {
      this.type(caseOf(key.char, this.upper));
      // A capital starts a name; the rest follows in small letters.
      if (this.upper && /\p{L}/u.test(key.char) && this.box.value.length === 1) this.upper = false;
    } else if (key.action === "space") {
      this.type(" ");
    } else if (key.action === "delete") {
      this.box.value = this.box.value.slice(0, -1);
      this.box.dispatchEvent(new Event("input", {bubbles: true}));
    } else if (key.action === "shift") {
      this.upper = !this.upper;
    } else if (key.action === "done") {
      this.onDone();
      return;
    }
    this.label();
  }
  // A controller's navigation action (gamepad.js).
  action(name) {
    if (["up", "down", "left", "right"].includes(name)) {
      this.at = moveKey(this.rows, this.at, name);
      this.label();
    } else if (name === "accept") {
      this.press(this.rows[this.at[0]][this.at[1]]);
    } else if (name === "back") {
      this.press({action: "delete"});
    } else if (name === "alt") {
      this.press({action: "shift"});
    } else if (name === "extra") {
      this.press({action: "space"});
    } else if (name === "start") {
      this.onDone();
    } else if (name === "menu") {
      this.onCancel();
    }
  }
}

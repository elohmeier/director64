// Gamepads as the console's controllers. Each connected pad becomes one of
// the four ports, reported to the runtime as an N64 controller sample (input.h
// buttons, stick in the console's range), so cursors, mouse ownership and
// every game's controller layer behave as on the console. Menus and the
// on-screen keyboard read the same pads as navigation actions instead.
//
// Indices follow the Gamepad API's standard mapping, which Chrome applies to
// a DualShock 4 or DualSense on Linux, Windows and macOS.
export const BUTTON = {
  cross: 0, circle: 1, square: 2, triangle: 3, l1: 4, r1: 5, l2: 6, r2: 7,
  share: 8, options: 9, l3: 10, r3: 11, up: 12, down: 13, left: 14, right: 15,
  home: 16, touchpad: 17,
};
// runtime/interaction/input.h
export const INPUT = {
  A: 1, B: 2, LEFT: 4, RIGHT: 8, UP: 16, DOWN: 32, START: 64,
  C_LEFT: 128, C_RIGHT: 256, C_UP: 512, C_DOWN: 1024,
};
export const PORTS = 4;

const MAPPED = [
  [BUTTON.cross, INPUT.A], [BUTTON.touchpad, INPUT.A], [BUTTON.circle, INPUT.B],
  [BUTTON.options, INPUT.START], [BUTTON.up, INPUT.UP], [BUTTON.down, INPUT.DOWN],
  [BUTTON.left, INPUT.LEFT], [BUTTON.right, INPUT.RIGHT],
];
// A worn DualShock stick rests up to about a tenth off centre.
export const DEAD_ZONE = 0.12;
// The console's stick reaches about ±80; input.c ignores ±8 and moves the
// cursor 0.11 px per unit and tick, so full deflection is about 530 px/s.
const STICK_MIN = 9, STICK_MAX = 80;
// L1 held: fine movement, for small hotspots.
const FINE = 0.35;
// Navigation: a held direction repeats after a pause.
const REPEAT_DELAY = 400, REPEAT_EVERY = 110;

const down = (pad, index) => {
  const button = pad.buttons[index];
  return !!button && (button.pressed || button.value > 0.5);
};

// A stick in the console's units: x right, y up. Radial dead zone, then a
// gentle curve so small deflections stay precise.
export function consoleStick(x, y, scale = 1) {
  const r = Math.hypot(x, y);
  if (!(r > DEAD_ZONE)) return [0, 0];
  const m = Math.min(1, (r - DEAD_ZONE) / (1 - DEAD_ZONE)) ** 1.5;
  const magnitude = STICK_MIN + (STICK_MAX - STICK_MIN) * m * scale;
  return [Math.round((x / r) * magnitude) || 0, Math.round((-y / r) * magnitude) || 0];
}

// One pad as a console controller sample. The right stick stands in for the
// C buttons (Mucklas steers with them).
export function consoleSample(pad) {
  let buttons = 0;
  for (const [index, bit] of MAPPED) if (down(pad, index)) buttons |= bit;
  const axes = pad.axes ?? [];
  const [rx, ry] = [axes[2] ?? 0, axes[3] ?? 0];
  if (rx < -0.5) buttons |= INPUT.C_LEFT;
  if (rx > 0.5) buttons |= INPUT.C_RIGHT;
  if (ry < -0.5) buttons |= INPUT.C_UP;
  if (ry > 0.5) buttons |= INPUT.C_DOWN;
  const [stickX, stickY] = consoleStick(axes[0] ?? 0, axes[1] ?? 0, down(pad, BUTTON.l1) ? FINE : 1);
  return {buttons, stickX, stickY};
}

// What a pad asks of a menu: held directions and pressed actions.
export function navigation(pad) {
  const axes = pad.axes ?? [];
  const x = axes[0] ?? 0, y = axes[1] ?? 0;
  return {
    up: down(pad, BUTTON.up) || y < -0.5,
    down: down(pad, BUTTON.down) || y > 0.5,
    left: down(pad, BUTTON.left) || x < -0.5,
    right: down(pad, BUTTON.right) || x > 0.5,
    accept: down(pad, BUTTON.cross) || down(pad, BUTTON.touchpad),
    back: down(pad, BUTTON.circle),
    alt: down(pad, BUTTON.triangle),
    extra: down(pad, BUTTON.square),
    start: down(pad, BUTTON.options),
    menu: down(pad, BUTTON.home) || down(pad, BUTTON.share),
  };
}
const DIRECTIONS = new Set(["up", "down", "left", "right"]);

// Polls the pads once per animation frame for every reader. `ports` are the
// connected pads in index order (at most four), `actions` the navigation
// actions that began (or repeated) this frame, in port order.
export class Gamepads extends EventTarget {
  constructor(source = () => globalThis.navigator?.getGamepads?.() ?? []) {
    super();
    this.source = source;
    this.at = -1;
    this.ports = [];
    this.actions = [];
    this.held = new Map(); // pad index -> Map(action -> when it last fired)
    this.ignored = new Map(); // pad index -> actions to ignore until released
    this.masked = new Map(); // pad index -> console buttons to ignore until released
    this.known = new Set();
  }
  pads() {
    return [...this.source()].filter((pad) => pad && pad.connected !== false).slice(0, PORTS);
  }
  poll(now) {
    if (now === this.at) return this;
    this.at = now;
    const pads = this.pads();
    const present = new Set(pads.map((pad) => pad.index));
    for (const index of this.known)
      if (!present.has(index)) this.dispatchEvent(new CustomEvent("disconnected", {detail: index}));
    for (const pad of pads)
      if (!this.known.has(pad.index)) this.dispatchEvent(new CustomEvent("connected", {detail: pad.id}));
    this.known = present;
    this.actions = [];
    this.ports = pads.map((pad) => {
      const sample = consoleSample(pad);
      const mask = (this.masked.get(pad.index) ?? 0) & sample.buttons;
      this.masked.set(pad.index, mask);
      sample.buttons &= ~mask;
      sample.active = sample.buttons !== 0 || sample.stickX !== 0 || sample.stickY !== 0;
      const held = this.held.get(pad.index) ?? new Map();
      const ignored = this.ignored.get(pad.index) ?? new Set();
      for (const [action, on] of Object.entries(navigation(pad))) {
        if (!on) {
          held.delete(action);
          ignored.delete(action);
        } else if (ignored.has(action)) {
          continue;
        } else if (!held.has(action)) {
          held.set(action, now);
          this.actions.push(action);
        } else if (DIRECTIONS.has(action) && now - held.get(action) >= REPEAT_DELAY) {
          held.set(action, now - REPEAT_DELAY + REPEAT_EVERY);
          this.actions.push(action);
        }
      }
      this.held.set(pad.index, held);
      this.ignored.set(pad.index, ignored);
      return sample;
    });
    return this;
  }
  get connected() { return this.ports.length; }
  get active() { return this.ports.some((port) => port.active); }
  // Whatever is held now belongs to the screen that just closed or opened:
  // neither the game nor a menu reads it until it is released and pressed
  // again.
  swallow() {
    this.actions = [];
    for (const pad of this.pads()) {
      this.masked.set(pad.index, consoleSample(pad).buttons);
      this.ignored.set(pad.index, new Set(Object.entries(navigation(pad))
        .filter(([, on]) => on).map(([action]) => action)));
    }
  }
}

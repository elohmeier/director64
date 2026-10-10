import assert from "node:assert/strict";
import test from "node:test";
import {BUTTON, DEAD_ZONE, Gamepads, INPUT, consoleSample, consoleStick} from "../../platforms/web/site/gamepad.js";
import {keyRows, moveKey} from "../../platforms/web/site/keyboard.js";
import {sharpFactor} from "../../platforms/web/site/screen.js";
import {DEFAULTS, readSettings} from "../../platforms/web/site/settings.js";
import {chooseLanguage, keys, t} from "../../platforms/web/site/strings.js";

// A standard-mapping pad: 18 buttons, 4 axes.
function pad({index = 0, pressed = [], axes = [0, 0, 0, 0]} = {}) {
  return {index, id: "Wireless Controller", connected: true, mapping: "standard", axes,
    buttons: Array.from({length: 18}, (_, i) => ({pressed: pressed.includes(i), value: pressed.includes(i) ? 1 : 0}))};
}

test("a pad's buttons become the console's", () => {
  const sample = consoleSample(pad({pressed: [BUTTON.cross, BUTTON.options, BUTTON.left]}));
  assert.equal(sample.buttons, INPUT.A | INPUT.START | INPUT.LEFT);
  assert.equal(consoleSample(pad({pressed: [BUTTON.circle]})).buttons, INPUT.B);
  assert.equal(consoleSample(pad({pressed: [BUTTON.touchpad]})).buttons, INPUT.A);
  // The right stick stands in for the C buttons.
  assert.equal(consoleSample(pad({axes: [0, 0, 1, -1]})).buttons, INPUT.C_RIGHT | INPUT.C_UP);
});

test("the stick has a dead zone, reaches the console's range and points up positive", () => {
  assert.deepEqual(consoleStick(DEAD_ZONE * 0.9, 0), [0, 0]);
  const [x] = consoleStick(DEAD_ZONE + 0.001, 0);
  assert.ok(x >= 9, "just past the dead zone moves the console's cursor at all");
  assert.deepEqual(consoleStick(1, 0), [80, 0]);
  assert.deepEqual(consoleStick(0, -1), [0, 80]); // the Gamepad API's y grows downward
  const [fine] = consoleStick(1, 0, 0.35);
  assert.ok(fine > 9 && fine < 40);
  // L1 slows the stick.
  assert.ok(consoleSample(pad({pressed: [BUTTON.l1], axes: [1, 0, 0, 0]})).stickX < 40);
});

test("navigation fires on press, repeats when held and ignores what was swallowed", () => {
  let pads = [pad({pressed: [BUTTON.down]})];
  const gamepads = new Gamepads(() => pads);
  assert.deepEqual(gamepads.poll(0).actions, ["down"]);
  assert.deepEqual(gamepads.poll(16).actions, []);
  assert.deepEqual(gamepads.poll(400).actions, ["down"]);
  assert.deepEqual(gamepads.poll(450).actions, []);
  assert.deepEqual(gamepads.poll(510).actions, ["down"]);
  // A button held over a screen change reaches neither side until released.
  pads = [pad({pressed: [BUTTON.cross]})];
  gamepads.poll(600);
  gamepads.swallow();
  assert.deepEqual(gamepads.poll(616).actions, []);
  assert.equal(gamepads.ports[0].buttons, 0);
  pads = [pad()];
  gamepads.poll(632);
  pads = [pad({pressed: [BUTTON.cross]})];
  assert.deepEqual(gamepads.poll(648).actions, ["accept"]);
  assert.equal(gamepads.ports[0].buttons, INPUT.A);
});

test("pads take ports in order and report connection changes", () => {
  let pads = [null, pad({index: 1}), pad({index: 3, axes: [1, 0, 0, 0]})];
  const gamepads = new Gamepads(() => pads);
  const events = [];
  gamepads.addEventListener("connected", () => events.push("connected"));
  gamepads.addEventListener("disconnected", () => events.push("disconnected"));
  gamepads.poll(0);
  assert.equal(gamepads.connected, 2);
  assert.ok(gamepads.active);
  assert.equal(gamepads.ports[1].stickX, 80);
  pads = [null, pad({index: 1})];
  gamepads.poll(16);
  assert.deepEqual(events, ["connected", "connected", "disconnected"]);
  assert.ok(!gamepads.active);
});

test("the on-screen keyboard's rows are whole and moves keep the column", () => {
  for (const german of [false, true]) {
    const rows = keyRows(german);
    assert.equal(rows.length, 5);
    assert.deepEqual(moveKey(rows, [0, 0], "left"), [0, 9]);
    assert.deepEqual(moveKey(rows, [0, 0], "up"), [4, 0]);
    // From "6" (column 5) down to the space bar (columns 3 to 5).
    assert.equal(rows[4][moveKey(rows, [3, 5], "down")[1]].action, "space");
    // And from the space bar back up to the column under its centre.
    assert.deepEqual(moveKey(rows, [4, 3], "up"), [3, 4]);
  }
  assert.ok(keyRows(true)[2].some((key) => key.char === "ß"));
  assert.ok(!keyRows(false).flat().some((key) => key.char === "\""));
});

test("sharp scaling enlarges by whole pixels up to the display size", () => {
  assert.equal(sharpFactor(640, 480), 1);
  assert.equal(sharpFactor(1440, 1080), 3); // 2.25x rounds up
  assert.equal(sharpFactor(1280, 960, 2), 4);
  assert.equal(sharpFactor(100000, 100000), 8);
});

test("settings fall back to defaults for anything malformed", () => {
  const storage = (value) => ({getItem: () => value});
  assert.deepEqual(readSettings(storage(null)), DEFAULTS);
  assert.deepEqual(readSettings(storage("{")), DEFAULTS);
  assert.deepEqual(readSettings(storage(JSON.stringify({volume: 7, scaling: "blur", muted: true}))),
    {...DEFAULTS, volume: 1, muted: true});
});

test("both languages have every text and fill placeholders", () => {
  assert.deepEqual(keys("de").sort(), keys("en").sort());
  for (const key of keys("en")) {
    const names = (lang) => [...t(key, {}, lang).matchAll(/\{(\w+)\}/g)].map((m) => m[1]).sort();
    assert.deepEqual(names("de"), names("en"), key);
  }
  assert.equal(t("import.ready", {seconds: 6}, "de"), "In 6 s umgewandelt.");
  assert.equal(chooseLanguage("", ["de-DE", "en"]), "de");
  assert.equal(chooseLanguage("?lang=en", ["de-DE"]), "en");
  assert.equal(chooseLanguage("", ["fr-FR"]), "en");
});

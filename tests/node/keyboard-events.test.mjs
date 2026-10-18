import assert from "node:assert/strict";
import test from "node:test";
import {KeyEvents} from "../../platforms/web/site/keyboard.js";

function engine(limit = 32) {
  const received = [], batch = [];
  return {received, batch, _d64_key_capacity: () => limit - batch.length,
    _d64_key: (...event) => { assert.ok(batch.length < limit); batch.push(event); },
    step() { received.push(...batch.splice(0)); }};
}

test("keyboard bursts retain all down/up pairs across engine capacity windows", () => {
  const module = engine(), keys = new KeyEvents(module), expected = [];
  for (let i = 0; i < 40; i++) {
    keys.forward(0, 97, true); keys.forward(0, 97, false);
    expected.push([0, 97, 1], [0, 97, 0]);
  }
  assert.equal(module.batch.length, 32);
  assert.equal(keys.pending.length, 48);
  while (module.batch.length || keys.pending.length) { module.step(); keys.drain(); }
  assert.deepEqual(module.received, expected);
  assert.equal(keys.error, "");
});

test("modified, focused or paused releases preserve original key ownership", () => {
  const module = engine(), keys = new KeyEvents(module);
  keys.forward(0, 97, true);
  keys.forward(0, 0, false, false);
  keys.forward(0, 65, true);
  keys.forward(0, 65, true); // repeat
  keys.releaseAll(); module.step();
  assert.deepEqual(module.received, [[0, 97, 1], [0, 97, 0], [0, 65, 1], [0, 65, 0]]);
  assert.equal(keys.held.size, 0);
  assert.equal(keys.forward(1, 98, true, false), false);
});

test("blur cleanup waits for capacity instead of dropping the release", () => {
  const module = engine(1), keys = new KeyEvents(module);
  keys.forward(0, 97, true); keys.releaseAll();
  assert.equal(keys.pending.length, 1);
  module.step(); keys.drain(); module.step();
  assert.deepEqual(module.received, [[0, 97, 1], [0, 97, 0]]);
});

test("host overflow and stale runtime exports are visible failures", () => {
  const keys = new KeyEvents(engine(0), 2);
  keys.forward(0, 97, true); keys.forward(0, 97, false); keys.forward(0, 97, true);
  assert.match(keys.error, /queue overflow/);
  const stale = new KeyEvents({});
  stale.forward(0, 97, true);
  assert.match(stale.error, /rebuild the player/);
});

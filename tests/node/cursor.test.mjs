import assert from "node:assert/strict";
import test from "node:test";
import {cursorPresentation} from "../../platforms/web/site/cursor.js";

test("physical mouse uses standard OS cursor roles with no duplicate canvas glyph", () => {
  for (const [role, css] of [[0, "default"], [1, "text"], [2, "crosshair"], [3, "crosshair"], [4, "wait"]]) {
    assert.deepEqual(cursorPresentation(role, true), {css, software: false});
    assert.deepEqual(cursorPresentation(role, false), {css: "none", software: true});
  }
});
test("authored and invert cursors stay on canvas; Director hidden cursor stays hidden", () => {
  assert.deepEqual(cursorPresentation(-1, true), {css: "none", software: true});
  assert.deepEqual(cursorPresentation(200, true), {css: "none", software: false});
  assert.deepEqual(cursorPresentation(200, false), {css: "none", software: false});
});

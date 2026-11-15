// The shared look (bridge LLD R28.2, design-notes §206-§207): which look a page starts in.
// Run: node --test firmware/esp32/web/test/theme.test.js
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const T = require("../theme.js");

test("nothing saved: Sunny on a touch device, the cockpit with a mouse", () => {
  assert.equal(T.pick(null, true), "play");
  assert.equal(T.pick(null, false), "cockpit");
  assert.equal(T.pick("", true), "play");
});

test("a saved look wins over the device", () => {
  assert.equal(T.pick("cockpit", true), "cockpit");
  assert.equal(T.pick("play", false), "play");
});

test("a junk saved value counts as nothing saved", () => {
  assert.equal(T.pick("junk", false), "cockpit");
  assert.equal(T.pick("junk", true), "play");
});

test("one key for every page", () => {
  assert.equal(T.KEY, "cq_theme");
});

// R31.10 A (design-notes §253): the press look's target
function node(tag, opts) { return Object.assign({ tagName: tag, disabled: false, parent: null }, opts || {}); }
function closest(n) { for (let x = n; x; x = x.parent) if (x.tagName === "BUTTON") return x; return null; }

test("the press look: the nearest button, from a child too", () => {
  const b = node("BUTTON"), icon = node("SVG", { parent: b });
  b.closest = () => b; icon.closest = () => closest(icon);
  assert.equal(T.pressTarget(b), b);
  assert.equal(T.pressTarget(icon), b);
});

test("the press look: none for a disabled button or outside any button", () => {
  const b = node("BUTTON", { disabled: true }), p = node("P");
  b.closest = () => b; p.closest = () => null;
  assert.equal(T.pressTarget(b), null);
  assert.equal(T.pressTarget(p), null);
  assert.equal(T.pressTarget(null), null);
  assert.equal(T.PRESS_MS, 150);
});

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

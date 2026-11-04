// TankModel (bridge LLD R31, design-notes §210): the tank's data and rules, no screen code.
// Run: node --test firmware/esp32/web/test/tank_model.test.js
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const T = require("../tank_model.js");

test("raw data, not texts: battery average and flags, heading, speed, WiFi, Uno", () => {
  const m = new T.TankModel();
  m.apply({ t: "ts", batt_v: 7.6, flags: ["batt_low"] });
  m.apply({ t: "tf", yaw: 91.6, spd_cmd: [-40, 25] });
  m.apply({ t: "wifi_status", mode: "home", rssi: -58 });
  assert.deepEqual(m.state.batt, { v: 7.6, flags: ["batt_low"] });
  assert.equal(m.state.hdg, 91.6);
  assert.equal(m.state.spd, 40);
  assert.deepEqual(m.state.wifi, { mode: "home", rssi: -58 });
  assert.equal(m.state.uno, true);
});

test("who drives: welcome gives this page's name, control_changed the owner", () => {
  const m = new T.TankModel();
  assert.equal(m.state.ownerKnown, false);
  m.apply({ t: "welcome", you: "phone-1", owner: "none", owner_client: "", tank: { fw: { uno: "1.0" } } });
  assert.equal(m.state.you, "phone-1");
  assert.equal(m.state.owner, "none");
  assert.equal(m.state.ownerKnown, true);
  m.apply({ t: "ev", kind: "control_changed", owner: "human", client: "phone-2" });
  assert.deepEqual([m.state.owner, m.state.ownerClient], ["human", "phone-2"]);
});

test("the link: up, then down clears what moves and was live", () => {
  const m = new T.TankModel();
  m.linkUp();
  assert.equal(m.state.link, "on");
  m.apply({ t: "tf", yaw: 10, spd_cmd: [20, 20] });
  m.linkDown();
  assert.equal(m.state.link, "down");
  assert.deepEqual([m.state.hdg, m.state.spd, m.state.pos, m.state.batt, m.state.uno], [null, null, null, null, null]);
});

test("views hear every change, once per change", () => {
  const m = new T.TankModel();
  let n = 0;
  m.on(() => { n++; });
  m.apply({ t: "tf", yaw: 1 });
  m.apply({ t: "something_else" });  // not the model's: no change
  m.linkDown();
  assert.equal(n, 2);
});

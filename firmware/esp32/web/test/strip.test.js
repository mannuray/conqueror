// The status strip on /ai, /logo and /settings (bridge LLD R30, design-notes §209): its rules and what each tank message
// shows. Run: node --test firmware/esp32/web/test/strip.test.js
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const S = require("../strip.js");

// moved from ai.test.js (Plan Z2b-1): the battery as the drive page shows it
test("battery percent and level from the voltage and the tank's flags", () => {
  assert.equal(S.battPct(8.5), 100);
  assert.equal(S.battPct(7.6), 50);
  assert.equal(S.battPct(7.5), 43);  // between 7.6 (50) and 7.4 (35)
  assert.equal(S.battPct(6.5), 0);
  assert.deepEqual(S.battState(7.6, []), { usb: false, pct: 50, level: "ok" });
  assert.deepEqual(S.battState(7.1, []), { usb: false, pct: 15, level: "low" });
  assert.deepEqual(S.battState(7.6, ["batt_low"]), { usb: false, pct: 50, level: "low" });
  assert.deepEqual(S.battState(6.9, ["batt_critical"]), { usb: false, pct: 8, level: "critical" });
  assert.deepEqual(S.battState(5.0, []), { usb: true, pct: null, level: "ok" });
});

// moved from logo.test.js (N7): a 10-reading average that starts again between USB and the pack
test("the battery average starts again between USB and the pack", () => {
  const m = new S.BattMean(3);  // from ai.test.js
  m.push(7.6); m.push(7.8);
  assert.ok(Math.abs(m.push(7.7) - 7.7) < 1e-9);
  assert.equal(m.push(5.0), 5.0);  // on USB: the average starts again
  const t = new S.BattMean(10);  // from logo.test.js
  for (let i = 0; i < 10; i++) t.push(8.0);
  assert.equal(t.push(6.9).toFixed(2), "7.89");   // one sagging reading barely moves it
  assert.equal(t.push(5.0), 5.0);                 // USB: a fresh average
  assert.equal(t.push(8.0), 8.0);                 // back on the pack: fresh again
});

// moved from logo.test.js (N7): WiFi by RSSI as the drive page; no reading is "no signal"
test("the WiFi icon and text", () => {
  assert.equal(S.wifiIcon(-50), "wifi");
  assert.equal(S.wifiIcon(-60), "wifi-high");
  assert.equal(S.wifiIcon(-70), "wifi-low");
  assert.equal(S.wifiIcon(-80), "wifi-zero");
  assert.equal(S.wifiIcon(null), "wifi-zero");
  assert.deepEqual(S.wifiText("home", -50), { icon: "wifi", text: "HOME", warn: false });
  assert.deepEqual(S.wifiText("home", -80), { icon: "wifi-zero", text: "HOME", warn: true });
  assert.deepEqual(S.wifiText("ap", null), { icon: "wifi", text: "OWN WIFI", warn: false });
  assert.deepEqual(S.wifiText("connecting", null), { icon: "wifi-zero", text: "CONNECTING", warn: true });
});

test("POS in metres", () => {
  assert.equal(S.posText(0.3, 0.5), "POS 0.3,0.5");
  assert.equal(S.posText(-1.26, 2), "POS -1.3,2.0");
});

test("each tank message fills its chip", () => {
  const s = new S.Strip();
  s.tank({ t: "welcome", tank: { fw: { uno: "1.0" } } });
  assert.deepEqual(s.view.uno, { text: "UNO ONLINE", warn: false });
  s.tank({ t: "ts", batt_v: 7.6, flags: [] });
  assert.deepEqual(s.view.batt, { text: "50 %", fill: 11, level: "ok", title: "battery 7.60 V" });
  s.tank({ t: "ts", batt_v: 7.6, flags: ["body_offline"] });
  assert.deepEqual(s.view.uno, { text: "UNO OFFLINE", warn: true });
  s.tank({ t: "tf", yaw: 91.6, spd_cmd: [-40, 25] });
  assert.equal(s.view.hdg, "HDG 92°");
  assert.equal(s.view.spd, "SPEED 40%");
  s.tank({ t: "wifi_status", mode: "home", rssi: -50 });
  assert.deepEqual(s.view.wifi, { icon: "wifi", text: "HOME", warn: false });
  s.tank({ t: "ai_at", url: "ws://x:8770/feed" });
  assert.equal(s.view.ai, true);
  s.tank({ t: "ai_at", url: null });
  assert.equal(s.view.ai, false);
  s.tank({ t: "ev", kind: "logo_pose", x_cm: 30, y_cm: 50, heading: 0 });
  assert.equal(s.view.pos, "POS 0.3,0.5");
});

test("USB power, then the socket drops", () => {
  const s = new S.Strip();
  s.tank({ t: "ts", batt_v: 5.0, flags: [] });
  assert.deepEqual(s.view.batt, { text: "USB", fill: 0, level: "ok", title: "on USB power" });
  s.linkDown();
  assert.deepEqual(s.view.wifi, { icon: "wifi-off", text: "RECONNECTING", warn: true });
  assert.deepEqual(s.view.batt, { text: "NO TANK", fill: 0, level: "low", title: "battery" });
  assert.deepEqual(s.view.uno, { text: "UNO --", warn: false });
});

test("a page that owns the AI HELPER chip (/ai, its feed) is not overridden by ai_at", () => {
  const s = new S.Strip();
  s.aiHelper(false);
  s.tank({ t: "ai_at", url: "ws://x:8770/feed" });
  assert.equal(s.view.ai, false);
  s.aiHelper(true);
  assert.equal(s.view.ai, true);
  s.pose(1.24, -0.5);
  assert.equal(s.view.pos, "POS 1.2,-0.5");
  s.fps(12);
  assert.equal(s.view.fps, "12 / s");
  s.fps(null);
  assert.equal(s.view.fps, "-- / s");
});

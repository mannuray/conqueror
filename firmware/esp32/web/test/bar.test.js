// The shared top bar (bridge LLD R29, design-notes §208): the menu's pages and STOP from a page with no socket of its own.
// Run: node --test firmware/esp32/web/test/bar.test.js
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const B = require("../bar.js");

test("the menu: five pages in order", () => {
  assert.equal(B.PAGES.map((p) => p.path).join(), "/,/logo,/ai,/settings,/help");
  assert.equal(B.PAGES.map((p) => p.name).join(), "DRIVE,LOGO,AI,SETTINGS,HELP");
});

test("here(): the menu entry of a path, null for an unknown one", () => {
  assert.equal(B.here("/").name, "DRIVE");
  assert.equal(B.here("/logo").name, "LOGO");
  assert.equal(B.here("/ai").name, "AI");
  assert.equal(B.here("/settings").name, "SETTINGS");
  assert.equal(B.here("/help").name, "HELP");
  assert.equal(B.here("/nope"), null);
});

// A stand-in socket: `answer` is "ack", "error" or "silent"; it opens on the next tick.
function fakeWorld(answer) {
  const w = { sockets: [], timers: [], now: 0 };
  w.WebSocket = function (url) {
    const s = this;
    s.url = url; s.sent = []; s.closed = false; w.sockets.push(s);
    s.send = (t) => { s.sent.push(JSON.parse(t)); if (answer === "ack" && s.sent.length === 3) setImmediate(() => s.onmessage({ data: '{"t":"ack","id":' + s.sent[1].id + "}" })); };
    s.close = () => { s.closed = true; };
    setImmediate(() => { if (answer === "error") { s.onerror && s.onerror({}); s.onclose && s.onclose({}); } else s.onopen(); });
  };
  w.setTimeout = (f, ms) => { w.timers.push({ f, ms }); return w.timers.length; };
  w.clearTimeout = (h) => { if (w.timers[h - 1]) w.timers[h - 1].f = null; };
  w.fire = () => { w.timers.forEach((t) => { if (t.f) { const f = t.f; t.f = null; f(); } }); };
  return w;
}

function run(answer) {
  const w = fakeWorld(answer), calls = [];
  B.stopOnce("ws://tank/ws", (ok) => calls.push(ok), { WebSocket: w.WebSocket, setTimeout: w.setTimeout, clearTimeout: w.clearTimeout });
  return { w, calls };
}

const tick = () => new Promise((r) => setImmediate(() => setImmediate(r)));

test("stopOnce: the tank answers - hello, stop, logo_stop sent, done(true) once, socket closed", async () => {
  const { w, calls } = run("ack");
  await tick(); await tick();
  const s = w.sockets[0];
  assert.equal(s.url, "ws://tank/ws");
  assert.equal(s.sent.map((m) => m.t).join(), "hello,stop,logo_stop");
  assert.equal(s.sent[0].client, "help");
  assert.deepEqual(calls, [true]);
  assert.equal(s.closed, true);
  w.fire();  // the 2 s timer was cleared: nothing more
  assert.deepEqual(calls, [true]);
});

test("stopOnce: the socket fails (the bridge is full) - done(false) once, closed", async () => {
  const { w, calls } = run("error");
  await tick();
  assert.deepEqual(calls, [false]);
  assert.equal(w.sockets[0].closed, true);
  w.fire();
  assert.deepEqual(calls, [false]);
});

test("stopOnce: no answer - done(false) after the 2000 ms timer, closed", async () => {
  const { w, calls } = run("silent");
  await tick();
  assert.deepEqual(calls, []);
  assert.equal(w.timers[0].ms, 2000);
  w.fire();
  assert.deepEqual(calls, [false]);
  assert.equal(w.sockets[0].closed, true);
});

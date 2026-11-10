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

// A stand-in socket: `answer` is "ack", "error", "silent", "nack" (stop refused) or "ack2" (only logo_stop acked: a
// program stopped, the body's ack for stop still on its way); it opens on the next tick.
function fakeWorld(answer) {
  const w = { sockets: [], timers: [], now: 0 };
  w.WebSocket = function (url) {
    const s = this;
    s.url = url; s.sent = []; s.closed = false; w.sockets.push(s);
    s.send = (t) => { s.sent.push(JSON.parse(t)); if (s.sent.length !== 3) return;
      const reply = { ack: '{"t":"ack","id":' + s.sent[1].id + "}", nack: '{"t":"nack","id":' + s.sent[1].id + ',"reason":"BUSY"}',
        ack2: '{"t":"ack","id":' + s.sent[2].id + "}" }[answer];
      if (reply) setImmediate(() => s.onmessage({ data: reply })); };
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

test("stopOnce: only logo_stop acked - not yet 'Stopped'; done(false) after 2 s, closed", async () => {
  const { w, calls } = run("ack2");
  await tick(); await tick();
  assert.deepEqual(calls, []);
  w.fire();
  assert.deepEqual(calls, [false]);
  assert.equal(w.sockets[0].closed, true);
});

test("stopOnce: stop refused (nack) - never 'Stopped'; done(false) after 2 s, closed", async () => {
  const { w, calls } = run("nack");
  await tick(); await tick();
  assert.deepEqual(calls, []);
  w.fire();
  assert.deepEqual(calls, [false]);
  assert.equal(w.sockets[0].closed, true);
});

// R31: one STOP wiring for every page - on touch (a tap that slides never becomes a click), and from the keyboard
function fakeButton() {
  const b = { on: {}, addEventListener(type, fn) { (b.on[type] = b.on[type] || []).push(fn); },
    fire(type, ev) { (b.on[type] || []).forEach((f) => f(Object.assign({ preventDefault() { ev && (ev.prevented = true); } }, ev))); } };
  return b;
}

test("onStop: a touch stops at once, its click after it does not stop again", () => {
  const b = fakeButton(); let n = 0;
  B.onStop(() => n++, b);
  b.fire("pointerdown", {});
  b.fire("click", { detail: 1 });
  assert.equal(n, 1);
});

test("onStop: Enter / Space (a click with detail 0) stops", () => {
  const b = fakeButton(); let n = 0;
  B.onStop(() => n++, b);
  b.fire("click", { detail: 0 });
  assert.equal(n, 1);
});


test("stopOnce: its stop is a press (design-notes §232: only a press releases a held game)", async () => {
  const { w } = run("ack");
  await tick(); await tick();
  assert.equal(w.sockets[0].sent[1].t, "stop");
  assert.equal(w.sockets[0].sent[1].press, true);
});

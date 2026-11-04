// cqLink, the one link every page uses (bridge LLD R31, design-notes §210): a fake socket, timers, clock and document.
// Run: node --test firmware/esp32/web/test/link.test.js
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const K = require("../link.js");
const T = require("../tank_model.js");

function world() {
  const w = { t: 0, timers: [], sockets: [], doc: { hidden: false, fns: [], addEventListener(type, fn) { if (type === "visibilitychange") this.fns.push(fn); } },
    onceCalls: [] };
  w.WebSocket = function (url) {
    const s = this; s.url = url; s.sent = []; s.closed = false; w.sockets.push(s);
    s.send = (x) => s.sent.push(JSON.parse(x));
    s.close = () => { s.closed = true; };
  };
  let id = 0;
  w.setTimeout = (f, ms) => { w.timers.push({ id: ++id, at: w.t + ms, f, every: 0 }); return id; };
  w.setInterval = (f, ms) => { w.timers.push({ id: ++id, at: w.t + ms, f, every: ms }); return id; };
  w.clearTimeout = w.clearInterval = (h) => { w.timers = w.timers.filter((x) => x.id !== h); };
  w.now = () => w.t;
  w.advance = (ms) => {
    const end = w.t + ms;
    for (;;) {
      const due = w.timers.filter((x) => x.at <= end).sort((a, b) => a.at - b.at)[0];
      if (!due) break;
      w.t = due.at;
      if (due.every) due.at += due.every; else w.timers = w.timers.filter((x) => x !== due);
      due.f();
    }
    w.t = end;
  };
  w.show = (hidden) => { w.doc.hidden = hidden; w.doc.fns.forEach((f) => f()); };
  w.stopOnce = (url, done) => w.onceCalls.push({ url, done });
  w.last = () => w.sockets[w.sockets.length - 1];
  w.env = { WebSocket: w.WebSocket, setTimeout: w.setTimeout, clearTimeout: w.clearTimeout, setInterval: w.setInterval,
    clearInterval: w.clearInterval, now: w.now, doc: w.doc, url: "ws://tank/ws", stopOnce: w.stopOnce };
  return w;
}
const types = (s) => s.sent.map((m) => m.t);

function openLink(w, extra) {
  const log = [], model = new T.TankModel();
  const link = K.open(Object.assign({ client: "logo", model, env: w.env,
    onMessage: (m) => log.push("msg:" + m.t), onOpen: () => log.push("open"), onClose: () => log.push("close"),
    onHide: () => { log.push("hide"); link.send({ t: "stop" }); } }, extra || {}));
  return { link, model, log };
}

test("opens at once: hello with the page's name and the WiFi strength; the model knows the link is up", () => {
  const w = world(), { model, log } = openLink(w);
  assert.equal(w.sockets.length, 1);
  assert.equal(w.last().url, "ws://tank/ws");
  w.last().onopen();
  assert.deepEqual(w.last().sent[0], { t: "hello", client: "logo", proto: 1 });
  assert.deepEqual(types(w.last()), ["hello", "wifi_status"]);
  assert.equal(model.state.link, "on");
  assert.deepEqual(log, ["open"]);
});

test("each message goes to the model, then to the page", () => {
  const w = world(), { model, log } = openLink(w);
  w.last().onopen();
  w.last().onmessage({ data: '{"t":"tf","yaw":12}' });
  w.last().onmessage({ data: "not json" });
  assert.equal(model.state.hdg, 12);
  assert.deepEqual(log, ["open", "msg:tf"]);
});

test("a ping every second, the WiFi strength every 10 s", () => {
  const w = world(); openLink(w);
  const s = w.last(); s.onopen();
  for (let i = 0; i < 10; i++) { w.advance(1000); s.onmessage({ data: '{"t":"ts"}' }); }
  assert.equal(types(s).filter((t) => t === "ping").length, 10);
  assert.equal(types(s).filter((t) => t === "wifi_status").length, 2);  // on open and at 10 s
});

test("4 s with nothing received: closed, the model down, the page told once, then a new link after 500 ms", () => {
  const w = world(), { model, log } = openLink(w);
  const s = w.last(); s.onopen();
  w.advance(4000);
  assert.equal(s.closed, false);  // 4 s exactly is not yet silent
  w.advance(1000);
  assert.equal(s.closed, true);
  assert.equal(model.state.link, "down");
  assert.deepEqual(log, ["open", "close"]);
  w.advance(499);
  assert.equal(w.sockets.length, 1);
  w.advance(1);
  assert.equal(w.sockets.length, 2);
});

test("reconnect waits double each time up to 5 s, and starts again from the page's first wait after a good link", () => {
  const w = world(); openLink(w);
  const gaps = [];
  for (let i = 0; i < 6; i++) {
    const n = w.sockets.length, t0 = w.t;
    w.last().onclose();
    while (w.sockets.length === n) w.advance(100);
    gaps.push(w.t - t0);
  }
  assert.deepEqual(gaps, [500, 1000, 2000, 4000, 5000, 5000]);
  w.last().onopen(); w.last().onclose();
  const t0 = w.t, n = w.sockets.length;
  while (w.sockets.length === n) w.advance(100);
  assert.equal(w.t - t0, 500);
});

test("the drive page's first wait is 1000 ms", () => {
  const w = world(); openLink(w, { retryMs: 1000 });
  const t0 = w.t; w.last().onclose();
  while (w.sockets.length === 1) w.advance(100);
  assert.equal(w.t - t0, 1000);
});

test("hidden: the page's onHide first (its STOP goes out), then the link closes, and no new link while hidden", () => {
  const w = world(), { log } = openLink(w);
  const s = w.last(); s.onopen();
  w.show(true);
  assert.deepEqual(types(s).slice(-1), ["stop"]);
  assert.equal(s.closed, true);
  assert.deepEqual(log, ["open", "hide", "close"]);
  w.advance(20000);
  assert.equal(w.sockets.length, 1);
  w.show(false);  // shown again: at once, not after a back-off
  assert.equal(w.sockets.length, 2);
});

test("never two sockets: shown again while a link is still opening makes no second one", () => {
  const w = world(); openLink(w);
  w.show(false);
  w.show(false);
  assert.equal(w.sockets.length, 1);
});

test("STOP: sent on the link when it is open; through its own socket and said when it is down", () => {
  const w = world(), { link } = openLink(w);
  const said = [];
  w.last().onopen();
  link.stop([{ t: "stop", id: 7 }, { t: "logo_stop", id: 8 }], (text, ok) => said.push([text, ok]));
  assert.deepEqual(types(w.last()).slice(-2), ["stop", "logo_stop"]);
  assert.deepEqual(said, []);
  w.last().onclose();
  link.stop([{ t: "stop", id: 9 }], (text, ok) => said.push([text, ok]));
  assert.equal(w.onceCalls.length, 1);
  assert.equal(w.onceCalls[0].url, "ws://tank/ws");
  assert.deepEqual(said, [["Stopping…", true]]);
  w.onceCalls[0].done(false);
  assert.deepEqual(said[1], ["Couldn't reach the tank - use another STOP", false]);
});

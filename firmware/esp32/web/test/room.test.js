// The ROOM tab's model, words and view (bridge LLD R32, AI engine LLD §23): no screen code here.
// Run: node --test firmware/esp32/web/test/room.test.js
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const R = require("../room.js");

function pose(x, y, h) { return { x: x, y: y, h: h }; }

test("applyMap keeps the crop and the cells", () => {
  const m = new R.RoomModel();
  const cells = new Uint8Array([0, 2, 3, 4]);
  m.applyMap({ kind: "map", seq: 1, cell_m: 0.05, x0: -0.5, y0: 1, w: 2, h: 2 }, cells);
  assert.deepEqual(m.state.crop, { x0: -0.5, y0: 1, w: 2, h: 2, cell: 0.05 });
  assert.equal(m.state.cells, cells);
});

test("applyRoom drops people, as things and as names", () => {
  const m = new R.RoomModel();
  m.applyRoom({ t: "room", trail: [[0, 0]], home: "room2", area: null, seen_m2: 1.5,
    things: [{ word: "ball", x: 1, y: 0, ago_s: 3, where: "near the bed" }, { word: "person", x: 0, y: 0, ago_s: 1, where: "" }],
    names: [{ id: 1, name: "bed", x: 1, y: 0, go: [0.6, 0] }, { id: 2, name: "my baby", x: 2, y: 0, go: null }],
    why: { route: [], pending: [], check: null, looked: [] } });
  assert.deepEqual(m.state.things.map((t) => t.word), ["ball"]);
  assert.deepEqual(m.state.names.map((n) => n.name), ["bed"]);
  assert.equal(m.state.home, "room2");
  assert.equal(m.state.seenM2, 1.5);
});

test("applyStatus keeps the pose and the pan; change listeners called", () => {
  const m = new R.RoomModel();
  let n = 0;
  m.on(() => { n++; });
  m.applyStatus({ t: "status", pose: { x: 1, y: 2, h: 90 }, pan: -15 });
  assert.deepEqual(m.state.pose, { x: 1, y: 2, h: 90 });
  assert.equal(m.state.pan, -15);
  m.applyStatus({ t: "status" });  // no pose yet: kept as none
  assert.equal(m.state.pose, null);
  assert.equal(n, 2);
});

test("the feed: none, open, lost - the map kept when lost", () => {
  const m = new R.RoomModel();
  assert.equal(m.state.feed, "none");
  m.feedOpen();
  m.applyMap({ cell_m: 0.05, x0: 0, y0: 0, w: 1, h: 1 }, new Uint8Array([2]));
  m.feedDown();
  assert.equal(m.state.feed, "lost");
  assert.notEqual(m.state.cells, null);
});

test("layers: defaults, a stored choice, a bad stored string", () => {
  const m = new R.RoomModel();
  assert.deepEqual(m.state.layers, { things: true, trail: true, names: true, why: false });
  m.setLayer("why", true);
  m.setLayer("trail", false);
  const j = m.layersJson();
  const m2 = new R.RoomModel();
  m2.loadLayers(j);
  assert.deepEqual(m2.state.layers, { things: true, trail: false, names: true, why: true });
  const m3 = new R.RoomModel();
  m3.loadLayers("{not json");
  assert.deepEqual(m3.state.layers, { things: true, trail: true, names: true, why: false });
  m3.loadLayers('{"floor": false, "why": "yes"}');  // unknown keys and non-booleans ignored
  assert.deepEqual(m3.state.layers, { things: true, trail: true, names: true, why: false });
});

test("ago words", () => {
  assert.equal(R.RoomWords.ago(30), "just now");
  assert.equal(R.RoomWords.ago(120), "2 min ago");
  assert.equal(R.RoomWords.ago(7200), "today");
  assert.equal(R.RoomWords.ago(90000), "earlier");
});

test("room facts on one line", () => {
  assert.equal(R.RoomWords.facts({ home: "room2", area: { x: 0, y: 0, r: 3 }, seenM2: 4.23 }),
    "home room2 · play area 3 m · floor seen 4.2 m²");
  assert.equal(R.RoomWords.facts({ home: null, area: null, seenM2: 0 }), "no home · anywhere · floor seen 0.0 m²");
});

test("nameCheck: the engine's rules", () => {
  assert.equal(R.RoomWords.nameCheck(""), "Names are letters, spaces and - (up to 24).");
  assert.equal(R.RoomWords.nameCheck("x".repeat(25)), "Names are letters, spaces and - (up to 24).");
  assert.equal(R.RoomWords.nameCheck("bed2"), "Names are letters, spaces and - (up to 24).");
  assert.equal(R.RoomWords.nameCheck("kids"), "I don't name people.");
  assert.equal(R.RoomWords.nameCheck("Toy Box"), null);
});

test("setxyLine: SETXY right ahead, in cm, from the pose now", () => {
  const W = R.RoomWords;
  assert.equal(W.setxyLine([1, 0], pose(0, 0, 0)), "SETXY 0 100");
  assert.equal(W.setxyLine([0, 1], pose(0, 0, 0)), "SETXY -100 0");  // 1 m left
  assert.equal(W.setxyLine([0, 1], pose(0, 0, 90)), "SETXY 0 100");  // facing +y: it is ahead
  assert.equal(W.setxyLine([1.5, -0.25], pose(0.5, 0.25, 0)), "SETXY 50 100");
  assert.equal(W.setxyLine([20, 0], pose(0, 0, 0)), null);  // beyond 1000 cm
  assert.equal(W.setxyLine([1, 0], null), null);
  assert.equal(W.setxyLine(null, pose(0, 0, 0)), null);
  // the line uses the pose it is given at the press, not one from earlier (Review Focus 2)
  assert.equal(W.setxyLine([1, 0], pose(0.5, 0, 0)), "SETXY 0 50");
});

// ---------- the view (R32.2): up = +x, right = -y ----------
function mapped(extra) {
  const m = new R.RoomModel();
  // a crop 2 m along x (40 cells) by 1 m along y (20 cells), floor, with a high block at x 1.0, y 0.0
  const cells = new Uint8Array(40 * 20).fill(2);
  for (let i = 28; i < 32; ++i) for (let j = 8; j < 12; ++j) cells[i * 20 + j] = 4;
  m.applyMap({ cell_m: 0.05, x0: -0.4, y0: -0.5, w: 40, h: 20 }, cells);
  m.applyStatus({ pose: { x: 0, y: 0, h: 0 } });
  if (extra) extra(m);
  return m;
}

test("fit: everything inside, +x up, -y right, at least 2 m", () => {
  const m = mapped();
  const t = R.RoomView.fit(m.state, 400, 400);
  const V = R.RoomView;
  const top = V.toScreen(t, 1.6, 0), bottom = V.toScreen(t, -0.4, 0);
  assert.ok(top[1] < bottom[1], "+x is up");
  const right = V.toScreen(t, 0, -0.5), left = V.toScreen(t, 0, 0.5);
  assert.ok(right[0] > left[0], "-y is right");
  for (const p of [V.toScreen(t, -0.4, -0.5), V.toScreen(t, 1.6, 0.5)]) {
    assert.ok(p[0] >= 0 && p[0] <= 400 && p[1] >= 0 && p[1] <= 400, "inside: " + p);
  }
  const tiny = new R.RoomModel();
  tiny.applyMap({ cell_m: 0.05, x0: 0, y0: 0, w: 2, h: 2 }, new Uint8Array(4).fill(2));
  const tt = V.fit(tiny.state, 400, 400);
  assert.ok(400 / tt.scale >= 2 - 1e-9, "at least 2 m shown");
});

test("toRoom undoes toScreen", () => {
  const t = R.RoomView.fit(mapped().state, 300, 500);
  const p = R.RoomView.toScreen(t, 0.7, -0.3);
  const q = R.RoomView.toRoom(t, p[0], p[1]);
  assert.ok(Math.abs(q[0] - 0.7) < 1e-9 && Math.abs(q[1] + 0.3) < 1e-9);
});

test("hit: a pin, then a name, then a block, else none; hidden layers not hit", () => {
  const room = (m) => m.applyRoom({ things: [{ word: "ball", x: 1.0, y: 0.0, ago_s: 5, where: "" }],
    names: [{ id: 3, name: "bed", x: 1.0, y: 0.0, go: null }], trail: [], why: {} });
  const m = mapped(room);
  const V = R.RoomView, t = V.fit(m.state, 400, 400);
  const at = V.toScreen(t, 1.0, 0.0);
  assert.equal(V.hit(m.state, t, at[0], at[1]).kind, "pin");
  m.setLayer("things", false);
  assert.equal(V.hit(m.state, t, at[0], at[1]).kind, "name");
  assert.equal(V.hit(m.state, t, at[0], at[1]).ref.id, 3);
  m.setLayer("names", false);
  const b = V.hit(m.state, t, at[0], at[1]);
  assert.equal(b.kind, "block");
  assert.ok(Math.abs(b.ref.x - 1.0) < 0.05 && Math.abs(b.ref.y) < 0.05);
  const near = V.toScreen(t, 1.0, 0.2);  // 10 cm from the block's edge
  assert.equal(V.hit(m.state, t, near[0], near[1]).kind, "block");
  const floor = V.toScreen(t, 0.2, 0.3);
  assert.equal(V.hit(m.state, t, floor[0], floor[1]).kind, "none");
});

test("draw: an empty room and a full one draw without throwing", () => {
  const calls = [];
  const ctx = new Proxy({}, { get: (o, k) => (k in o ? o[k] : (...a) => { calls.push(k); }), set: (o, k, v) => { o[k] = v; return true; } });
  const colours = { floor: "#111", before: "#222", low: "#f80", high: "#f00", trail: "#0af", route: "#a0f",
    area: "#fb0", pin: "#fff", cone: "rgba(0,255,0,.2)", text: "#fff", bg: "#000" };
  const empty = new R.RoomModel();
  R.RoomView.draw(ctx, empty.state, R.RoomView.fit(empty.state, 200, 200), colours);
  const m = mapped((m) => m.applyRoom({ trail: [[0, 0], [0.5, 0.1]], home: "room2", area: { x: 0, y: 0, r: 1 },
    things: [{ word: "ball", x: 1, y: 0.3, ago_s: 1, where: "" }], names: [{ id: 1, name: "bed", x: 1, y: 0, go: [0.6, 0] }],
    why: { route: [[0, 0], [1, 0]], pending: [[1.2, 0.3]], check: [1.2, 0.3], looked: [[0.5, 0]] } }));
  m.setLayer("why", true);
  R.RoomView.draw(ctx, m.state, R.RoomView.fit(m.state, 200, 200), colours);
  assert.ok(calls.includes("fillRect") && calls.includes("arc") && calls.includes("fillText"));
});

test("splitMap: a map frame splits; a camera frame is not one", () => {
  function frame(head, body) {
    const h = new TextEncoder().encode(JSON.stringify(head)), b = new Uint8Array(4 + h.length + body.length);
    new DataView(b.buffer).setUint32(0, h.length); b.set(h, 4); b.set(body, 4 + h.length);
    return b.buffer;
  }
  const png = new Uint8Array([137, 80, 78, 71, 1, 2]);
  const m = R.splitMap(frame({ kind: "map", seq: 1, cell_m: 0.05, x0: 0, y0: 0, w: 1, h: 1, png: png.length }, png));
  assert.equal(m.header.kind, "map");
  assert.deepEqual(Array.from(m.png), Array.from(png));
  assert.equal(R.splitMap(frame({ seq: 1, t_ms: 5, boxes: [], cam: 2, depth: 0 }, new Uint8Array([255, 216]))), null);
  assert.equal(R.splitMap(new ArrayBuffer(2)), null);
});

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

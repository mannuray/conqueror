// The /ai page - tests of the deciding code (bridge LLD R27.4, Plan Z2b-1 Task 10).
// Run: node --test firmware/esp32/web/test/ai.test.js
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const zlib = require("node:zlib");
const A = require("../ai.js");

// A frame as the engine builds it (AI engine LLD §22.2): 4-byte big-endian header length, header JSON, camera, depth.
function frame(header, cam, depth) {
  const head = Buffer.from(JSON.stringify(Object.assign({}, header, { cam: cam.length, depth: depth ? depth.length : 0 })));
  const len = Buffer.alloc(4);
  len.writeUInt32BE(head.length, 0);
  const b = Buffer.concat([len, head, Buffer.from(cam), Buffer.from(depth || [])]);
  return b.buffer.slice(b.byteOffset, b.byteOffset + b.length);
}

test("splitFrame reads the header, the camera and the depth", () => {
  const f = A.splitFrame(frame({ seq: 3, t_ms: 1200, boxes: [["toy", 0.9, 100, 100, 100, 100]] }, [0xff, 0xd8, 1], [0xff, 0xd8]));
  assert.equal(f.header.seq, 3);
  assert.deepEqual(f.header.boxes[0], ["toy", 0.9, 100, 100, 100, 100]);
  assert.deepEqual(Array.from(f.cam), [0xff, 0xd8, 1]);
  assert.deepEqual(Array.from(f.depth), [0xff, 0xd8]);
});

test("splitFrame without depth gives null", () => {
  const f = A.splitFrame(frame({ seq: 4 }, [1, 2], null));
  assert.equal(f.depth, null);
  assert.equal(f.cam.length, 2);
});

// A frame with a class picture after the depth (AI engine LLD §26.2, final review I1): header marks [w, h, n], then n
// bytes of zlib-packed classes, row by row.
function marksFrame(header, cam, depth, w, h, marks) {
  const z = zlib.deflateSync(Buffer.from(marks));
  const head = Buffer.from(JSON.stringify(Object.assign({}, header, { cam: cam.length, depth: depth.length, marks: [w, h, z.length] })));
  const len = Buffer.alloc(4);
  len.writeUInt32BE(head.length, 0);
  const b = Buffer.concat([len, head, Buffer.from(cam), Buffer.from(depth), z]);
  return b.buffer.slice(b.byteOffset, b.byteOffset + b.length);
}

test("splitFrame finds the packed class picture after the depth; unpackMarks opens it", async () => {
  const f = A.splitFrame(marksFrame({ seq: 5 }, [1, 2], [3], 2, 1, [2, 3]));
  assert.deepEqual(Array.from(f.depth), [3]);
  assert.equal(f.marks.w, 2);
  assert.equal(f.marks.h, 1);
  assert.deepEqual(Array.from(await A.unpackMarks(f.marks)), [2, 3]);
});

test("splitFrame without marks gives null, a short class picture is refused", () => {
  assert.equal(A.splitFrame(frame({ seq: 4 }, [1, 2], [3])).marks, null);
  const f = marksFrame({ seq: 5 }, [1, 2], [3], 2, 2, [2, 3, 1, 1]);
  assert.throws(() => A.splitFrame(f.slice(0, f.byteLength - 1)));
});

test("a class picture of a size no engine sends is dropped, the picture kept (final review M4)", async () => {
  for (const [w, h] of [[0, 3], [-2, 3], [1000, 1000]]) {
    const f = A.splitFrame(marksFrame({ seq: 6 }, [1, 2], [3], w, h, [1, 1]));
    assert.equal(f.marks, null);
    assert.equal(f.cam.length, 2);
  }
  // packed bytes that open to the wrong size: no marks
  const f = A.splitFrame(marksFrame({ seq: 7 }, [1, 2], [3], 2, 2, [1, 1, 1]));
  assert.equal(await A.unpackMarks(f.marks), null);
});

test("MARKS shows the depth until the engine has sent any marks (an old engine, final review M3)", () => {
  assert.equal(A.drawsDepth("depth", true), true);
  assert.equal(A.drawsDepth("marks", false), true);
  assert.equal(A.drawsDepth("marks", true), false);
});

test("marksColours: nothing and above clear, floor faint green, low red, high orange", () => {
  const c = A.marksColours(new Uint8Array([0, 1, 2, 3, 4]), 5, 1);
  assert.equal(c.length, 20);
  const px = (i) => Array.from(c.slice(i * 4, i * 4 + 4));
  assert.equal(px(0)[3], 0);
  assert.equal(px(4)[3], 0);
  const [floor, low, high] = [px(1), px(2), px(3)];
  assert.ok(floor[1] > floor[0] && floor[1] > floor[2] && Math.abs(floor[3] - 64) <= 2);  // green ~25 %
  assert.ok(low[0] > 200 && low[1] < 80 && Math.abs(low[3] - 179) <= 2);  // red ~70 %
  assert.ok(high[0] > 200 && high[1] > 100 && high[1] < 200 && high[2] < 80 && Math.abs(high[3] - 179) <= 2);  // orange
});

test("the VIEW B head line follows the layer; MARKS is the default", () => {
  assert.equal(A.layerHead("marks"), "VIEW B // WHAT IT SEES");
  assert.equal(A.layerHead("depth"), "VIEW B // DEPTH MAP");
  assert.equal(A.pickLayer(null), "marks");
  assert.equal(A.pickLayer("depth"), "depth");
  assert.equal(A.pickLayer("rubbish"), "marks");
});

test("splitFrame refuses a short buffer", () => {
  assert.throws(() => A.splitFrame(new ArrayBuffer(2)));
  const f = frame({ seq: 1 }, [1, 2, 3], null);
  assert.throws(() => A.splitFrame(f.slice(0, f.byteLength - 1)));
});

test("the buttons' one-line programs", () => {
  assert.equal(A.findLine("toy", true, 2), 'AREA 2  FIND "toy');
  assert.equal(A.findLine("toy", true, 1.5), 'AREA 1.5  FIND "toy');
  assert.equal(A.findLine("toy", false, 2), 'FIND "toy');
  assert.equal(A.wanderLine(30), "WANDER 30");
  assert.equal(A.exploreLine(), "EXPLORE");
});

const THINGS = [
  { word: "toy", names: ["car", "racing car"], measured: true },
  { word: "cup", names: ["mug"], measured: false },
];

test("typed: a word or a name runs as FIND, anything else is unknown", () => {
  assert.deepEqual(A.typed("  Racing   Car ", THINGS), { kind: "run", word: "toy", measured: true });
  assert.deepEqual(A.typed("mug", THINGS), { kind: "run", word: "cup", measured: false });
  assert.deepEqual(A.typed("dragon", THINGS), { kind: "unknown", word: "dragon" });
  assert.deepEqual(A.typed("person", THINGS), { kind: "unknown", word: "person" });
  assert.equal(A.unknownText("dragon"), "I don't know what a dragon is.");
});

test("teachCheck: the engine's rules and words", () => {
  assert.equal(A.teachCheck("cup", ["mug"]), null);
  assert.equal(A.teachCheck("Cup Holder", []), null);
  assert.equal(A.teachCheck("cup!", []), "Use letters, spaces and - only.");
  assert.equal(A.teachCheck("", []), "Use letters, spaces and - only.");
  assert.equal(A.teachCheck("a".repeat(25), []), "That word is too long.");
  assert.equal(A.teachCheck("cup", ["a", "b", "c", "d", "e", "f", "g", "h", "i"]), "At most 8 names.");
  assert.equal(A.teachCheck("person", []), "I never look for people.");
  assert.equal(A.teachCheck("cup", ["mug2"]), "Use letters, spaces and - only.");
  // design-notes §203: person words are refused too (a second check; the detector always reports a person)
  assert.equal(A.teachCheck("kid", []), "I never look for people.");
  assert.equal(A.teachCheck("cup", ["baby"]), "I never look for people.");
  assert.equal(A.teachCheck("little boy", []), "I never look for people.");
});

test("sameFeed: an ai_at for the feed already open changes nothing (final review I2)", () => {
  assert.equal(A.sameFeed("ws://a:8770/feed", 1, "ws://a:8770/feed", 1), true);
  assert.equal(A.sameFeed("ws://a:8770/feed", 1, "ws://b:8770/feed", 1), false);
  assert.equal(A.sameFeed(null, 0, "ws://a:8770/feed", 1), false);
});

test("Sayer: nothing before unlock, each sentence once, quiet while muted", () => {
  const said = [];
  const s = new A.Sayer((t) => said.push(t));
  s.say("Ready to play!");
  assert.deepEqual(said, []);  // iOS: speech only after a tap
  s.unlock();
  s.say("Ready to play!");
  s.say("Ready to play!");
  s.say("I'm searching for the toy.");
  assert.deepEqual(said, ["Ready to play!", "I'm searching for the toy."]);
  s.mute(true);
  s.say("I found the toy!");
  s.mute(false);
  s.say("I found the toy!");  // said while muted: not said again
  assert.deepEqual(said, ["Ready to play!", "I'm searching for the toy."]);
});

test("cameraState follows the R27.4 diagram", () => {
  let s = "NoAI";
  s = A.cameraState(s, "ai_at_url"); assert.equal(s, "NoAI");  // the feed opens next
  s = A.cameraState(s, "stream_ok"); assert.equal(s, "TankStream");
  s = A.cameraState(s, "feed_open"); assert.equal(s, "Engine");
  s = A.cameraState(s, "feed_closed"); assert.equal(s, "NoAI");
  s = A.cameraState("Engine", "ai_at_null"); assert.equal(s, "NoAI");
  s = A.cameraState("TankStream", "ai_at_null"); assert.equal(s, "TankStream");
  s = A.cameraState("Engine", "stream_ok"); assert.equal(s, "Engine");  // the engine's views win
});

test("protoOk: only version 1", () => {
  assert.equal(A.protoOk(1), true);
  assert.equal(A.protoOk(2), false);
  assert.equal(A.protoOk(undefined), false);
  assert.equal(A.PROTO_TEXT, "Please update the AI helper.");
});

test("boxRect: 0..1000 boxes to canvas pixels", () => {
  assert.deepEqual(A.boxRect(["toy", 0.9, 100, 250, 500, 500], 320, 240), { word: "toy", score: 0.9, x: 32, y: 60, w: 160, h: 120 });
});

// the battery rules moved to strip.test.js (bridge LLD R30)

test("pickVoice: a voice on the device first, the page's language first", () => {
  const v = [
    { name: "Google UK English", lang: "en-GB", localService: false },
    { name: "Samantha", lang: "en-US", localService: true },
    { name: "Daniel", lang: "en-GB", localService: true },
    { name: "Amelie", lang: "fr-CA", localService: true },
  ];
  assert.equal(A.pickVoice(v, "en-GB").name, "Daniel");
  assert.equal(A.pickVoice(v, "en-US").name, "Samantha");
  assert.equal(A.pickVoice(v, "de-DE").name, "Samantha");  // no German: an English voice on the device
  assert.equal(A.pickVoice([{ name: "Google US", lang: "en-US", localService: false }], "en-US").name, "Google US");
  assert.equal(A.pickVoice([], "en-US"), null);
});

// ---------- the GAMES card on PLAY (Plan G3, design-notes §224, bridge LLD R34) ----------
const LESSON = "; Lesson 6 - chase with the baby\nTO CHASE :cm :near :far\n  WAIT 1\nEND\nBABYGAME 70 100 5\n";

test("gameMoment: no helper, locked, ready", () => {
  assert.deepEqual(A.gameMoment({ feed: false, people: true, state: "off", running: false }).st, "NO AI HELPER");
  const locked = A.gameMoment({ feed: true, people: false, state: "off", running: false });
  assert.equal(locked.st, "LOCKED"); assert.equal(locked.canStart, false); assert.match(locked.say, /seeing test/);
  const ready = A.gameMoment({ feed: true, people: true, state: "off", running: false });
  assert.equal(ready.st, "READY"); assert.equal(ready.canStart, true);
  assert.equal(A.gameMoment({ feed: true, people: true, state: "off", running: true }).canStart, false);  // a word runs
});

test("gameMoment: playing with the baby's distance, caught, holding still", () => {
  const p = A.gameMoment({ feed: true, people: true, state: "playing", running: true, person: { seen: true, d_cm: 72, coming: 1 } });
  assert.equal(p.st, "PLAYING"); assert.match(p.say, /72 cm/); assert.match(p.say, /coming/); assert.equal(p.canStart, false);
  assert.match(A.gameMoment({ feed: true, people: true, state: "playing", running: true, person: { seen: false, d_cm: -1, coming: 0 } }).say, /looking/i);
  assert.equal(A.gameMoment({ feed: true, people: true, state: "arming", running: true }).st, "PLAYING");
  const c = A.gameMoment({ feed: true, people: true, state: "caught", running: true, caught: 2 });
  assert.equal(c.st, "CAUGHT!"); assert.match(c.say, /2/);
  const h = A.gameMoment({ feed: false, people: true, state: "held", running: false });
  assert.equal(h.st, "HOLDING STILL"); assert.match(h.say, /STOP/);  // held wins even when the helper is gone
});

test("babyLine puts the grown-ups' numbers in the call and keeps the rest", () => {
  const out = A.babyLine(LESSON, 60, 120, 3);
  assert.ok(out.startsWith("; Lesson 6"));
  assert.ok(out.includes("TO CHASE :cm :near :far"));
  assert.ok(out.trimEnd().endsWith("BABYGAME 60 120 3"));
  assert.equal(out.match(/BABYGAME/g).length, 1);
});

test("babyLine clamps: never under 40 cm, follow beyond back-off, at most 14 minutes", () => {
  assert.ok(A.babyLine(LESSON, 10, 20, 99).trimEnd().endsWith("BABYGAME 40 50 14"));
  assert.ok(A.babyLine(LESSON, "x", null, 0).trimEnd().endsWith("BABYGAME 60 80 1"));  // §248
});

test("babyLine: the follow distance stays beyond the back-off one even left empty (G3 final review M1)", () => {
  assert.ok(A.babyLine(LESSON, 150, "", 5).trimEnd().endsWith("BABYGAME 150 160 5"));
});

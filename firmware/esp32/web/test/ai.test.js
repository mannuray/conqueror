// The /ai page - tests of the deciding code (bridge LLD R27.4, Plan Z2b-1 Task 10).
// Run: node --test firmware/esp32/web/test/ai.test.js
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
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

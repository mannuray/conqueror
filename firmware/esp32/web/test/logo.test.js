// Tank Logo page - tests of the deciding code (Tank Logo LLD §21.9).
// Run: node --test firmware/esp32/web/test/logo.test.js
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const path = require("node:path");
const L = require("../logo.js");
const WORDS = require("../generated/words.json");

const idx = L.wordIndex(WORDS.words, WORDS.presets);

// the class of each non-space token, as "text:cls"
function classes(text, own) {
  return L.colour(text, idx, own)
    .filter((s) => s.cls !== "sp")
    .map((s) => text.slice(s.from, s.to) + ":" + s.cls);
}

test("each kind of word gets its group's colour", () => {
  assert.deepEqual(classes("FD 50 RT 90 REPEAT LED BEEP DISTANCE AND"), [
    "FD:g-move", "50:num", "RT:g-turn", "90:num", "REPEAT:g-flow", "LED:g-show", "BEEP:g-show",
    "DISTANCE:g-sense", "AND:g-maths",
  ]);
});

test("words are found in any case and by alias", () => {
  assert.deepEqual(classes("forward fd Forward"), ["forward:g-move", "fd:g-move", "Forward:g-move"]);
});

test("your own words, inputs, comments and brackets", () => {
  const text = "TO SQUARE :size\n  REPEAT 4 [ FD :size RT 90 ] ; four sides\nEND\nSQUARE 50";
  assert.deepEqual(classes(text), [
    "TO:g-flow", "SQUARE:own", ":size:inp", "REPEAT:g-flow", "4:num", "[:op", "FD:g-move", ":size:inp",
    "RT:g-turn", "90:num", "]:op", "; four sides:cmt", "END:g-flow", "SQUARE:own", "50:num",
  ]);
});

test("a word the check reported as your own is not underlined", () => {
  assert.deepEqual(classes("ZIGZAG 3", { ZIGZAG: 1 }), ["ZIGZAG:own", "3:num"]);
});

test("an unknown word is underlined, a preset is a value", () => {
  assert.deepEqual(classes("FORWAD 50 LED GREEN"), ["FORWAD:bad", "50:num", "LED:g-show", "GREEN:num"]);
});

test("SAY and PLAY lists are text, not words", () => {
  assert.deepEqual(classes("SAY [ well done ] PLAY [ C4 E4 2 ]"), [
    "SAY:g-show", "[:op", "well:str", "done:str", "]:op", "PLAY:g-show", "[:op", "C4:str", "E4:str", "2:str", "]:op",
  ]);
});

test("the colour spans cover the whole text", () => {
  const text = "REPEAT 4 [FD 50 RT 90]\n; end";
  const spans = L.colour(text, idx);
  assert.equal(spans[0].from, 0);
  assert.equal(spans[spans.length - 1].to, text.length);
  spans.slice(1).forEach((s, i) => assert.equal(s.from, spans[i].to));
});

function word(name) { return WORDS.words.find((w) => w.name === name); }

test("FORWARD goes in with its number selected", () => {
  const r = L.insertWord("", 0, 0, word("FORWARD"));
  assert.equal(r.text, "FORWARD 50");
  assert.equal(r.text.slice(r.selStart, r.selEnd), "50");
});

test("REPEAT goes in with the cursor inside the brackets", () => {
  const r = L.insertWord("", 0, 0, word("REPEAT"));
  assert.equal(r.text, "REPEAT 4 [  ]");
  assert.equal(r.selStart, r.selEnd);
  assert.equal(r.text.slice(0, r.selStart), "REPEAT 4 [ ");
});

test("a word inside brackets stays on the line, after other text it starts a new line with the same indent", () => {
  const inside = L.insertWord("REPEAT 4 [  ]", 11, 11, word("FORWARD"));
  assert.equal(inside.text, "REPEAT 4 [ FORWARD 50 ]");
  const after = L.insertWord("  FD 10", 7, 7, word("RIGHT"));
  assert.equal(after.text, "  FD 10\n  RIGHT 90");
  assert.equal(after.text.slice(after.selStart, after.selEnd), "90");
});

test("LED selects the colour, PLAY selects the notes, a reporter goes in as one word", () => {
  const led = L.insertWord("", 0, 0, word("LED"));
  assert.equal(led.text.slice(led.selStart, led.selEnd), "GREEN");
  const play = L.insertWord("", 0, 0, word("PLAY"));
  assert.equal(play.text, "PLAY [ C4 E4 G4 2 ]");
  assert.equal(play.text.slice(play.selStart, play.selEnd), "C4 E4 G4 2");
  const dist = L.insertWord("IF ", 3, 3, word("DISTANCE"));
  assert.equal(dist.text, "IF DISTANCE");
  assert.equal(dist.selStart, dist.text.length);
});

test("a selection is replaced, and a new line leaves no spaces behind", () => {
  const r = L.insertWord("FD 10 XX", 6, 8, word("BEEP"));
  assert.equal(r.text, "FD 10\nBEEP");
  assert.equal(r.selStart, r.text.length);
});

function memory() {
  const m = new Map();
  return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, String(v)), removeItem: (k) => m.delete(k), m };
}

test("a draft is kept, brought back and cleared", () => {
  const s = memory();
  const d = new L.Drafts(s);
  d.keep("square", "FD 60", "FD 50");
  assert.ok(d.unsaved("square"));
  assert.deepEqual(new L.Drafts(s).restore("square"), { text: "FD 60", saved: "FD 50" });
  d.clear("square");
  assert.equal(d.restore("square"), null);
});

test("text equal to the saved text is no draft", () => {
  const d = new L.Drafts(memory());
  d.keep("square", "FD 60", "FD 50");
  d.keep("square", "FD 50", "FD 50");
  assert.equal(d.unsaved("square"), false);
});

test("storage that throws (private mode) only turns drafts off", () => {
  const bad = { getItem() { throw new Error("no"); }, setItem() { throw new Error("no"); }, removeItem() { throw new Error("no"); } };
  const d = new L.Drafts(bad);
  assert.doesNotThrow(() => d.keep("a", "x", "y"));
  assert.equal(d.restore("a"), null);
  assert.doesNotThrow(() => d.clear("a"));
  assert.equal(new L.Drafts(null).unsaved("a"), false);
});

test("a broken draft is ignored", () => {
  const s = memory();
  s.setItem("cq.logo.draft.a", "{not json");
  assert.equal(new L.Drafts(s).restore("a"), null);
});

test("the view fits the whole path at one scale with the start shown", () => {
  const pts = [{ x: 0, y: 0 }, { x: 0, y: 100 }, { x: 100, y: 100 }, { x: 100, y: 0 }];
  const v = L.fitView(pts, 400, 300, 20);
  pts.forEach((p) => {
    const s = L.toScreen(v, p.x, p.y);
    assert.ok(s.x >= 20 - 1e-9 && s.x <= 380 + 1e-9 && s.y >= 20 - 1e-9 && s.y <= 280 + 1e-9, JSON.stringify(s));
  });
  assert.equal(v.scale, 260 / 100);
});

test("forward is up the screen and a right turn goes right", () => {
  const v = L.fitView([{ x: 0, y: 50 }, { x: 50, y: 50 }], 300, 300, 10);
  const start = L.toScreen(v, 0, 0), ahead = L.toScreen(v, 0, 50), right = L.toScreen(v, 50, 50);
  assert.ok(ahead.y < start.y);
  assert.ok(right.x > ahead.x);
});

test("a tiny path is not blown up and the grid follows the zoom", () => {
  const small = L.fitView([{ x: 0, y: 2 }], 300, 300, 10);
  assert.equal(small.scale, 280 / 50);
  assert.equal(small.grid, 5);
  const big = L.fitView([{ x: 0, y: 2000 }], 300, 300, 10);
  assert.ok(big.grid >= 100);
});

test("the planned path is solid up to the first sensor read, dashed after", () => {
  const runs = L.plannedRuns([[0, 0, 0, 1, 0], [0, 500, 0, 1, 0], [0, 600, 0, 2, 1], [0, 700, 0, 2, 1]]);
  assert.equal(runs.length, 2);
  assert.equal(runs[0].solid, true);
  assert.deepEqual(runs[0].pts, [{ x: 0, y: 0 }, { x: 0, y: 50 }]);
  assert.equal(runs[1].solid, false);
  assert.deepEqual(runs[1].pts[0], { x: 0, y: 50 }, "the dashed run starts where the solid one ended");
  assert.equal(runs[1].pts.length, 3);
  assert.equal(runs[1].endH, 0);
  assert.equal(L.plannedRuns([[0, 0, 0, 1, 0], [0, 0, 90, 2, 0]])[0].endH, 90, "the arrow points the way the plan ends");
});

test("a check is due 600 ms after the last key", () => {
  const c = new L.CheckScheduler(600, 500);
  c.edited(0);
  c.edited(400);
  assert.equal(c.due(900), 0);
  assert.equal(c.due(1000), 2);
  assert.equal(c.due(1001), 0, "one check at a time");
});

test("a reply for an older text is dropped and the newer text is checked next", () => {
  const c = new L.CheckScheduler(600, 500);
  c.edited(0);
  const e = c.due(600);
  c.edited(700);
  assert.equal(c.replied(e), false);
  assert.equal(c.due(1300), 2);
  assert.equal(c.replied(2), true);
});

test("busy is tried again 0.5 s later", () => {
  const c = new L.CheckScheduler(600, 500);
  c.edited(0);
  const e = c.due(600);
  c.busy(e, 650);
  assert.equal(c.due(1100), 0);
  assert.equal(c.due(1150), e);
});

test("check at once", () => {
  const c = new L.CheckScheduler(600, 500);
  c.edited(0);
  c.now(10);
  assert.equal(c.due(10), 1);
});

function run(events, s = L.runBarStart()) {
  return events.reduce((st, ev) => L.runBar(st, ev), s);
}

test("RUN only after an OK check", () => {
  let s = run([{ e: "edit" }, { e: "checking" }, { e: "checked", ok: false, prog: 3 }]);
  assert.equal(s.st, "mistakes");
  assert.equal(L.canRun(s), false);
  assert.equal(L.runBar(s, { e: "run" }).act, null);
  s = run([{ e: "edit" }, { e: "checking" }, { e: "checked", ok: true, prog: 4 }], s);
  assert.equal(s.st, "ready");
  const r = L.runBar(s, { e: "run", step: false });
  assert.equal(r.st, "starting");
  assert.equal(r.act, "run");
});

test("a run: ack, lines, finished", () => {
  let s = run([{ e: "checked", ok: true, prog: 4 }, { e: "run" }, { e: "ack" }]);
  assert.equal(s.st, "running");
  assert.ok(L.locked(s));
  s = run([{ e: "line", line: 3, round: 2 }], s);
  assert.deepEqual([s.line, s.round], [3, 2]);
  s = run([{ e: "ended", reason: "finished", line: 9, msg: "" }], s);
  assert.equal(s.st, "ended");
  assert.equal(L.locked(s), false);
  assert.equal(L.endText(s.end), "Done");
  assert.ok(L.canRun(s), "run the same program again");
});

test("an edit while running changes nothing", () => {
  const s = run([{ e: "checked", ok: true, prog: 4 }, { e: "run" }, { e: "ack" }, { e: "edit" }]);
  assert.equal(s.st, "running");
  assert.equal(s.prog, 4);
});

test("step mode: a pause is a line message with a new line; the move on that line runs", () => {
  // the bridge's order (RunController): pause at line 1, then FD on line 1, RT on line 1, pause at line 2
  let s = run([{ e: "checked", ok: true, prog: 4 }, { e: "run", step: true }, { e: "ack" }, { e: "line", line: 1, round: 0 }]);
  assert.equal(s.st, "stepping");
  s = L.runBar(s, { e: "step" });
  assert.equal(s.st, "running");
  assert.equal(s.act, "step");
  s = run([{ e: "line", line: 1, round: 0 }, { e: "line", line: 1, round: 0 }], s);
  assert.equal(s.st, "running", "the moves on the paused line do not light NEXT");
  assert.equal(L.runBar(s, { e: "step" }).act, null, "no STEP is sent while the tank moves");
  assert.equal(run([{ e: "line", line: 2, round: 0 }], s).st, "stepping");
});

test("step mode: REPEAT rounds on one line pause once", () => {
  let s = run([{ e: "checked", ok: true, prog: 4 }, { e: "run", step: true }, { e: "ack" }, { e: "line", line: 1, round: 1 }, { e: "step" }]);
  s = run([{ e: "line", line: 1, round: 1 }, { e: "line", line: 1, round: 2 }, { e: "line", line: 1, round: 3 }], s);
  assert.equal(s.st, "running");
  assert.equal(s.round, 3);
});

test("RUN straight through never stops at lines", () => {
  const s = run([{ e: "checked", ok: true, prog: 4 }, { e: "run", step: false }, { e: "ack" }, { e: "line", line: 1, round: 0 }, { e: "line", line: 2, round: 0 }]);
  assert.equal(s.st, "running");
});

test("CHECK_AGAIN checks once and runs again; a second CHECK_AGAIN stops trying", () => {
  let s = run([{ e: "checked", ok: true, prog: 4 }, { e: "run" }, { e: "nack", reason: "CHECK_AGAIN" }]);
  assert.equal(s.st, "checking");
  assert.equal(s.act, "check");
  s = L.runBar(s, { e: "checked", ok: true, prog: 5 });
  assert.equal(s.st, "starting");
  assert.equal(s.act, "run");
  assert.equal(s.prog, 5);
  s = L.runBar(s, { e: "nack", reason: "CHECK_AGAIN", message: "The program changed - check it again" });
  assert.equal(s.st, "ready");
  assert.equal(s.act, null);
  assert.equal(s.msg, "The program changed - check it again");
});

test("CHECK_AGAIN, then the new check has mistakes: no run", () => {
  const s = run([{ e: "checked", ok: true, prog: 4 }, { e: "run" }, { e: "nack", reason: "CHECK_AGAIN" }, { e: "checked", ok: false, prog: 5 }]);
  assert.equal(s.st, "mistakes");
  assert.equal(s.act, null);
});

test("other refusals are a sentence", () => {
  const s = run([{ e: "checked", ok: true, prog: 4 }, { e: "run" }, { e: "nack", reason: "NOT_OWNER", message: "Another source has control" }]);
  assert.equal(s.st, "ready");
  assert.equal(s.msg, "Someone else is driving the tank.");
});

test("another device's program: RUN off until it ends, our STOP still goes", () => {
  let s = run([{ e: "checked", ok: true, prog: 4 }, { e: "pose" }]);
  assert.equal(s.other, true);
  assert.equal(L.canRun(s), false);
  assert.equal(L.runBar(s, { e: "stop" }).act, "stop");
  s = run([{ e: "ended", reason: 7040, line: 2, msg: "Stopped" }], s);
  assert.equal(s.other, false);
  assert.equal(s.st, "ready", "their ending is not ours");
  assert.ok(L.canRun(s));
});

test("our own events are not another device's program", () => {
  const s = run([{ e: "checked", ok: true, prog: 4 }, { e: "run" }, { e: "started" }, { e: "pose" }, { e: "ack" }, { e: "pose" }]);
  assert.equal(s.other, false);
});

test("a hidden page ends our run with 7046 and says so", () => {
  const s = run([{ e: "checked", ok: true, prog: 4 }, { e: "run" }, { e: "ack" }, { e: "hidden" }]);
  assert.equal(s.st, "ended");
  assert.equal(s.end.reason, 7046);
  assert.match(L.endText(s.end), /7046.*hidden/);
});

test("an ending with a code names the line", () => {
  assert.equal(L.endText({ reason: 7024, line: 3, msg: "Something is in the way on line 3." }),
    "Ended (7024) on line 3: Something is in the way on line 3.");
});

test("the word list is the generated one, with a group for every word", () => {
  assert.ok(WORDS.words.length > 50);
  WORDS.words.forEach((w) => assert.ok(["move", "turn", "flow", "show", "sense", "maths"].includes(w.group), w.name));
  assert.equal(path.basename(require.resolve("../generated/words.json")), "words.json");
});

// a fetch stand-in: answers from a list, one per call; records the calls and how many ran at once
function fakeFetch(answers) {
  const calls = [];
  let open = 0;
  const f = (path, opts) => {
    calls.push({ path, method: opts.method, body: opts.body, together: ++open });
    const a = answers.shift() || { status: 200, json: {} };
    const headers = { get: () => ("json" in a ? "application/json" : "text/plain") };
    return new Promise((resolve) => setTimeout(() => {
      open--;
      resolve({ status: a.status, headers, json: () => Promise.resolve(a.json), text: () => Promise.resolve(a.text) });
    }, 2));
  };
  return { f, calls };
}
// the 0.5 s retry made short; a long timer (the 15 s timeout) kept, and never holding Node open
const soon = (fn, ms) => (ms >= 1000 ? setTimeout(fn, ms).unref() : setTimeout(fn, Math.min(ms, 3)));

test("requests go one at a time", async () => {
  const ff = fakeFetch([{ status: 200, json: { a: 1 } }, { status: 200, text: "FD 50" }]);
  const q = new L.RequestQueue(ff.f, soon, 500, 20);
  const [a, b] = await Promise.all([q.send("GET", "/logo/programs"), q.send("GET", "/logo/programs/square")]);
  assert.deepEqual(a, { status: 200, data: { a: 1 } });
  assert.deepEqual(b, { status: 200, data: "FD 50" });
  assert.deepEqual(ff.calls.map((c) => c.together), [1, 1]);
});

test("busy is tried again, then the answer comes back", async () => {
  const ff = fakeFetch([{ status: 503, json: { ok: false, why: "BUSY" } }, { status: 200, json: { ok: true } }]);
  const q = new L.RequestQueue(ff.f, soon, 500, 20);
  const r = await q.send("PUT", "/logo/programs/square", "FD 50");
  assert.deepEqual(r, { status: 200, data: { ok: true } });
  assert.equal(ff.calls.length, 2);
  assert.equal(ff.calls[1].body, "FD 50", "the same body again");
});

test("busy too long is given back as 503, and the queue goes on", async () => {
  const ff = fakeFetch([{ status: 503, json: {} }, { status: 503, json: {} }, { status: 200, json: { x: 2 } }]);
  const q = new L.RequestQueue(ff.f, soon, 500, 2);
  const first = q.send("POST", "/logo/check", "FD 1");
  const second = q.send("GET", "/logo/programs");
  assert.equal((await first).status, 503);
  assert.deepEqual(await second, { status: 200, data: { x: 2 } });
});

test("a plain-text body is never parsed as JSON, and no link gives status 0", async () => {
  const ff = fakeFetch([{ status: 200, text: "{not json" }]);
  const q = new L.RequestQueue(ff.f, soon, 500, 20);
  assert.deepEqual(await q.send("GET", "/logo/programs/x"), { status: 200, data: "{not json" });
  const down = new L.RequestQueue(() => Promise.reject(new Error("offline")), soon, 500, 20);
  assert.deepEqual(await down.send("GET", "/logo/programs"), { status: 0, data: null });
});

test("the retry timer is called on its own, like a browser's setTimeout needs", async () => {
  const ff = fakeFetch([{ status: 503, json: {} }, { status: 200, json: { ok: true } }]);
  const strictLater = function (fn, ms) {
    "use strict";
    assert.equal(this, undefined, "a browser throws Illegal invocation when setTimeout gets another this");
    soon(fn, ms);
  };
  const r = await new L.RequestQueue(ff.f, strictLater, 500, 20).send("PUT", "/logo/programs/a", "FD 1");
  assert.equal(r.status, 200);
});

test("an edit after CHECK_AGAIN cancels the run (nobody pressed RUN for the new text)", () => {
  let s = run([{ e: "checked", ok: true, prog: 4 }, { e: "run" }, { e: "nack", reason: "CHECK_AGAIN" }, { e: "edit" }]);
  s = L.runBar(s, { e: "checked", ok: true, prog: 6 });
  assert.equal(s.st, "ready");
  assert.equal(s.act, null);
});

test("a stored draft counts as unsaved without being read", () => {
  const s = memory();
  s.setItem("cq.logo.draft.a", "{not json");
  assert.equal(new L.Drafts(s).unsaved("a"), true);
  assert.equal(new L.Drafts(s).unsaved("b"), false);
});

test("a request that never answers gives up after the timeout and the queue goes on", async () => {
  let n = 0;
  const f = () => (++n === 1 ? new Promise(() => {}) : Promise.resolve({ status: 200, headers: { get: () => "application/json" }, json: () => Promise.resolve({ ok: 1 }) }));
  const q = new L.RequestQueue(f, soon, 500, 20, 30);
  const first = q.send("POST", "/logo/check", "FD 1");
  const second = q.send("GET", "/logo/programs");
  assert.deepEqual(await first, { status: 0, data: null });
  assert.deepEqual(await second, { status: 200, data: { ok: 1 } });
});

// ---------- revision N6: the laptop layout (Tank Logo LLD §23) ----------
test("the four shortcuts: Esc, Ctrl/Cmd + Enter, F10, Ctrl/Cmd + S", () => {
  assert.equal(L.shortcut("Escape", false, false, false, false, false), "stop");
  assert.equal(L.shortcut("Enter", true, false, false, false, false), "run");
  assert.equal(L.shortcut("Enter", false, true, false, false, false), "run");
  assert.equal(L.shortcut("F10", false, false, false, false, false), "step");
  assert.equal(L.shortcut("s", true, false, false, false, false), "save");
  assert.equal(L.shortcut("S", false, true, false, true, false), "save");
});

test("a held key only ever stops; plain keys and Alt do nothing", () => {
  assert.equal(L.shortcut("Escape", false, false, false, false, true), "stop");
  assert.equal(L.shortcut("Enter", true, false, false, false, true), null);
  assert.equal(L.shortcut("F10", false, false, false, false, true), null);
  assert.equal(L.shortcut("s", true, false, false, false, true), null);
  assert.equal(L.shortcut("Enter", false, false, false, false, false), null);
  assert.equal(L.shortcut("s", false, false, false, false, false), null);
  assert.equal(L.shortcut("Enter", true, false, true, false, false), null);
  assert.equal(L.shortcut("F9", false, false, false, false, false), null);
});

test("a program start opens MESSAGES and forgets a picked tab", () => {
  const s = L.panelTab({ tab: "problems", picked: true }, { e: "started" });
  assert.deepEqual(s, { tab: "messages", picked: false });
});

test("problems open PROBLEMS only when idle and the tab was not picked", () => {
  const idle = { tab: "messages", picked: false };
  assert.equal(L.panelTab(idle, { e: "problems", n: 2, running: false }).tab, "problems");
  assert.equal(L.panelTab(idle, { e: "problems", n: 0, running: false }).tab, "messages");
  assert.equal(L.panelTab(idle, { e: "problems", n: 2, running: true }).tab, "messages");
  const picked = L.panelTab(idle, { e: "pick", tab: "messages" });
  assert.deepEqual(picked, { tab: "messages", picked: true });
  assert.equal(L.panelTab(picked, { e: "problems", n: 3, running: false }).tab, "messages");
});

test("the panel ignores other events and does not change the state it was given", () => {
  const s = { tab: "problems", picked: false };
  assert.deepEqual(L.panelTab(s, { e: "line" }), s);
  L.panelTab(s, { e: "started" });
  assert.deepEqual(s, { tab: "problems", picked: false });
});

test("the camera streams only when wide, not hidden and the page is visible", () => {
  assert.equal(L.cameraWanted(true, false, false), true);
  assert.equal(L.cameraWanted(false, false, false), false);
  assert.equal(L.cameraWanted(true, true, false), false);
  assert.equal(L.cameraWanted(true, false, true), false);
});

// ---------- revision N7: the shared Lucide icons (bridge LLD R25, Tank Logo LLD §23.9) ----------
test("the WiFi icon follows the signal as on the Drive page", () => {
  assert.equal(L.wifiIcon(-50), "wifi");
  assert.equal(L.wifiIcon(-60), "wifi-high");
  assert.equal(L.wifiIcon(-70), "wifi-low");
  assert.equal(L.wifiIcon(-80), "wifi-zero");
  assert.equal(L.wifiIcon(null), "wifi-zero");  // Drive: no reading is "no signal", not "WiFi off"
});

test("the battery icon follows the charge, and USB is a plug", () => {
  assert.equal(L.batteryIcon(8.2), "battery-full");
  assert.equal(L.batteryIcon(7.87), "battery-full");   // Drive's curve: 70 % is 7.87 V
  assert.equal(L.batteryIcon(7.86), "battery-medium");
  assert.equal(L.batteryIcon(7.6), "battery-medium");
  assert.equal(L.batteryIcon(7.1), "battery-low");
  assert.equal(L.batteryIcon(6.8), "battery-warning");
  assert.equal(L.batteryIcon(5.0), "plug");
});

test("the battery is averaged over 10 readings and starts again between USB and the pack (as Drive)", () => {
  const m = new L.BattMean(10);
  for (let i = 0; i < 10; i++) m.push(8.0);
  assert.equal(m.push(6.9).toFixed(2), "7.89");   // one sagging reading does not change the icon
  assert.equal(m.push(5.0), 5.0);                 // USB: a fresh average
  assert.equal(m.push(8.0), 8.0);                 // back on the pack: fresh again
});

test("the battery warning comes from the tank's flags, as on Drive", () => {
  assert.equal(L.battWarn(["batt_low"]), true);
  assert.equal(L.battWarn(["batt_critical"]), true);
  assert.equal(L.battWarn([]), false);
  assert.equal(L.battWarn(undefined), false);
});

# Tank Logo N4 — the `/logo` Page Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Tank Logo page on the bridge (`/logo`): write a program with a coloured, child-friendly editor, see mistakes and the planned path as you type, RUN / STEP / STOP it, watch the running line and the driven path, and keep programs on the tank (drafts in the browser).

**Architecture:** One plain HTML page (`web/logo.html`) whose deciding code lives in `web/logo.js` as plain functions tested by Node's built-in test runner; `pack_web.py` puts `logo.js` and the generated word list into the page and serves it from the existing asset table. The page uses the N3 transport unchanged: program text over HTTP (`/logo/check`, `/logo/programs`), control and events over the WebSocket (`logo_run` / `logo_step` / `logo_stop`, `logo_*` events). Nothing changes in how programs run.

**Tech Stack:** HTML / CSS / JavaScript (no framework, ES5 style like the other pages), Node ≥ 18 built-in `node:test` (no packages), Python 3.12 (`logogen`, `pack_web.py`, a stand-in tank on `http.server`), PlatformIO bridge build, Playwright for the browser check.

**Spec:** `docs/lld/tank-logo-lld.md` §21 (approved 2026-10-19), with §14 and §20; design-notes §106 (decisions); `docs/design/bridge-ux.md` §2; screens `docs/design/stitch/15-tank-logo-desktop-v2.png`, `16-tank-logo-mobile-v2.png`.

**Branch:** `plan-n4` from `main`. Task 5 (stand) only with the owner present and STOP within reach.

## Global Constraints

- Plain HTML/CSS/JS, no framework, nothing loaded from the network; `/logo` ≤ **24 KB gzipped** with the word list (`pack_web.py` fails the build above it).
- The bridge only **serves** the page (pack_web's asset table): no new WebSocket messages, no protocol change, no change in how programs run; safety unchanged.
- Generated files are never edited by hand: `firmware/esp32/web/generated/words.json` comes from `cd logo && uv run logogen write`.
- Python: 3.12, type hints, ruff + pyright strict clean (`logo/` with its own project; `web/test/fake_tank.py` checked with `tools/smoke`'s ruff and pyright).
- JavaScript tests: `node --test firmware/esp32/web/test/logo.test.js` (the file path; the folder form fails), no npm packages.
- Browser storage (`localStorage`) only for drafts and the last opened program; every read and write in `try`, the page works without it.
- No browser dialogs (`alert` / `confirm` / `prompt`): names and delete confirmations are in-page sheets.
- Hardware: stand (tracks off the ground) with the owner present; ask "stand or floor?" before every run and say what will move.
- Never print WiFi SSIDs or passwords or the OTA password; the bridge has no authentication (home network, development only).
- Commits: Conventional Commits, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Rules this plan decides (owner confirms at plan review; recorded in Task 6)

1. **Mistakes:** marked in the editor (red wavy underline under the word, ▲ and a red line number in the gutter) and written **just under the editor**, each quoting its line with a ▲ under the word; tapping one puts the cursor there. A text box cannot open a gap under one of its lines, so the sentence cannot sit inside the text (§14 said "right under the line").
2. **One `/logo` request at a time** from the page (`RequestQueue`): the bridge's logo worker takes one job and answers a second `503 busy`; the page retries a 503 every 0.5 s for up to 10 s and never parses a non-JSON body as JSON.
3. **Step mode:** a pause is a `logo_line` whose line differs from the last one; the bridge also sends `logo_line` with each move, light or sound on the paused line (`RunController`), and those do not light NEXT. `round` is 0 outside loops (no badge).
4. **A draft is never hidden:** opening a program that has a draft shows the draft, also when the tank's copy changed meanwhile (with a note "… was changed on the tank since your draft - SAVE keeps your draft").
5. **SAVE AS onto an existing program needs a second press**; an example always asks for a new name.
6. Level-3 words other than `OUTPUT` show greyed with "later" (v1 builds levels 1–2 and `OUTPUT`).
7. Two quieter colour groups besides the four approved: **sense** (light blue) and **maths** (soft white).
8. The page's ☰ MENU is a short list (Drive, Settings, Help); the Drive page's menu item LOGO is enabled (it said COMING SOON).
9. Help's Logo section is rewritten for v1 (it still showed `MAKE`, variables and "this bridge cannot run programs yet").
10. The page sends `hello` with client `logo` (the bridge only answers `welcome`); each check carries `?name=` so `logo_started` names a saved program.
11. `pack_web.py` can be loaded by the stand-in tank (`runpy`): its PlatformIO branch runs only where SCons provides `Import`.
12. `scripts/check-protocol.sh` finds `logo/` from its own folder: inside a hook, `git rev-parse --show-toplevel` gives the current folder in a worktree, and the pre-commit hook failed there.
13. No `http_server.cpp` change: the asset table registers `/logo` (at most 16 of the 20 URI handlers then in use: 6 assets, `/ws`, `/info`, `/source.tar.gz`, `/stream` and `/capture` if on the same server, 5 Logo); Task 5 step 1 checks that `/logo` answers.

## Review Focus

1. **Several `/logo` requests at once** (page open: the list, opening the last program and the first check; SAVE while a check runs; another device checking) → nothing is lost and a busy tank never shows as "not connected" — `RequestQueue` tests (Task 2), the strict stand-in tank in the browser check (Task 4 step 3), the stand (Task 5).
2. **NEXT in step mode** → lit only at a real pause, never while the tank moves — the step tests written from the bridge's event order (Task 2), the stand (Task 5 step 4).
3. **A draft against a program changed on another device** → the draft is shown with a note, not overwritten — browser check (Task 4 step 3).
4. **Opening the page while another device's program runs** (only `logo_pose` / `logo_line` arrive, never `logo_started`) → "a program is running on another device", RUN off, STOP works — `RunBar` test (Task 2), the stand (Task 5 step 6).
5. **An 8 KB program and a 2000-point path on the owner's phone** → typing keeps up (one colouring per frame), the path draws — measured in Chromium (Task 4 step 3: one colouring per frame, ≈ 11 ms for 8 KB; ≈ 1 ms per path), felt on the phone (Task 5 step 8).

## File map

| Task | Creates | Changes |
|---|---|---|
| 1 Word groups | — | `logo/spec/words.yaml`, `logo/generator/logogen/{model,emit}.py`, `logo/tests/test_logogen.py`, generated `firmware/esp32/web/generated/words.json`, `scripts/check-protocol.sh` |
| 2 Page logic | `firmware/esp32/web/logo.js`, `firmware/esp32/web/test/logo.test.js` | — |
| 3 The page | `firmware/esp32/web/logo.html`, `firmware/esp32/web/test/fake_tank.py` | `firmware/esp32/scripts/pack_web.py` |
| 4 Links, Help, browser check | — | `firmware/esp32/web/{basic_drive,help,help_logo}.html` |
| 5 Stand checks | — | — (results in design-notes, Task 6) |
| 6 Docs | — | `docs/lld/tank-logo-lld.md` (§21.10), `docs/design-notes.md` (§106–§107), `CLAUDE.md` |

---

### Task 1: A colour group per word (and the hook in worktrees)

**Files:**
- Modify: `logo/generator/logogen/model.py`, `logo/generator/logogen/emit.py`, `logo/spec/words.yaml`, `scripts/check-protocol.sh`
- Generated: `firmware/esp32/web/generated/words.json` (by `logogen write`, never by hand)
- Test: `logo/tests/test_logogen.py`

**Interfaces:**
- Produces: every word in `words.json` has `"group"`: one of `move`, `turn`, `flow`, `show`, `sense`, `maths` (Task 2 colours by it); `model.GROUPS`; `Word.group`.

- [ ] **Step 1: Write the failing tests** — add `import json` as the first line of `logo/tests/test_logogen.py` and append:

```python
def test_every_word_has_a_colour_group() -> None:
    s = spec()
    assert all(w.group in model.GROUPS for w in s.words)
    groups = {
        n: s.word(n).group for n in ("FORWARD", "RIGHT", "WAIT", "LED", "DISTANCE", "AND", "FIRST")
    }
    assert groups == {
        "FORWARD": "move",
        "RIGHT": "turn",
        "WAIT": "flow",
        "LED": "show",
        "DISTANCE": "sense",
        "AND": "maths",
        "FIRST": "maths",
    }


def test_words_json_carries_the_group() -> None:
    data = json.loads(emit.render_words_json(spec()))
    fd = next(w for w in data["words"] if w["name"] == "FORWARD")
    assert fd["group"] == "move"


def test_a_word_without_a_group_is_refused() -> None:
    with pytest.raises(ValueError, match="FD2: unknown group"):
        model._load_words([{"name": "FD2", "kind": "command", "level": 1}])  # pyright: ignore[reportPrivateUsage]
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd logo && uv run pytest -q tests`
Expected: 3 failed (`AttributeError: 'Word' object has no attribute 'group'`, `KeyError: 'group'`, `DID NOT RAISE`), 11 passed.

- [ ] **Step 3: The model and the JSON** — apply:

```diff
diff --git a/logo/generator/logogen/emit.py b/logo/generator/logogen/emit.py
index edf83ad..34cb334 100644
--- a/logo/generator/logogen/emit.py
+++ b/logo/generator/logogen/emit.py
@@ -164,6 +164,7 @@ def render_words_json(spec: Spec) -> str:
             "aliases": w.aliases,
             "kind": w.kind,
             "level": w.level,
+            "group": w.group,
             "needs": w.needs,
             "inputs": [{"name": i.name, "type": i.type, "unit": i.unit} for i in w.inputs],
             "help": w.help,
diff --git a/logo/generator/logogen/model.py b/logo/generator/logogen/model.py
index 181316c..e63a6f8 100644
--- a/logo/generator/logogen/model.py
+++ b/logo/generator/logogen/model.py
@@ -9,6 +9,8 @@ from typing import Any, cast
 import yaml
 
 KINDS = {"command", "reporter", "control", "operator", "constant"}
+# the editor's colour per word (LLD §21.3)
+GROUPS = {"move", "turn", "flow", "show", "sense", "maths"}
 TYPES = {
     "number",
     "condition",
@@ -56,6 +58,7 @@ class Word:
     name: str
     kind: str
     level: int
+    group: str = ""
     aliases: list[str] = field(default_factory=list[str])
     inputs: list[Input] = field(default_factory=list[Input])
     needs: list[str] = field(default_factory=list[str])
@@ -127,6 +130,8 @@ def _load_words(raw: list[dict[str, Any]]) -> list[Word]:
         name = str(w["name"])
         if w["kind"] not in KINDS:
             raise ValueError(f"{name}: unknown kind {w['kind']}")
+        if w.get("group") not in GROUPS:
+            raise ValueError(f"{name}: unknown group {w.get('group')}")
         inputs: list[Input] = []
         for i in w.get("inputs", []):
             if i["type"] not in TYPES:
@@ -146,6 +151,7 @@ def _load_words(raw: list[dict[str, Any]]) -> list[Word]:
                 name=name,
                 kind=str(w["kind"]),
                 level=int(w["level"]),
+                group=str(w["group"]),
                 aliases=[str(a) for a in w.get("aliases", [])],
                 inputs=inputs,
                 needs=[str(n) for n in w.get("needs", [])],
```

- [ ] **Step 4: A group for every word** — run this once from the repo root (it adds `group:` after each word's `kind:`; the rest are `maths`):

```python
import pathlib, re
G = {}
for g, names in {
    "move": "FORWARD BACK SPEED HOME ARC PIVOT FIND FOLLOW",
    "turn": "RIGHT LEFT",
    "flow": "WAIT REPEAT STOP TO IF IFELSE WHILE FOREVER OUTPUT FOREACH",
    "show": "LED BEEP SOUND LOOK PLAY SAY",
    "sense": "DISTANCE OBSTACLE? BLOCKED? HEADING TILTED? BATTERY TIMER REPCOUNT SEE? HOWMANY",
}.items():
    for n in names.split():
        G[n] = g
G["WAIT UNTIL"] = "flow"
y = pathlib.Path("logo/spec/words.yaml")
out, cur = [], None
for ln in y.read_text().split("\n"):
    m = re.match(r"  - name: (.+)$", ln)
    if m:
        cur = m.group(1).strip().strip('"')
    out.append(ln)
    if ln.startswith("    kind:") and cur:
        out.append(f"    group: {G.get(cur, 'maths')}")  # every other word: operators, list words, TRUE / FALSE
y.write_text("\n".join(out))
```

and add the field to the comment block at the top of `logo/spec/words.yaml` (between `level` and `kind`):

```yaml
#   level            - 1 Starter, 2 Explorer, 3 Expert (editor grouping only; v1 builds levels 1-2 plus OUTPUT,
#                      the other level-3 words are reserved until v1.1)
#   group            - the editor colour (LLD §21.3): move, turn, flow, show, sense, maths
#   kind             - command (a statement), reporter (gives back a value), control (special form, grammar.md §3),
```

Check: `grep -c "^    group:" logo/spec/words.yaml` → `64`.

- [ ] **Step 5: Regenerate and run the tests**

Run: `cd logo && uv run logogen write && uv run pytest -q tests && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: `logogen: wrote 4 files`, `14 passed`, ruff clean, `0 errors`; `git status` shows only `words.json` changed among the generated files (the C++ `words.h` does not carry groups).

- [ ] **Step 6: The pre-commit check in a worktree** — replace `scripts/check-protocol.sh` with:

```bash
#!/usr/bin/env bash
# Fails if protocol/defs changed without regenerating outputs.
set -euo pipefail
# The repo from this script's own folder: inside a hook, git rev-parse --show-toplevel gives the current folder
# in a worktree.
top="$(cd "$(dirname "$0")/.." && pwd)"
cd "$top/protocol"
uv run protogen check
cd "$top/logo"
uv run logogen check
```

Run: `(cd /tmp && bash "$OLDPWD/scripts/check-protocol.sh"); echo "exit=$?"`
Expected: `exit=0` (run from the repo root; it works from any folder now).

- [ ] **Step 7: Commit**

```bash
git add scripts/check-protocol.sh
git commit -m "fix(tools): the pre-commit check finds logo/ from its own folder (worktrees)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git add logo/ firmware/esp32/web/generated/words.json
git commit -m "feat(logo): a colour group per word in words.yaml and words.json

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: The page's deciding code

**Files:**
- Create: `firmware/esp32/web/logo.js`
- Test: `firmware/esp32/web/test/logo.test.js`

**Interfaces:**
- Consumes: `firmware/esp32/web/generated/words.json` with `group` (Task 1).
- Produces (global `LogoCore` in the browser, `module.exports` in Node), used by Task 3:
  - `wordIndex(words, presets) → idx`; `ownWords(text) → {NAME: 1}`; `colour(text, idx, extraOwn?) → [{from, to, cls}]` with `cls` ∈ `g-move g-turn g-flow g-show g-sense g-maths num own inp str cmt op bad sp`
  - `insertWord(text, selStart, selEnd, word) → {text, selStart, selEnd}`
  - `new Drafts(storage)`: `keep(name, text, saved)`, `restore(name) → {text, saved} | null`, `clear(name)`, `unsaved(name) → bool`
  - `fitView(points, w, h, margin) → {scale, ox, oy, grid}`; `toScreen(view, x, y) → {x, y}`; `plannedRuns(path) → [{solid, pts: [{x, y}], endH}]`
  - `new CheckScheduler(delayMs, retryMs)`: `edited(now) → edit`, `now(now)`, `due(now) → edit | 0`, `replied(edit) → bool`, `busy(edit, now)`
  - `new RequestQueue(fetchFn, later, retryMs, tries)`: `send(method, path, body?) → Promise<{status, data}>`
  - `runBarStart() → state`; `runBar(state, event) → state` (with `act` ∈ `"run" "check" "step" "stop" null`); `canRun(state)`, `locked(state)`, `endText(end)`; `NACK_TEXT`

- [ ] **Step 1: Write the failing tests** — `firmware/esp32/web/test/logo.test.js`:

```js
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
const soon = (fn, ms) => setTimeout(fn, Math.min(ms, 3));

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
    setTimeout(fn, Math.min(ms, 3));
  };
  const r = await new L.RequestQueue(ff.f, strictLater, 500, 20).send("PUT", "/logo/programs/a", "FD 1");
  assert.equal(r.status, 200);
});
```

- [ ] **Step 2: Run them to see them fail**

Run: `node --test firmware/esp32/web/test/logo.test.js`
Expected: FAIL — `Cannot find module '../logo.js'`.

- [ ] **Step 3: Write the code** — `firmware/esp32/web/logo.js`:

```js
/* Tank Logo page - the deciding code (Tank Logo LLD §21.4): plain functions, no DOM, so Node can test them
   (node --test firmware/esp32/web/test/logo.test.js). pack_web.py puts this file into /logo in place of <!--CQ:LOGO_JS-->. */
var LogoCore = (function () {
  "use strict";

  // ---------- colours (§21.3): the colouring only colours, the tank's check decides what is right ----------
  var EXTRA = { END: "flow", UNTIL: "flow" };  // parts of TO ... END and WAIT UNTIL
  var TEXT_AFTER = { SAY: 1, PLAY: 1 };       // [ ... ] after these is text or notes, not words

  // word name or alias (upper case) -> group, and the preset names (LEFT, GREEN, HORN, ...)
  function wordIndex(words, presets) {
    var idx = { groups: {}, presets: {} };
    words.forEach(function (w) {
      w.name.split(" ").forEach(function (part) { if (!idx.groups[part]) idx.groups[part] = w.group; });
      (w.aliases || []).forEach(function (a) { idx.groups[a] = w.group; });
    });
    Object.keys(presets || {}).forEach(function (k) {
      presets[k].forEach(function (p) { idx.presets[String(p).toUpperCase()] = 1; });
    });
    Object.keys(EXTRA).forEach(function (k) { idx.groups[k] = EXTRA[k]; });
    return idx;
  }

  // the names after TO in the text (your own words, known before any check)
  function ownWords(text) {
    var own = {}, re = /(^|[\s\[\]])TO\s+([^\s\[\]();:"]+)/gi, m;
    while ((m = re.exec(text)) !== null) own[m[2].toUpperCase()] = 1;
    return own;
  }

  // text -> [{from, to, cls}] over the whole text; cls: g-move ... g-maths, num, own, inp, str, cmt, op, bad, sp
  function colour(text, idx, extraOwn) {
    var own = ownWords(text), spans = [], i = 0, n = text.length, textDepth = 0, lastWord = "", m;
    Object.keys(extraOwn || {}).forEach(function (k) { own[k.toUpperCase()] = 1; });
    function push(to, cls) { spans.push({ from: i, to: to, cls: cls }); i = to; }
    while (i < n) {
      var rest = text.slice(i), c = text[i];
      if ((m = /^\s+/.exec(rest))) { push(i + m[0].length, "sp"); continue; }
      if (c === ";") { var e = text.indexOf("\n", i); push(e < 0 ? n : e, "cmt"); continue; }
      if (c === "[") { if (textDepth > 0 || TEXT_AFTER[lastWord]) textDepth++; push(i + 1, "op"); lastWord = ""; continue; }
      if (c === "]") { if (textDepth > 0) textDepth--; push(i + 1, "op"); continue; }
      if (textDepth > 0) { m = /^[^\s\[\]]+/.exec(rest); push(i + m[0].length, "str"); continue; }
      if ((m = /^\d+(\.\d+)?/.exec(rest))) { push(i + m[0].length, "num"); lastWord = ""; continue; }
      if ((m = /^[+\-*\/=<>()]+/.exec(rest))) { push(i + m[0].length, "op"); continue; }
      m = /^[^\s\[\]()+\-*\/=<>;]+/.exec(rest);
      var tok = m[0], up = tok.toUpperCase(), cls;
      if (tok[0] === ":" || tok[0] === "?") cls = "inp";
      else if (tok[0] === '"') cls = "str";
      else if (idx.groups[up]) cls = "g-" + idx.groups[up];
      else if (own[up]) cls = "own";
      else if (idx.presets[up]) cls = "num";
      else cls = "bad";
      lastWord = up;
      push(i + tok.length, cls);
    }
    return spans;
  }

  // ---------- inserting a word (§14, §21): the example value selected, REPEAT with the cursor inside ----------
  // "|" = the cursor, "{...}" = the selection
  var TEMPLATES = {
    REPEAT: "REPEAT 4 [ | ]", FOREVER: "FOREVER [ | ]", IF: "IF {OBSTACLE?} [  ]",
    IFELSE: "IFELSE {DISTANCE < 30} [  ] [  ]", WHILE: "WHILE {NOT OBSTACLE?} [  ]",
    "WAIT UNTIL": "WAIT UNTIL {DISTANCE < 20}", TO: "TO {SHAPE}\n  \nEND", FOREACH: "FOREACH [ {50 100} ] [ FD ? ]"
  };

  function template(word) {
    if (TEMPLATES[word.name]) return TEMPLATES[word.name];
    var ex = word.example || "";
    if (word.kind !== "command" || ex.indexOf(word.name + " ") !== 0) return word.name + "|";
    var after = ex.slice(word.name.length + 1), b = after.indexOf("[ ");
    if (b >= 0 && after.lastIndexOf(" ]") === after.length - 2) {
      return word.name + " " + after.slice(0, b + 2) + "{" + after.slice(b + 2, after.length - 2) + "} ]";
    }
    return word.name + " {" + after + "}";
  }

  // where the word goes: a new line for a statement after other text (same indent), a space after other text
  function joiner(text, at, statement) {
    var before = text.slice(0, at), lineStart = before.lastIndexOf("\n") + 1, line = before.slice(lineStart);
    if (/^\s*$/.test(line)) return "";
    if (/\[\s*$/.test(line)) return /\s$/.test(line) ? "" : " ";
    if (statement) return "\n" + /^\s*/.exec(line)[0];
    return /\s$/.test(line) ? "" : " ";
  }

  // -> {text, selStart, selEnd}
  function insertWord(text, selStart, selEnd, word) {
    var t = template(word), statement = word.kind === "command" || word.kind === "control";
    var pre = joiner(text, selStart, statement), cur = t.indexOf("|"), a, b, body;
    if (cur >= 0) { body = t.replace("|", ""); a = b = cur; } else {
      a = t.indexOf("{"); body = t.replace("{", "").replace("}", ""); b = t.indexOf("}") - 1;
    }
    var head = text.slice(0, selStart);
    if (pre.charAt(0) === "\n") head = head.replace(/[ \t]+$/, "");  // no spaces left at the end of the line
    var out = head + pre + body + text.slice(selEnd), at = head.length + pre.length;
    return { text: out, selStart: at + a, selEnd: at + b };
  }

  // ---------- drafts in this browser (§21.8) ----------
  var DRAFT_KEY = "cq.logo.draft.";

  function Drafts(storage) { this.s = storage || null; }
  Drafts.prototype.keep = function (name, text, saved) {
    try {
      if (!this.s) return;
      if (text === saved) this.s.removeItem(DRAFT_KEY + name);
      else this.s.setItem(DRAFT_KEY + name, JSON.stringify({ text: text, saved: saved }));
    } catch (e) { this.s = null; }  // private mode / full: drafts off, the page still works
  };
  Drafts.prototype.restore = function (name) {
    try {
      var d = this.s ? JSON.parse(this.s.getItem(DRAFT_KEY + name) || "null") : null;
      return d && typeof d.text === "string" && typeof d.saved === "string" ? d : null;
    } catch (e) { return null; }
  };
  Drafts.prototype.clear = function (name) { try { if (this.s) this.s.removeItem(DRAFT_KEY + name); } catch (e) { this.s = null; } };
  Drafts.prototype.unsaved = function (name) { return this.restore(name) !== null; };

  // ---------- path view (§21.7): Logo axes, x right, y forward (up), right turns positive ----------
  var GRID_STEPS = [5, 10, 25, 50, 100, 250, 500];

  // points [{x, y}] in cm (the start 0, 0 always shown) -> {scale px/cm, ox, oy, grid cm}; screen y = oy - y * scale
  function fitView(points, w, h, margin) {
    var x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    points.forEach(function (p) { x0 = Math.min(x0, p.x); x1 = Math.max(x1, p.x); y0 = Math.min(y0, p.y); y1 = Math.max(y1, p.y); });
    var sx = Math.max(x1 - x0, 50), sy = Math.max(y1 - y0, 50);  // at least 50 cm across: a tiny path is not blown up
    var scale = Math.min((w - 2 * margin) / sx, (h - 2 * margin) / sy);  // the same scale both ways (1:1)
    var grid = GRID_STEPS.filter(function (g) { return g * scale >= 12; })[0] || 500;
    return { scale: scale, ox: w / 2 - ((x0 + x1) / 2) * scale, oy: h / 2 + ((y0 + y1) / 2) * scale, grid: grid };
  }

  function toScreen(v, x, y) { return { x: v.ox + x * v.scale, y: v.oy - y * v.scale }; }

  // the check's path [[x_mm, y_mm, heading, line, uncertain], ...] -> runs {solid, pts:[{x, y}] cm, endH}
  function plannedRuns(path) {
    var runs = [], cur = null;
    (path || []).forEach(function (p) {
      var solid = !p[4], pt = { x: p[0] / 10, y: p[1] / 10 };
      if (!cur || cur.solid !== solid) {
        cur = { solid: solid, pts: cur ? [cur.pts[cur.pts.length - 1]] : [] };
        runs.push(cur);
      }
      cur.pts.push(pt);
      cur.endH = p[2];  // heading at the run's last point (the arrow at the end of the plan)
    });
    return runs;
  }

  // ---------- checking as you type (§21.5) ----------
  function CheckScheduler(delayMs, retryMs) {
    this.delay = delayMs; this.retry = retryMs; this.edit = 0; this.dueAt = -1; this.inflight = 0;
  }
  CheckScheduler.prototype.edited = function (now) { this.edit++; this.dueAt = now + this.delay; return this.edit; };
  CheckScheduler.prototype.now = function (now) { this.dueAt = now; };  // check at once (CHECK_AGAIN, page shown)
  // -> the edit number to check now, or 0
  CheckScheduler.prototype.due = function (now) {
    if (this.inflight || this.dueAt < 0 || now < this.dueAt) return 0;
    this.dueAt = -1; this.inflight = this.edit;
    return this.edit;
  };
  // the reply for edit e: true = it is for the current text (show it), false = stale (drop it)
  CheckScheduler.prototype.replied = function (e) { this.inflight = 0; return e === this.edit; };
  // busy (503) or no answer: try again a little later, unless the text changed meanwhile (already due)
  CheckScheduler.prototype.busy = function (e, now) {
    this.inflight = 0;
    if (e === this.edit && this.dueAt < 0) this.dueAt = now + this.retry;
  };

  // ---------- one /logo request at a time (bridge: one worker job at a time, a second gets 503 busy) ----------
  // send(method, path, body) -> Promise of {status, data}: data = parsed JSON, the text, or null.
  // A 503 is tried again after retryMs, at most `tries` times; then it is given back as {status: 503}.
  function RequestQueue(fetchFn, later, retryMs, tries) {
    this.f = fetchFn; this.later = later; this.retry = retryMs; this.tries = tries; this.q = []; this.busy = false;
  }
  RequestQueue.prototype.send = function (method, path, body) {
    var self = this;
    return new Promise(function (resolve) { self.q.push({ m: method, p: path, b: body, left: self.tries, done: resolve }); self.next(); });
  };
  RequestQueue.prototype.next = function () {
    var self = this, job = this.q[0];
    if (this.busy || !job) return;
    this.busy = true;
    var opts = { method: job.m };
    if (job.b !== undefined) { opts.body = job.b; opts.headers = { "Content-Type": "text/plain" }; }
    function finish(res) { self.q.shift(); self.busy = false; job.done(res); self.next(); }
    this.f(job.p, opts).then(function (r) {
      if (r.status === 503 && job.left > 1) {
        job.left--; self.busy = false;
        var later = self.later;  // called on its own: a browser's setTimeout refuses another `this`
        later(function () { self.next(); }, self.retry);
        return null;
      }
      var json = (r.headers && r.headers.get && r.headers.get("Content-Type") || "").indexOf("json") >= 0;
      return (json ? r.json() : r.text()).then(function (d) { finish({ status: r.status, data: d }); },
        function () { finish({ status: r.status, data: null }); });
    }).catch(function () { finish({ status: 0, data: null }); });  // no link
  };

  // ---------- run bar (§21.6) ----------
  // st: editing, checking, ready, mistakes, starting, running, stepping, ended; other = another device's program
  function runBarStart() {
    return { st: "editing", prog: 0, ok: false, step: false, again: false, mine: false, other: false,
             line: 0, round: 0, end: null, msg: "", act: null };
  }

  var NACK_TEXT = {  // the rest use the bridge's own message
    BUSY: "The tank is busy (another program may be running) - try again.",
    NOT_OWNER: "Someone else is driving the tank."
  };

  function copy(s) { var o = {}; Object.keys(s).forEach(function (k) { o[k] = s[k]; }); o.act = null; return o; }

  function canRun(s) { return (s.st === "ready" || (s.st === "ended" && s.ok)) && !s.other && s.prog > 0; }
  function locked(s) { return s.st === "starting" || s.st === "running" || s.st === "stepping"; }
  function ours(s) { return s.mine || s.st === "starting"; }

  // (state, event) -> new state; state.act = what the page must do now: "run", "check", "step", "stop" or null
  function runBar(s0, ev) {
    var s = copy(s0);
    switch (ev.e) {
      case "edit": if (!locked(s)) { s.st = "editing"; s.prog = 0; s.ok = false; s.msg = ""; } break;
      case "checking": if (!locked(s)) s.st = "checking"; break;
      case "checked":
        if (locked(s) && !s.again) break;
        s.prog = ev.prog; s.ok = ev.ok; s.st = ev.ok ? "ready" : "mistakes";
        if (s.again) { s.again = s.ok; if (s.ok && !s.other) { s.st = "starting"; s.act = "run"; } else s.again = false; }
        break;
      case "run":
        if (!canRun(s)) break;
        s.st = "starting"; s.step = !!ev.step; s.again = false; s.end = null; s.msg = ""; s.line = 0; s.act = "run";
        break;
      case "ack": if (s.st === "starting") { s.st = "running"; s.mine = true; s.again = false; } break;
      case "nack":
        if (s.st !== "starting") break;
        if (ev.reason === "CHECK_AGAIN" && !s.again) { s.again = true; s.st = "checking"; s.act = "check"; break; }
        s.st = s.ok ? "ready" : "mistakes"; s.again = false; s.msg = NACK_TEXT[ev.reason] || ev.message || ev.reason;
        break;
      case "started": case "pose": if (!ours(s)) s.other = true; break;
      case "line":  // sent at each step-mode pause (the line changed) and with each move, light or sound
        if (!ours(s)) { s.other = true; break; }
        if (s.mine) s.st = s.step && ev.line !== s.line ? "stepping" : "running";
        s.line = ev.line; s.round = ev.round;
        break;
      case "step": if (s.st === "stepping") { s.st = "running"; s.act = "step"; } break;
      case "stop": s.act = "stop"; break;
      case "ended":
        if (!s.mine) { s.other = false; break; }
        s.st = "ended"; s.mine = false; s.end = { reason: ev.reason, line: ev.line, msg: ev.msg };
        break;
      case "hidden": case "closed":  // the bridge ends a program whose page went away (7046, bridge R11)
        if (s.mine || s.st === "starting") {
          s.st = "ended"; s.mine = false;
          s.end = { reason: 7046, line: s.line, msg: "Stopped: the page was hidden or the link dropped." };
        }
        s.other = false;
        break;
      default: break;
    }
    return s;
  }

  function endText(end) {
    if (!end) return "";
    return end.reason === "finished" ? "Done" : "Ended (" + end.reason + ")" + (end.line ? " on line " + end.line : "") + ": " + end.msg;
  }

  return {
    wordIndex: wordIndex, ownWords: ownWords, colour: colour, insertWord: insertWord, Drafts: Drafts,
    fitView: fitView, toScreen: toScreen, plannedRuns: plannedRuns, CheckScheduler: CheckScheduler, RequestQueue: RequestQueue,
    runBarStart: runBarStart, runBar: runBar, canRun: canRun, locked: locked, endText: endText, NACK_TEXT: NACK_TEXT
  };
})();
if (typeof module !== "undefined") module.exports = LogoCore;
```

- [ ] **Step 4: Run the tests**

Run: `node --test firmware/esp32/web/test/logo.test.js`
Expected: `ℹ tests 43`, `ℹ pass 43`, `ℹ fail 0`.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/web/logo.js firmware/esp32/web/test/logo.test.js
git commit -m "feat(web): the Tank Logo page's deciding code with Node tests

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: The `/logo` page, packed within its budget

**Files:**
- Create: `firmware/esp32/web/logo.html`, `firmware/esp32/web/test/fake_tank.py`
- Modify: `firmware/esp32/scripts/pack_web.py`

**Interfaces:**
- Consumes: `LogoCore` (Task 2), `words.json` (Task 1); the bridge's `/logo/check`, `/logo/programs[/<name>]` (LLD §20.4) and WebSocket `logo_run` / `logo_step` / `logo_stop`, events `ev` with `kind` `logo_started` `logo_line` `logo_pose` `logo_say` `logo_note` `logo_ended`, `ts` (battery, flags), `tf` (`dist_cm`), `wifi_status`, `welcome`, `ack` / `nack`.
- Produces: `GET /logo` (gzipped, from the asset table); `pack_web.assemble_logo(project) → bytes`, `pack_web.gz(bytes) → bytes`; the stand-in tank for Task 4.

- [ ] **Step 1: See that `/logo` is not packed yet**

Run: `cd firmware/esp32 && python3 scripts/pack_web.py .`
Expected: the list of assets has **no** `/logo`.

- [ ] **Step 2: Pack `/logo`** — apply to `firmware/esp32/scripts/pack_web.py`:

```diff
diff --git a/firmware/esp32/scripts/pack_web.py b/firmware/esp32/scripts/pack_web.py
index 55d7f67..5ac71eb 100644
--- a/firmware/esp32/scripts/pack_web.py
+++ b/firmware/esp32/scripts/pack_web.py
@@ -6,6 +6,8 @@
 - /help: web/help.html with <!--CQ:PROTOCOL--> replaced by the protogen output
   src/generated/help_protocol.html and <!--CQ:LOGO--> by web/help_logo.html (Task 9);
   skipped while web/help.html does not exist.
+- /logo (Plan N4, Tank Logo LLD §21): web/logo.html with <!--CQ:LOGO_JS--> replaced by
+  web/logo.js and <!--CQ:WORDS--> by web/generated/words.json. Budget 24 KB gzipped.
 Also runnable alone: python3 scripts/pack_web.py [project_dir]
 """
 
@@ -19,6 +21,7 @@ import zlib
 
 PAGE_BUDGET = 25 * 1024
 HELP_BUDGET = 16 * 1024
+LOGO_BUDGET = 24 * 1024  # Plan N4 (Tank Logo LLD §21.9): /logo, gzipped, words included
 SETTINGS_BUDGET = 36 * 1024  # Plan M: /settings, gzipped; Plan O (LLD R13.1): 36 KB for four tabs
 
 
@@ -74,6 +77,18 @@ def assemble_help(project: pathlib.Path) -> bytes | None:
     return text.replace("<!--CQ:PROTOCOL-->", proto).replace("<!--CQ:LOGO-->", logo).encode()
 
 
+def assemble_logo(project: pathlib.Path) -> bytes:
+    web = project / "web"
+    text = (web / "logo.html").read_text(encoding="utf-8")
+    for marker in ("<!--CQ:LOGO_JS-->", "<!--CQ:WORDS-->"):
+        if marker not in text:
+            raise SystemExit(f"pack_web: {marker} missing in web/logo.html")
+    code = (web / "logo.js").read_text(encoding="utf-8")
+    words = (web / "generated" / "words.json").read_text(encoding="utf-8").strip()
+    words = words.replace("</", "<\\/")  # never closes the <script> it sits in
+    return text.replace("<!--CQ:LOGO_JS-->", code).replace("<!--CQ:WORDS-->", words).encode()
+
+
 def c_array(name: str, data: bytes) -> str:
     lines = [f"inline const uint8_t {name}[] = {{"]
     for i in range(0, len(data), 20):
@@ -101,6 +116,10 @@ def build(project: pathlib.Path) -> None:
     if len(settings) > SETTINGS_BUDGET:
         raise SystemExit(f"pack_web: /settings {len(settings)} B gzipped > {SETTINGS_BUDGET} B budget")
     assets.append(("/settings", "text/html; charset=utf-8", settings, True))
+    logo = gz(assemble_logo(project))
+    if len(logo) > LOGO_BUDGET:
+        raise SystemExit(f"pack_web: /logo {len(logo)} B gzipped > {LOGO_BUDGET} B budget")
+    assets.append(("/logo", "text/html; charset=utf-8", logo, True))
     help_page = assemble_help(project)
     if help_page is not None:
         packed = gz(help_page)
@@ -146,6 +165,6 @@ def build(project: pathlib.Path) -> None:
 
 if __name__ == "__main__":
     build(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve())
-else:  # PlatformIO extra script
+elif "Import" in globals() or hasattr(__import__("builtins"), "Import"):  # PlatformIO script
     Import("env")  # type: ignore[name-defined]  # noqa: F821 - SCons injects Import/env
     build(pathlib.Path(env.subst("$PROJECT_DIR")))  # type: ignore[name-defined]  # noqa: F821
```

- [ ] **Step 3: The page** — `firmware/esp32/web/logo.html`:

```html
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#050805">
<title>Tank Logo</title>
<link rel="icon" href="/icon-192.png">
<style>
/* Plan N4 (Tank Logo LLD §21): write, check, run and step Tank Logo programs. Look per the Stitch screens
   docs/design/stitch/15-tank-logo-desktop-v2.png (laptop) and 16-tank-logo-mobile-v2.png (phone), without the speed
   and PAUSE controls (design-notes §106). STOP is on this page because it moves the tank (§72). */
:root {
  --bg: #050805; --panel: #070f08; --edge-c: rgba(51, 255, 102, .32); --line: rgba(51, 255, 102, .14); --ui: #33ff66;
  --dim: #1f9a40; --warn: #ffb000; --danger: #ff3b30; --active: #e8ffe8; --text: #b8f5c8;
  --move: #33ff66; --turn: #b5e853; --flow: #3ee0e8; --show: #ff7ad9; --sense: #8cc8ff; --maths: #dfe8df;
  --num: #ffb000; --own: #ffffff; --cmt: #4f7a5a;
  --mono: "JetBrains Mono", ui-monospace, Menlo, Consolas, "DejaVu Sans Mono", monospace;
}
* { box-sizing: border-box; }
html, body { margin: 0; background: var(--bg); color: var(--text); font: 13px/1.4 var(--mono); }
body { background-image: repeating-linear-gradient(0deg, rgba(51, 255, 102, .025) 0 1px, transparent 1px 3px); padding-bottom: 76px; }
a, button, input, textarea, select { font: inherit; color: inherit; }
button { background: transparent; border: 1px solid var(--edge-c); color: var(--ui); padding: 8px 10px; letter-spacing: .06em; cursor: pointer; }
button:disabled { opacity: .35; cursor: default; }
svg { width: 14px; height: 14px; fill: none; stroke: currentColor; stroke-width: 2; stroke-linecap: round; stroke-linejoin: round; vertical-align: -2px; }
header { padding: 10px max(12px, env(safe-area-inset-left)) 8px; border-bottom: 1px solid var(--edge-c); }
.top { display: flex; align-items: center; gap: 8px; }
.top a { color: var(--ui); text-decoration: none; letter-spacing: .08em; font-weight: 700; }
.top h1 { flex: 1; font-size: 13px; margin: 0; color: var(--ui); letter-spacing: .14em; text-align: center; }
#stop { background: var(--danger); color: #fff; border-color: var(--danger); font-weight: 700; padding: 8px 14px; }
.chips { display: grid; grid-template-columns: repeat(3, 1fr); gap: 6px; margin-top: 8px; }
.chip { border: 1px solid var(--edge-c); padding: 4px 6px; color: var(--ui); text-align: center; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; font-size: 11px; }
.chip.warn { color: var(--warn); border-color: rgba(255, 176, 0, .5); }
#menu { display: none; position: absolute; right: 12px; top: 52px; z-index: 5; background: var(--panel); border: 1px solid var(--edge-c); }
#menu.on { display: block; } #menu a { display: block; padding: 12px 18px; color: var(--ui); text-decoration: none; border-bottom: 1px solid var(--line); }
main { padding: 10px 12px; display: grid; gap: 10px; }
.card { border: 1px solid var(--edge-c); background: var(--panel); padding: 10px; min-width: 0; }
.card h2 { margin: 0 0 8px; font-size: 12px; color: var(--ui); letter-spacing: .1em; display: flex; justify-content: space-between; gap: 8px; }
.badge { color: var(--warn); border: 1px solid rgba(255, 176, 0, .5); padding: 0 6px; font-size: 11px; }
.progbar { display: flex; gap: 6px; } .progbar span { display: none; } .progbar select { flex: 1; min-width: 0; background: var(--bg); border: 1px solid var(--edge-c); color: var(--ui); padding: 8px; }
#plist { display: none; list-style: none; margin: 0; padding: 0; }
#plist li { padding: 7px 10px; cursor: pointer; color: var(--text); } #plist li.on { color: var(--ui); background: rgba(51, 255, 102, .08); border-left: 2px solid var(--ui); }
#plist .sec { color: var(--dim); font-size: 11px; cursor: default; letter-spacing: .1em; padding-top: 12px; }
.dot::after { content: " ●"; color: var(--warn); }
#tabs { display: grid; grid-template-columns: 1fr 1fr; gap: 6px; } #tabs button.on { background: rgba(51, 255, 102, .12); color: var(--active); border-color: var(--ui); }
.ed { position: relative; display: flex; border: 1px solid var(--line); background: var(--bg); height: 46vh; min-height: 220px; }
#gut, #hl, #src { margin: 0; font: 16px/1.6 var(--mono); padding: 8px 0; white-space: pre; tab-size: 2; }
#gut { width: 3.2em; text-align: right; padding-right: 8px; color: var(--cmt); overflow: hidden; user-select: none; flex: none; }
#gut .err { color: var(--danger); } #gut .run { color: var(--warn); }
.edw { position: relative; flex: 1; min-width: 0; overflow: hidden; }
#hl, #src { position: absolute; inset: 0; padding-left: 4px; padding-right: 8px; }
#hl { overflow: hidden; pointer-events: none; color: var(--text); }
#src { width: 100%; height: 100%; resize: none; border: 0; outline: 0; background: transparent; color: transparent; caret-color: var(--active); overflow: auto; }
#src[readonly] { caret-color: transparent; }
#hl div { min-height: 1.6em; position: relative; } #hl div.run { background: rgba(255, 176, 0, .14); box-shadow: inset 3px 0 var(--warn); }
.rnd { margin-left: 12px; font-style: normal; font-size: 11px; color: #000; background: var(--warn); padding: 1px 6px; vertical-align: 2px; }
.g-move { color: var(--move); } .g-turn { color: var(--turn); } .g-flow { color: var(--flow); } .g-show { color: var(--show); }
.g-sense { color: var(--sense); } .g-maths { color: var(--maths); } .num { color: var(--num); } .own { color: var(--own); }
.inp { color: var(--own); font-style: italic; } .str { color: var(--text); } .cmt { color: var(--cmt); font-style: italic; } .op { color: var(--dim); }
.bad { text-decoration: underline wavy var(--danger); } .err { text-decoration: underline wavy var(--danger); text-underline-offset: 3px; }
.pad { display: flex; flex-wrap: wrap; gap: 6px; margin-top: 8px; } .pad button { padding: 7px 9px; font-size: 13px; }
.pad button small { color: var(--dim); } .help { margin-top: 6px; color: var(--dim); min-height: 1.4em; }
#diags { margin-top: 8px; } .diag { border-left: 3px solid var(--danger); padding: 6px 8px; margin-bottom: 6px; cursor: pointer; }
.diag.warn { border-color: var(--warn); } .diag pre { margin: 0; font: 15px/1.4 var(--mono); color: var(--active); overflow: hidden; }
.diag p { margin: 2px 0 0; font-size: 14px; color: var(--active); }
.msgs { display: flex; gap: 10px; } #log { flex: 1; min-width: 0; max-height: 9.5em; overflow: auto; font-size: 12px; }
#log div { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; } #log .say { color: var(--show); } #log .end { color: var(--warn); }
#mini { width: 84px; height: 84px; border: 1px solid var(--line); flex: none; cursor: pointer; }
.pathw { position: relative; height: 52vh; min-height: 260px; border: 1px solid var(--line); background: var(--bg); }
#path { width: 100%; height: 100%; display: block; }
#pnote { position: absolute; left: 8px; top: 6px; color: var(--dim); font-size: 11px; }
.stats { display: grid; grid-template-columns: repeat(3, 1fr); gap: 6px; margin-top: 8px; text-align: center; }
.stats div { border: 1px solid var(--line); padding: 6px; } .stats small { display: block; color: var(--dim); font-size: 10px; letter-spacing: .1em; }
.stats b { color: var(--ui); font-size: 14px; white-space: nowrap; }
#bar { position: fixed; left: 0; right: 0; bottom: 0; background: var(--panel); border-top: 1px solid var(--edge-c); padding: 8px 12px max(8px, env(safe-area-inset-bottom)); display: flex; gap: 8px; align-items: center; z-index: 4; }
#status { flex: 1; min-width: 0; font-size: 12px; color: var(--ui); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
#status.s-warn { color: var(--warn); } #status.s-bad { color: var(--danger); }
#run { background: var(--warn); color: #000; border-color: var(--warn); font-weight: 700; padding: 12px 18px; }
#step { padding: 12px 14px; }
.sheet { display: none; position: fixed; inset: 0; background: rgba(0, 0, 0, .7); z-index: 9; align-items: center; justify-content: center; }
.sheet.on { display: flex; } .sheet .card { width: min(360px, 92vw); } .sheet input { width: 100%; background: var(--bg); border: 1px solid var(--edge-c); padding: 10px; margin: 8px 0; color: var(--active); }
.sheet .row { display: flex; gap: 8px; justify-content: flex-end; } .sheet p { margin: 4px 0; } .sheet .why { color: var(--warn); min-height: 1.4em; }
.hide { display: none !important; }
@media (max-width: 899px) { body:not(.tpath) #cPath { display: none; } body.tpath #cEdit, body.tpath #cMsg { display: none; } }
@media (min-width: 900px) {
  body { padding-bottom: 0; }
  header .inner { display: flex; align-items: center; gap: 16px; } .top { flex: 1; } .top h1 { text-align: left; }
  .chips { display: flex; margin-top: 0; }
  main { grid-template-columns: 220px minmax(0, 1.2fr) minmax(0, 1fr); align-items: start; }
  #cProg { grid-row: span 3; } #plist { display: block; } .progbar select, #tabs { display: none; }
  .progbar { flex-wrap: wrap; } .progbar span { display: inline; } .progbar button { flex: 1; }
  #cEdit { grid-column: 2; } #cMsg { grid-column: 2; } #cPath { grid-column: 3; grid-row: 1 / span 3; }
  #mini { display: none; } #bar { position: static; grid-column: 2; border: 1px solid var(--edge-c); padding: 8px; }
  .ed { height: 52vh; }
}
</style>
</head>
<body>
<header><div class="inner">
  <div class="top">
    <a href="/"><svg viewBox="0 0 24 24"><path d="m15 18-6-6 6-6"/></svg>DRIVE</a>
    <h1>[ TANK LOGO ]</h1>
    <button id="mbtn" type="button" aria-label="Menu"><svg viewBox="0 0 24 24"><path d="M4 6h16M4 12h16M4 18h16"/></svg></button>
    <button id="stop" type="button">STOP</button>
  </div>
  <div class="chips">
    <span class="chip" id="cBatt">-- V</span>
    <span class="chip" id="cWifi">WIFI --</span>
    <span class="chip" id="cUno">● UNO --</span>
  </div>
</div></header>
<nav id="menu"><a href="/">DRIVE</a><a href="/settings">SETTINGS</a><a href="/help">HELP</a></nav>
<main>
<section class="card" id="cProg">
  <h2>[ PROGRAMS ]</h2>
  <div class="progbar">
    <select id="psel" aria-label="Program"></select>
    <button id="pnew" type="button" title="New program"><svg viewBox="0 0 24 24"><path d="M12 5v14M5 12h14"/></svg><span> NEW</span></button>
    <button id="psave" type="button" title="Save on the tank"><svg viewBox="0 0 24 24"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/><path d="M17 21v-8H7v8M7 3v5h8"/></svg><span> SAVE</span></button>
    <button id="pren" type="button" title="Rename"><svg viewBox="0 0 24 24"><path d="M21.17 6.81a1 1 0 0 0-3.99-3.99L3.84 16.17a2 2 0 0 0-.5.83l-1.32 4.35a.5.5 0 0 0 .62.62l4.35-1.32a2 2 0 0 0 .83-.5z"/></svg><span> RENAME</span></button>
    <button id="pdel" type="button" title="Delete" style="color:var(--danger)"><svg viewBox="0 0 24 24"><path d="M3 6h18M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2"/></svg><span> DELETE</span></button>
  </div>
  <ul id="plist"></ul>
</section>
<div id="tabs"><button type="button" class="on" data-t="prog">[ PROGRAM ]</button><button type="button" data-t="path">PATH</button></div>
<section class="card" id="cEdit">
  <h2><span id="title">[ 01 // PROGRAM ]</span><span class="badge hide" id="state"></span></h2>
  <div class="ed"><pre id="gut"></pre><div class="edw"><pre id="hl" aria-hidden="true"></pre>
    <textarea id="src" wrap="off" spellcheck="false" autocapitalize="characters" autocomplete="off" aria-label="Program text"></textarea></div></div>
  <div id="diags"></div>
  <div class="pad" id="pad"></div>
  <div class="help" id="help"></div>
</section>
<section class="card" id="cMsg">
  <h2>[ MESSAGES ]</h2>
  <div class="msgs"><div id="log"></div><canvas id="mini" title="Open the path"></canvas></div>
</section>
<section class="card" id="cPath">
  <h2>[ 02 // PATH ]<span id="pgrid" class="badge">5 cm GRID</span></h2>
  <div class="pathw"><canvas id="path"></canvas><span id="pnote"></span></div>
  <div class="stats">
    <div><small>HEADING</small><b id="sHead">--</b></div>
    <div><small>DISTANCE AHEAD</small><b id="sDist">--</b></div>
    <div><small>TIME</small><b id="sTime">0:00 / 5:00</b></div>
  </div>
</section>
<div id="bar">
  <span id="status">● CONNECTING</span>
  <button id="step" type="button">STEP</button>
  <button id="run" type="button">▶ RUN</button>
</div>
</main>
<div class="sheet" id="nameS"><div class="card">
  <h2 id="nameT">SAVE AS</h2>
  <p>Name: 1-24 letters a-z, numbers, _ or -</p>
  <input id="nameI" maxlength="24" autocapitalize="none" spellcheck="false">
  <p class="why" id="nameW"></p>
  <div class="row"><button id="nameC" type="button">CANCEL</button><button id="nameO" type="button">OK</button></div>
</div></div>
<div class="sheet" id="delS"><div class="card">
  <h2>DELETE</h2><p id="delT"></p>
  <div class="row"><button id="delC" type="button">CANCEL</button><button id="delO" type="button" style="color:var(--danger)">DELETE</button></div>
</div></div>
<script>
<!--CQ:LOGO_JS-->
</script>
<script>
(function () {
  "use strict";
  var W = <!--CQ:WORDS-->;
  var L = LogoCore, $ = function (id) { return document.getElementById(id); };
  var idx = L.wordIndex(W.words, W.presets), src = $("src");
  var sched = new L.CheckScheduler(600, 500), bar = L.runBarStart(), drafts = new L.Drafts(storage());
  var http = new L.RequestQueue(function (p, o) { return fetch(p, o); }, function (f, ms) { setTimeout(f, ms); }, 500, 20);  // ≤ 10 s of busy
  var ws = null, open = false, linked = false, retry = 500, nextId = 1, runId = 0, stepId = 0;
  var cur = { name: "", example: false, saved: "" }, progs = [], own = {}, diags = [], planned = [], cutShort = false;
  var driven = [], pose = null, startedAt = 0, logLines = [];
  var NEW = "<new>", START_TEXT = "; my program\nFORWARD 50\nRIGHT 90\n";

  function storage() { try { return window.localStorage; } catch (e) { return null; } }
  function remember(k, v) { try { localStorage.setItem(k, v); } catch (e) { /* drafts off */ } }
  function recall(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
  function esc(s) { return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"); }
  function el(tag, cls, text) { var e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; }
  function send(o) { if (ws && open) { ws.send(JSON.stringify(o)); return true; } return false; }
  function clock(ms) { var s = Math.max(0, Math.floor(ms / 1000)); return Math.floor(s / 60) + ":" + ("0" + (s % 60)).slice(-2); }

  // ---------- editor ----------
  function lineStarts(text) { var a = [0]; for (var i = 0; i < text.length; i++) if (text[i] === "\n") a.push(i + 1); return a; }
  function render() {
    var text = src.value, spans = L.colour(text, idx, own), starts = lineStarts(text), errAt = {}, errLine = {};
    diags.forEach(function (d) {
      if (d.line >= 1 && d.line <= starts.length) { errAt[starts[d.line - 1] + Math.max(0, d.col - 1)] = 1; errLine[d.line] = 1; }
    });
    var running = L.locked(bar) || bar.st === "ended" ? bar.line : 0, lines = [], cell = "";
    spans.forEach(function (s) {
      var hit = false;
      Object.keys(errAt).forEach(function (k) { if (k >= s.from && k < s.to && s.cls !== "sp") hit = true; });
      text.slice(s.from, s.to).split("\n").forEach(function (part, i) {
        if (i > 0) { lines.push(cell); cell = ""; }
        if (part) cell += '<span class="' + s.cls + (hit ? " err" : "") + '">' + esc(part) + "</span>";
      });
    });
    lines.push(cell);
    var gut = "";
    $("hl").innerHTML = lines.map(function (h, i) {
      var n = i + 1, run = n === running && L.locked(bar);
      gut += '<div class="' + (errLine[n] ? "err" : run ? "run" : "") + '">' + (errLine[n] ? "▲" : "") + n + "</div>";
      return '<div class="' + (run ? "run" : "") + '">' + h + (run && bar.round ? '<i class="rnd">ROUND ' + bar.round + "</i>" : "") + "</div>";
    }).join("");
    $("gut").innerHTML = gut;
    scrollSync();
  }
  function scrollSync() { $("hl").scrollTop = $("gut").scrollTop = src.scrollTop; $("hl").scrollLeft = src.scrollLeft; }
  function edited() {
    if (L.locked(bar)) return;
    bar = L.runBar(bar, { e: "edit" });
    sched.edited(Date.now());
    drafts.keep(cur.name || NEW, src.value, cur.saved);
    soon(); showProgs(); showBar();
  }
  var pending = false;  // one colouring per screen frame, however fast the keys come (an 8 KB text ≈ 11 ms)
  function soon() { if (!pending) { pending = true; requestAnimationFrame(function () { pending = false; render(); }); } }
  src.addEventListener("input", edited);
  src.addEventListener("scroll", scrollSync);
  src.addEventListener("keydown", function (e) {
    if (e.key === "Tab" && !src.readOnly) { e.preventDefault(); put("  ", 0, 0); }
  });
  function put(text, a, b) {
    var s = src.selectionStart, v = src.value;
    src.value = v.slice(0, s) + text + v.slice(src.selectionEnd);
    src.setSelectionRange(s + a + (a ? 0 : text.length), s + b + (b ? 0 : text.length));
    edited();
  }
  function setText(text) {  // another program: its own mistakes, plan and path
    src.value = text; diags = []; own = {}; planned = []; driven = []; pose = null; cutShort = false;
    sched.edited(Date.now() - 600); bar = L.runBar(bar, { e: "edit" }); render(); showDiags(); draw();
  }

  // ---------- word buttons (levels, §83) ----------
  var more = false;
  function showPad() {
    var pad = $("pad"); pad.textContent = "";
    W.words.forEach(function (w) {
      var later = w.level === 3 && w.name !== "OUTPUT";  // v1: levels 1-2 and OUTPUT
      if (w.level > 1 && !more) return;
      var b = el("button", "g-" + w.group, w.name);
      b.type = "button";
      if (later) { b.disabled = true; b.appendChild(el("small", "", " later")); }
      b.addEventListener("click", function () { insert(w); });
      pad.appendChild(b);
    });
    var m = el("button", "", more ? "FEWER WORDS ▴" : "MORE WORDS ▾"); m.type = "button";
    m.addEventListener("click", function () { more = !more; showPad(); });
    pad.appendChild(m);
  }
  function insert(w) {
    if (L.locked(bar)) return;
    var r = L.insertWord(src.value, src.selectionStart, src.selectionEnd, w);
    src.value = r.text; src.focus(); src.setSelectionRange(r.selStart, r.selEnd);
    var ins = (w.inputs || []).map(function (i) { return i.name; }).join(" ");
    $("help").textContent = "? " + w.name + (ins ? " " + ins : "") + " - " + w.help;
    edited();
  }

  // ---------- checking (§21.5) ----------
  function checkTick() {
    var e = sched.due(Date.now());
    if (!e) return;
    var text = src.value;
    bar = L.runBar(bar, { e: "checking" }); showBar();
    http.send("POST", "/logo/check" + (cur.name ? "?name=" + encodeURIComponent(cur.name) : ""), text).then(function (r) {
      var m = r.data;
      if (r.status !== 200 || !m || typeof m !== "object") { sched.busy(e, Date.now()); return; }
      if (!sched.replied(e)) return;
      diags = m.diags || []; own = {};
      (m.words || []).forEach(function (w) { own[w.name] = 1; });
      planned = L.plannedRuns(m.path); cutShort = !!m.cut_short;
      bar = L.runBar(bar, { e: "checked", ok: !!m.ok, prog: m.prog || 0 });
      showDiags(); render(); draw(); act(); showBar();
    });
  }
  setInterval(checkTick, 100);
  function showDiags() {
    var box = $("diags"), lines = src.value.split("\n"); box.textContent = "";
    diags.slice(0, 5).forEach(function (d) {
      var row = el("div", "diag" + (d.warning ? " warn" : "")), text = lines[d.line - 1] || "";
      var pre = el("pre", "", "line " + d.line + ": " + text + "\n" + new Array(("line " + d.line + ": ").length + Math.max(0, d.col - 1) + 1).join(" ") + "▲");
      row.appendChild(pre); row.appendChild(el("p", "", d.msg + " (" + d.code + ")"));
      row.addEventListener("click", function () { goTo(d.line, d.col); });
      box.appendChild(row);
    });
  }
  function goTo(line, col) {
    var s = lineStarts(src.value), at = (s[line - 1] || 0) + Math.max(0, col - 1);
    src.focus(); src.setSelectionRange(at, at);
  }

  // ---------- running (§21.6) ----------
  function act() {
    var a = bar.act; bar.act = null;
    if (a === "run") {
      runId = nextId++; startedAt = 0;
      if (!send({ t: "logo_run", prog: bar.prog, step: bar.step, id: runId })) bar = L.runBar(bar, { e: "nack", reason: "OFFLINE", message: "Not connected to the tank." });
    } else if (a === "check") sched.now(Date.now());
    else if (a === "step") { stepId = nextId++; send({ t: "logo_step", id: stepId }); }
    else if (a === "stop") { send({ t: "stop", id: nextId++ }); send({ t: "logo_stop", id: nextId++ }); }
  }
  function onBar(ev) { var was = bar.st; bar = L.runBar(bar, ev); act(); if (bar.st !== was) lockEditor(); showBar(); }
  function lockEditor() {
    src.readOnly = L.locked(bar);
    if (bar.st === "running" && !startedAt) { startedAt = Date.now(); driven = []; pose = null; logLines = []; log("program started", ""); }
    render();
  }
  $("run").addEventListener("click", function () { if (bar.st === "stepping") onBar({ e: "step" }); else onBar({ e: "run", step: false }); });
  $("step").addEventListener("click", function () { if (bar.st === "stepping") onBar({ e: "step" }); else onBar({ e: "run", step: true }); });
  $("stop").addEventListener("click", function () { onBar({ e: "stop" }); });
  function showBar() {
    var s = bar, st = $("status"), t = "", cls = "";
    var n = diags.filter(function (d) { return !d.warning; }).length;
    if (s.other && !L.locked(s)) { t = "● A PROGRAM IS RUNNING ON ANOTHER DEVICE"; cls = "s-warn"; }
    else if (s.st === "editing" || s.st === "checking") t = "● CHECKING …";
    else if (s.st === "mistakes") { t = "● " + n + (n === 1 ? " MISTAKE" : " MISTAKES") + " - see under the program"; cls = "s-bad"; }
    else if (s.st === "ready") t = s.msg ? "● " + s.msg : "● READY";
    else if (s.st === "starting") t = "● STARTING …";
    else if (s.st === "running") { t = "● RUNNING · line " + (s.line || "-") + " · " + clock(Date.now() - startedAt); cls = "s-warn"; }
    else if (s.st === "stepping") { t = "● STEP · line " + s.line + " - press STEP for the next line"; cls = "s-warn"; }
    else if (s.st === "ended") { t = L.endText(s.end); cls = s.end && s.end.reason === "finished" ? "" : "s-bad"; }
    if (!open && !L.locked(s)) { t = "● NOT CONNECTED"; cls = "s-warn"; }
    st.textContent = t; st.className = cls;
    $("run").disabled = !(L.canRun(s) || s.st === "stepping") || !open;
    $("run").textContent = s.st === "stepping" ? "▶ NEXT" : "▶ RUN";
    $("step").disabled = !(L.canRun(s) || s.st === "stepping") || !open;
    $("state").textContent = L.locked(s) ? "RUNNING" : s.st === "ended" ? "ENDED" : "";
    $("state").className = "badge" + ($("state").textContent ? "" : " hide");
    $("sTime").textContent = (L.locked(s) ? clock(Date.now() - startedAt) : "0:00") + " / 5:00";
    $("psave").disabled = $("pdel").disabled = $("pren").disabled = L.locked(s);
    if (!L.locked(s) && s.st !== "running") startedAt = s.st === "ended" ? startedAt : 0;
  }
  setInterval(function () { if (L.locked(bar)) showBar(); }, 1000);
  function log(text, cls) {
    var d = new Date(), t = ("0" + d.getHours()).slice(-2) + ":" + ("0" + d.getMinutes()).slice(-2) + ":" + ("0" + d.getSeconds()).slice(-2);
    logLines.push({ t: "[" + t + "] " + text, cls: cls }); if (logLines.length > 60) logLines.shift();
    var box = $("log"); box.textContent = "";
    logLines.forEach(function (l) { box.appendChild(el("div", l.cls, l.t)); });
    box.scrollTop = box.scrollHeight;
  }
  function onEvent(m) {
    var mine = bar.mine || bar.st === "starting";
    switch (m.kind) {
      case "logo_started": onBar({ e: "started" }); break;
      case "logo_line":
        onBar({ e: "line", line: m.line, round: m.round });
        if (mine) { log("line " + m.line + ": " + (src.value.split("\n")[m.line - 1] || "").trim(), ""); render(); }
        break;
      case "logo_pose":
        onBar({ e: "pose" });
        if (mine) { pose = { x: m.x_cm, y: m.y_cm, h: m.heading }; driven.push({ x: m.x_cm, y: m.y_cm }); $("sHead").textContent = m.heading + "°"; draw(); }
        break;
      case "logo_say": if (mine) log("SAY " + m.text, "say"); break;
      case "logo_note": if (mine) log("line " + m.line + ": " + m.msg, ""); break;
      case "logo_ended":
        var ours = bar.mine;
        onBar({ e: "ended", reason: m.reason, line: m.line, msg: m.msg });
        if (ours) { log(L.endText(bar.end), "end"); render(); }
        break;
      default: break;
    }
  }

  // ---------- path (§21.7) ----------
  function canvasFit(c) {
    var r = c.getBoundingClientRect(), k = window.devicePixelRatio || 1;
    c.width = Math.max(1, Math.round(r.width * k)); c.height = Math.max(1, Math.round(r.height * k));
    var g = c.getContext("2d"); g.setTransform(k, 0, 0, k, 0, 0);
    return { g: g, w: r.width, h: r.height };
  }
  function drawOn(c, small) {
    var f = canvasFit(c), g = f.g, all = [];
    if (f.w < 2) return;
    planned.forEach(function (run) { all = all.concat(run.pts); });
    var v = L.fitView(all.concat(driven), f.w, f.h, small ? 6 : 24);
    g.clearRect(0, 0, f.w, f.h);
    if (!small) {
      g.strokeStyle = "rgba(51,255,102,.10)"; g.lineWidth = 1; g.beginPath();
      var o = L.toScreen(v, 0, 0), step = v.grid * v.scale;
      for (var x = o.x % step; x < f.w; x += step) { g.moveTo(x, 0); g.lineTo(x, f.h); }
      for (var y = o.y % step; y < f.h; y += step) { g.moveTo(0, y); g.lineTo(f.w, y); }
      g.stroke();
      $("pgrid").textContent = v.grid + " cm GRID";
    }
    g.lineWidth = small ? 1.5 : 2;
    planned.forEach(function (run) {
      g.strokeStyle = "rgba(51,255,102,.55)"; g.setLineDash(run.solid ? [] : [6, 5]); line(g, v, run.pts);
    });
    g.setLineDash([]);
    if (driven.length > 1) { g.strokeStyle = "#ffb000"; g.lineWidth = small ? 2 : 3; line(g, v, driven); }
    var s = L.toScreen(v, 0, 0);
    g.strokeStyle = "#33ff66"; g.lineWidth = 1.5; g.beginPath(); g.arc(s.x, s.y, small ? 3 : 6, 0, 7); g.stroke();
    var at = pose || lastPlanned();
    if (at) arrow(g, v, at, small ? 5 : 10);
    if (!small) {
      var cm = [100, 50, 25, 10, 5].filter(function (c) { return c * v.scale <= f.w / 4; })[0] || 5, len = cm * v.scale;
      g.fillStyle = "#1f9a40"; g.fillRect(f.w - 14 - len, f.h - 14, len, 2);  // bottom right: a scale of about 1/4 width
      g.font = "11px monospace"; g.fillText(cm + " cm", f.w - 14 - len, f.h - 20);
      g.fillText("(0,0) START", s.x + 9, s.y + 16);
    }
  }
  function line(g, v, pts) {
    g.beginPath();
    pts.forEach(function (p, i) { var s = L.toScreen(v, p.x, p.y); if (i) g.lineTo(s.x, s.y); else g.moveTo(s.x, s.y); });
    g.stroke();
  }
  function lastPlanned() {
    var r = planned[planned.length - 1]; if (!r) return null;
    var p = r.pts[r.pts.length - 1];
    return { x: p.x, y: p.y, h: r.endH || 0 };
  }
  function arrow(g, v, p, size) {
    var s = L.toScreen(v, p.x, p.y), a = (p.h || 0) * Math.PI / 180, dx = Math.sin(a), dy = -Math.cos(a);
    g.fillStyle = "#ffb000"; g.beginPath();
    g.moveTo(s.x + dx * size * 1.4, s.y + dy * size * 1.4);
    g.lineTo(s.x - dy * size * .8 - dx * size, s.y + dx * size * .8 - dy * size);
    g.lineTo(s.x + dy * size * .8 - dx * size, s.y - dx * size * .8 - dy * size);
    g.closePath(); g.fill();
  }
  function draw() {
    drawOn($("path"), false); drawOn($("mini"), true);
    $("pnote").textContent = cutShort ? "path cut short (a limit was reached)" : "";
  }
  window.addEventListener("resize", draw);
  $("mini").addEventListener("click", function () { tab("path"); });
  function tab(t) {
    document.body.classList.toggle("tpath", t === "path");
    Array.prototype.forEach.call($("tabs").children, function (b) { b.classList.toggle("on", b.dataset.t === t); });
    draw();
  }
  $("tabs").addEventListener("click", function (e) { if (e.target.dataset.t) tab(e.target.dataset.t); });

  // ---------- programs and drafts (§21.8) ----------
  function prog(name) { return "/logo/programs/" + encodeURIComponent(name); }
  function loadList() {
    return http.send("GET", "/logo/programs").then(function (r) {
      if (r.status === 200 && r.data && r.data.programs) { progs = r.data.programs; showProgs(); }
    });
  }
  function label(p) { return p.name; }
  function showProgs() {
    var sel = $("psel"), ul = $("plist"), mine = progs.filter(function (p) { return !p.example; }), ex = progs.filter(function (p) { return p.example; });
    sel.textContent = ""; ul.textContent = "";
    function item(p, isNew) {
      var name = isNew ? NEW : p.name, on = (cur.name || NEW) === name, dot = drafts.unsaved(name);
      var o = el("option", "", (isNew ? "(new program)" : label(p)) + (dot ? " ●" : "")); o.value = name; o.selected = on; sel.appendChild(o);
      var li = el("li", (on ? "on" : "") + (dot ? " dot" : ""), (on ? "> " : "") + (isNew ? "(new program)" : label(p)));
      li.addEventListener("click", function () { openProg(name); }); ul.appendChild(li);
    }
    ul.appendChild(el("li", "sec", "SAVED ON THE TANK"));
    item(null, true); mine.forEach(function (p) { item(p, false); });
    ul.appendChild(el("li", "sec", "[ EXAMPLES ]"));
    ex.forEach(function (p) { item(p, false); });
    $("title").textContent = "[ 01 // PROGRAM : " + (cur.name || "new") + " ]" + (drafts.unsaved(cur.name || NEW) ? " ●" : "");
  }
  $("psel").addEventListener("change", function () { openProg($("psel").value); });
  function openProg(name) {
    if (L.locked(bar)) { showProgs(); return; }
    var p = progs.filter(function (q) { return q.name === name; })[0], d = drafts.restore(name);
    cur = { name: name === NEW ? "" : name, example: !!(p && p.example), saved: "" };
    remember("cq.logo.last", name);
    if (name === NEW) { cur.saved = ""; setText(d ? d.text : START_TEXT); showProgs(); return; }
    http.send("GET", prog(name)).then(function (r) {
      if (r.status !== 200 || typeof r.data !== "string") { log("Could not open " + name + (r.status === 0 ? " - not connected." : "."), "end"); return; }
      cur.saved = r.data;
      if (d && d.text !== r.data) {  // a draft is never hidden (§106); the tank's copy may have changed meanwhile
        setText(d.text); drafts.keep(name, d.text, r.data);
        if (d.saved !== r.data) log(name + " was changed on the tank since your draft - SAVE keeps your draft.", "end");
      } else setText(r.data);
      showProgs();
    });
  }
  $("pnew").addEventListener("click", function () { openProg(NEW); });
  function askName(title, start, then) {
    $("nameT").textContent = title; $("nameI").value = start; $("nameW").textContent = ""; $("nameS").classList.add("on");
    $("nameI").focus();
    $("nameO").onclick = function () {
      var n = $("nameI").value.trim().toLowerCase();
      if (!/^[a-z0-9_-]{1,24}$/.test(n)) { $("nameW").textContent = "Use 1-24 of a-z, 0-9, _ or -"; return; }
      then(n);
    };
  }
  $("nameC").addEventListener("click", function () { $("nameS").classList.remove("on"); });
  var WHY = { NAME: "That name cannot be used.", FULL: "The tank is full (32 programs) - delete one first.",
    TOO_BIG: "The program is too long (8 KB).", EXAMPLE: "Examples cannot be changed - pick a new name.",
    RUNNING: "Not while a program runs." };
  function save(name, text) {
    return http.send("PUT", prog(name), text).then(function (r) {
      var m = r.data;
      if (r.status === 0) return "Not connected to the tank.";
      if (!m || typeof m !== "object") return r.status === 503 ? "The tank is busy - try again." : "Not saved.";
      return m.ok ? "" : WHY[m.why] || m.why || "Not saved.";
    });
  }
  function saveAs(title, then) {
    var replace = "";
    askName(title, cur.example ? "" : cur.name, function (n) {
      var text = src.value, taken = progs.some(function (p) { return p.name === n; });
      if (taken && n !== cur.name && replace !== n) {  // never replace another program without a second press
        replace = n; $("nameW").textContent = n + " is already on the tank - press OK again to replace it."; return;
      }
      save(n, text).then(function (why) {
        if (why) { $("nameW").textContent = why; return; }
        $("nameS").classList.remove("on");
        drafts.clear(cur.name || NEW);
        var old = cur.name; cur = { name: n, example: false, saved: text }; remember("cq.logo.last", n);
        if (then) then(old);
        loadList();
      });
    });
  }
  $("psave").addEventListener("click", function () {
    if (!cur.name || cur.example) { saveAs("SAVE AS"); return; }
    var text = src.value;
    save(cur.name, text).then(function (why) {
      if (why) { log(why, "end"); return; }
      cur.saved = text; drafts.clear(cur.name); log("saved " + cur.name, ""); loadList();
    });
  });
  $("pren").addEventListener("click", function () {
    if (!cur.name || cur.example) { saveAs("SAVE AS"); return; }
    saveAs("RENAME", function (old) {
      if (old && old !== cur.name) http.send("DELETE", prog(old)).then(loadList);
    });
  });
  $("pdel").addEventListener("click", function () {
    if (!cur.name) { setText(""); drafts.clear(NEW); showProgs(); return; }
    if (cur.example) { log(WHY.EXAMPLE, "end"); return; }
    $("delT").textContent = "Delete " + cur.name + " from the tank?"; $("delS").classList.add("on");
  });
  $("delC").addEventListener("click", function () { $("delS").classList.remove("on"); });
  $("delO").addEventListener("click", function () {
    var name = cur.name;
    http.send("DELETE", prog(name)).then(function (r) {
      var m = r.data;
      $("delS").classList.remove("on");
      if (!m || typeof m !== "object" || !m.ok) { log(m && WHY[m.why] || "Not deleted.", "end"); return; }
      drafts.clear(name); loadList().then(function () { openProg(NEW); });
    });
  });
  $("mbtn").addEventListener("click", function () { $("menu").classList.toggle("on"); });

  // ---------- tank link (bridge R11: no link while the page is hidden) ----------
  function bars(rssi) { return rssi >= -55 ? "▂▄▆█" : rssi >= -65 ? "▂▄▆" : rssi >= -75 ? "▂▄" : "▂"; }
  function onMessage(m) {
    switch (m.t) {
      case "welcome": $("cUno").textContent = m.tank && m.tank.fw && m.tank.fw.uno ? "● UNO ONLINE" : "● UNO OFFLINE"; break;
      case "ts":
        if (m.batt_v !== null && m.batt_v !== undefined) $("cBatt").textContent = m.batt_v < 5.5 ? "USB" : m.batt_v.toFixed(2) + " V";
        var off = (m.flags || []).indexOf("body_offline") >= 0;
        $("cUno").textContent = off ? "● UNO OFFLINE" : "● UNO ONLINE"; $("cUno").className = "chip" + (off ? " warn" : "");
        break;
      case "tf": $("sDist").textContent = m.dist_cm === null || m.dist_cm === undefined ? "--" : m.dist_cm + " cm"; break;
      case "wifi_status": $("cWifi").textContent = m.mode === "home" ? "HOME " + bars(m.rssi) : "OWN WIFI"; $("cWifi").className = "chip"; break;
      case "ev": if (m.kind && m.kind.indexOf("logo_") === 0) onEvent(m); break;
      case "ack": if (m.id === runId) { onBar({ e: "ack" }); lockEditor(); } break;
      case "nack":
        if (m.id === runId) onBar({ e: "nack", reason: m.reason, message: m.message });
        else if (m.id === stepId) log(m.message || "STEP refused", "end");
        break;
      default: break;
    }
  }
  function connect() {
    if (document.hidden || (ws && ws.readyState <= 1)) return;
    ws = new WebSocket("ws://" + location.host + "/ws");
    ws.onopen = function () {
      open = true; retry = 500; send({ t: "hello", client: "logo", proto: 1 }); send({ t: "wifi_status" });
      if (linked) loadList();  // a new link may be a restarted bridge; the first list comes from the start below
      linked = true; showBar();
    };
    ws.onmessage = function (e) { var m; try { m = JSON.parse(e.data); } catch (x) { return; } onMessage(m); };
    ws.onclose = function () {
      open = false; $("cWifi").textContent = "RECONNECTING"; $("cWifi").className = "chip warn";
      onBar({ e: "closed" }); lockEditor();
      setTimeout(connect, retry); retry = Math.min(retry * 2, 5000);
    };
    ws.onerror = function () { ws.close(); };
  }
  setInterval(function () { send({ t: "ping", t0: Date.now() }); }, 1000);
  document.addEventListener("visibilitychange", function () {
    if (document.hidden) { onBar({ e: "hidden" }); if (ws && ws.readyState <= 1) ws.close(); } else { retry = 500; connect(); lockEditor(); }
  });

  showPad();
  var last = recall("cq.logo.last") || NEW, d0 = drafts.restore(NEW);
  if (last === NEW) setText(d0 ? d0.text : START_TEXT);
  connect();
  loadList().then(function () { if (last !== NEW) openProg(last); else showProgs(); });
  tab("prog");
})();
</script>
</body>
</html>
```

- [ ] **Step 4: Pack it and build the bridge**

Run: `cd firmware/esp32 && python3 scripts/pack_web.py . && pio run -e bridge`
Expected: `pack_web: … /logo 19254 B …` (it must stay ≤ 24576 B), `[SUCCESS]`, flash ≈ 44 %. The PlatformIO run prints the same `pack_web:` line (its branch still runs under SCons).

- [ ] **Step 5: The stand-in tank** — `firmware/esp32/web/test/fake_tank.py`:

```python
"""A stand-in tank for the /logo browser check (Tank Logo LLD §21.9): HTTP only, standard library.

Serves the packed /logo page and answers /logo/check and /logo/programs like the bridge
(LLD §20.4), one request at a time like its logo worker (a second gets 503 busy). The WebSocket
is faked inside the browser by the check itself (an injected WebSocket class), as in Plans M, O.
    python3 firmware/esp32/web/test/fake_tank.py [port]   -> http://127.0.0.1:8765/logo
A check refuses any text containing FORWAD (7001 at that word); a text containing "; bigpath"
gets a 2000-point spiral (the bridge's most) cut short; any other text is OK with a 50 cm square.
"""

from __future__ import annotations

import json
import math
import pathlib
import re
import runpy
import sys
import threading
import time
from collections.abc import Callable
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, cast
from urllib.parse import urlsplit

PROJECT = pathlib.Path(__file__).resolve().parents[2]
_PACK = runpy.run_path(str(PROJECT / "scripts" / "pack_web.py"), run_name="pack_web")
assemble_logo = cast(Callable[[pathlib.Path], bytes], _PACK["assemble_logo"])
gz = cast(Callable[[bytes], bytes], _PACK["gz"])

PROGRAMS: dict[str, str] = {"square": "REPEAT 4 [ FD 50 RT 90 ]\n"}
EXAMPLES = {"first_square": "; a square\nREPEAT 4 [ FORWARD 50 RIGHT 90 ]\n"}
SQUARE = [
    [0, 0, 0, 1, 0],
    [0, 500, 0, 1, 0],
    [0, 500, 90, 1, 0],
    [500, 500, 90, 1, 0],
    [500, 500, 180, 1, 0],
    [500, 0, 180, 1, 0],
    [500, 0, 270, 1, 1],
    [0, 0, 270, 1, 1],
]
state = {"prog": 0, "busy": 0}
WORKER = threading.Lock()


def check(text: str) -> dict[str, Any]:
    state["prog"] += 1
    for n, line in enumerate(text.split("\n"), 1):
        m = re.search(r"FORWAD", line, re.IGNORECASE)
        if m:
            diag = {
                "code": 7001,
                "line": n,
                "col": m.start() + 1,
                "msg": "I don't know FORWAD. Did you mean FORWARD?",
                "warning": False,
            }
            return {
                "ok": False,
                "prog": state["prog"],
                "diags": [diag],
                "words": [],
                "handles_obstacles": False,
                "path": [],
                "cut_short": False,
            }
    path, cut = SQUARE, False
    if "; bigpath" in text:
        path = [
            [round(math.sin(i / 20) * i), round(math.cos(i / 20) * i), i % 360, 1, int(i > 1000)]
            for i in range(2000)
        ]
        cut = True
    words = [
        {"name": w.upper(), "inputs": 0, "gives": False}
        for w in re.findall(r"\bTO\s+(\w+)", text, re.I)
    ]
    return {
        "ok": True,
        "prog": state["prog"],
        "diags": [],
        "words": words,
        "handles_obstacles": False,
        "path": path,
        "cut_short": cut,
    }


class Handler(BaseHTTPRequestHandler):
    def reply(self, code: int, body: bytes, ctype: str, gz: bool = False) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        if gz:
            self.send_header("Content-Encoding", "gzip")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def json(self, obj: Any, code: int = 200) -> None:
        self.reply(code, json.dumps(obj).encode(), "application/json")

    def body(self) -> str:
        return self.rfile.read(int(self.headers.get("Content-Length") or 0)).decode("utf-8")

    def where(self) -> str:
        return urlsplit(self.path).path

    def name(self) -> str:
        return self.where().rsplit("/", 1)[1]

    def job(self, work: Callable[[], None]) -> None:
        """Like the bridge's logo worker: one /logo/* request at a time, a second one gets 503."""
        if not WORKER.acquire(blocking=False):
            self.body()  # the bridge drains the body too
            state["busy"] += 1
            return self.json({"ok": False, "why": "BUSY"}, 503)
        try:
            time.sleep(0.05)  # a check takes ~40 ms on the bridge
            work()
        finally:
            WORKER.release()

    def do_GET(self) -> None:
        path = self.where()
        if path == "/logo":
            return self.reply(200, gz(assemble_logo(PROJECT)), "text/html; charset=utf-8", True)
        if path == "/fake/stats":  # for the browser check: how many requests were refused busy
            return self.json({"busy": state["busy"]})
        if path == "/logo/programs":
            return self.job(self.list)
        if path.startswith("/logo/programs/"):
            return self.job(self.load)
        self.reply(404, b"", "text/plain")

    def list(self) -> None:
        rows = [{"name": n, "bytes": len(t), "example": False} for n, t in sorted(PROGRAMS.items())]
        rows += [{"name": n, "bytes": len(t), "example": True} for n, t in EXAMPLES.items()]
        self.json({"programs": rows})

    def load(self) -> None:
        text = PROGRAMS.get(self.name(), EXAMPLES.get(self.name()))
        if text is None:
            return self.json({"ok": False, "why": "NOT_FOUND"}, 404)
        self.reply(200, text.encode(), "text/plain; charset=utf-8")

    def do_POST(self) -> None:
        if self.where() != "/logo/check":
            return self.reply(404, b"", "text/plain")
        self.job(lambda: self.json(check(self.body())))

    def do_PUT(self) -> None:
        self.job(self.save)

    def save(self) -> None:
        n, text = self.name(), self.body()
        if n in EXAMPLES:
            return self.json({"ok": False, "why": "EXAMPLE"}, 409)
        if not re.fullmatch(r"[a-z0-9_-]{1,24}", n):
            return self.json({"ok": False, "why": "NAME"}, 409)
        PROGRAMS[n] = text
        self.json({"ok": True})

    def do_DELETE(self) -> None:
        self.job(self.remove)

    def remove(self) -> None:
        n = self.name()
        if n in EXAMPLES:
            return self.json({"ok": False, "why": "EXAMPLE"}, 409)
        PROGRAMS.pop(n, None)
        self.json({"ok": True})

    def log_message(self, format: str, *args: Any) -> None:
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    print(f"fake tank: http://127.0.0.1:{port}/logo", flush=True)
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
```

Run: `cd tools/smoke && uv run ruff check ../../firmware/esp32/web/test/fake_tank.py && uv run ruff format --check ../../firmware/esp32/web/test/fake_tank.py && uv run pyright ../../firmware/esp32/web/test/fake_tank.py`
Expected: clean, `0 errors`.

Run (from the repo root, in the background): `python3 firmware/esp32/web/test/fake_tank.py 8765`, then `curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8765/logo` and `curl -s -X POST --data "FORWAD 5" http://127.0.0.1:8765/logo/check`
Expected: `200`; `{"ok": false, "prog": 1, "diags": [{"code": 7001, "line": 1, "col": 1, …`.

- [ ] **Step 6: Size check and the Node tests still green**

Run: `python3 scripts/size_check.py && node --test firmware/esp32/web/test/logo.test.js`
Expected: no size findings; `ℹ pass 43`.

- [ ] **Step 7: Commit**

```bash
git add firmware/esp32/web/logo.html firmware/esp32/web/test/fake_tank.py firmware/esp32/scripts/pack_web.py
git commit -m "feat(web): the /logo page, packed within 24 KB, and a stand-in tank for the browser check

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Menu and Help links, and the browser check

**Files:**
- Modify: `firmware/esp32/web/basic_drive.html`, `firmware/esp32/web/help.html`, `firmware/esp32/web/help_logo.html`

**Interfaces:**
- Consumes: `/logo` (Task 3), the stand-in tank (Task 3 step 5).
- Produces: the Drive page's ☰ MENU opens `/logo`; Help links to it and describes Tank Logo v1.

- [ ] **Step 1: The Drive menu item and Help's phone line** — apply:

```diff
diff --git a/firmware/esp32/web/basic_drive.html b/firmware/esp32/web/basic_drive.html
index b4d7749..21ed785 100644
--- a/firmware/esp32/web/basic_drive.html
+++ b/firmware/esp32/web/basic_drive.html
@@ -230,7 +230,7 @@ input[type=range] { width: 100%; accent-color: var(--ui); }
 </div>
 <div id="dim" class="ov"></div>
 <div id="menuSheet" class="ov sheet glass"><p class="title">[ MENU ]</p>
-  <a class="mi off" id="miLogo" aria-disabled="true"><svg viewBox="0 0 24 24"><polyline points="16 18 22 12 16 6"/><polyline points="8 6 2 12 8 18"/></svg><span><b>LOGO</b><small>write and run programs</small></span><i class="bdg">COMING SOON</i></a>
+  <a class="mi" id="miLogo" href="/logo"><svg viewBox="0 0 24 24"><polyline points="16 18 22 12 16 6"/><polyline points="8 6 2 12 8 18"/></svg><span><b>LOGO</b><small>write and run programs</small></span><i>›</i></a>
   <a class="mi" id="miSettings" href="/settings"><svg viewBox="0 0 24 24"><path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z"/><circle cx="12" cy="12" r="3"/></svg><span><b>SETTINGS</b><small>WiFi · tank · tune · log</small></span><i>›</i></a>
   <a class="mi off" id="miAi" target="_blank" rel="noopener" aria-disabled="true"><svg viewBox="0 0 24 24"><path d="M12 3l1.9 5.8L20 10l-6.1 1.9L12 18l-1.9-6.1L4 10l6.1-1.2z"/></svg><span><b>AI</b><small id="miAiTxt">set its address in Settings</small></span><i class="bdg" id="miAiBdg">NOT SET UP</i></a>
   <a class="mi" id="miHelp" href="/help"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="10"/><path d="M9.09 9a3 3 0 0 1 5.83 1c0 2-3 3-3 3"/><path d="M12 17h.01"/></svg><span><b>HELP</b><small>how-to and facts</small></span><i>›</i></a>
diff --git a/firmware/esp32/web/help.html b/firmware/esp32/web/help.html
index 8ed7a07..e772985 100644
--- a/firmware/esp32/web/help.html
+++ b/firmware/esp32/web/help.html
@@ -60,7 +60,7 @@ link <span id="unoSynced">?</span>, last reset <span id="unoReset">?</span></td>
 
 <h2>[ DRIVE ]</h2>
 <ul>
-<li>Phone: open <a href="/">/</a> - <b>☰ MENU</b> (Logo - coming soon, Settings, AI - once its address is saved, Help), stick (release = stop), big STOP, max speed, camera stick. One driver at a time;
+<li>Phone: open <a href="/">/</a> - <b>☰ MENU</b> (<a href="/logo">Logo</a>, Settings, AI - once its address is saved, Help), stick (release = stop), big STOP, max speed, camera stick. One driver at a time;
 others see "Busy". The IR remote beats everyone.</li>
 <li><b>STOP from any device holds:</b> after a STOP from a phone that is not driving, the driving phone is refused
 ("Stopped by another device") until its driver lets go of the stick.</li>
```

- [ ] **Step 2: Help's Logo section for v1** — replace `firmware/esp32/web/help_logo.html` with:

```html
<!-- Tank Logo quick reference (hand-written; keep in step with logo/spec/words.yaml and Tank Logo LLD §21) -->
<section id="logo">
<h2>[ TANK LOGO - QUICK REFERENCE ]</h2>
<p class="note">Write, check and run programs on the <a href="/logo">Logo page</a> (☰ MENU → LOGO). The tank checks each
program as you type and draws its path before anything moves; RUN goes straight through, STEP runs one line per
press, STOP ends it at once.</p>
<table>
<tr><th>Move</th><td><code>FORWARD n</code>/<code>FD</code>, <code>BACK n</code>/<code>BK</code> (1-500 cm, needs the
encoders) &middot; <code>RIGHT a</code>/<code>RT</code>, <code>LEFT a</code>/<code>LT</code> (degrees, right is +) &middot;
<code>ARC RIGHT a r</code> (radius &ge; 31 cm) &middot; <code>PIVOT LEFT a</code> &middot; <code>SPEED p</code> (10-100 %)
&middot; <code>HOME</code> (face the start way again) &middot; <code>WAIT s</code> (&le; 60 s)</td></tr>
<tr><th>Look / light / sound</th><td><code>LOOK LEFT|RIGHT|UP|DOWN|HOME</code> &middot; <code>LED GREEN</code>
(RED GREEN BLUE YELLOW PURPLE WHITE ORANGE OFF) &middot; <code>BEEP</code> &middot; <code>SOUND HORN</code> (the tank's
24 sounds) &middot; <code>PLAY [ C4 E4 G4 2 ]</code> &middot; <code>SAY [ well done ]</code></td></tr>
<tr><th>Sensors</th><td><code>DISTANCE</code> (cm) &middot; <code>OBSTACLE?</code> &middot; <code>BLOCKED?</code> (the last
move was stopped by something in the way) &middot; <code>HEADING</code> &middot; <code>TILTED?</code> &middot;
<code>BATTERY</code> &middot; <code>TIMER</code> (s) &middot; <code>RANDOM n</code> &middot; <code>REPCOUNT</code></td></tr>
<tr><th>Language</th><td><code>REPEAT n [ ]</code> &middot; <code>FOREVER [ ]</code> &middot; <code>WHILE c [ ]</code>
&middot; <code>WAIT UNTIL c</code> &middot; <code>IF c [ ]</code> &middot; <code>IFELSE c [ ] [ ]</code> &middot;
<code>TO name :a &hellip; END</code> (your own words, recursion allowed) &middot; <code>OUTPUT x</code> &middot;
<code>AND OR NOT</code> &middot; <code>STOP</code> ends the whole program &middot; <code>; comment</code></td></tr>
<tr><th>Limits</th><td>5 min run time &middot; 8 KB of text &middot; 64 words of your own &middot; 100 calls deep
(calls at the end of a word use no depth) &middot; WAIT &le; 60 s &middot; FORWARD/BACK &le; 500 cm per step</td></tr>
<tr><th>Safety</th><td>Every move is a normal tank move: the obstacle brake, deadman, tilt and battery cut-offs apply.
Something in the way ends the program unless it uses <code>BLOCKED?</code>. STOP from anywhere (this page, a phone, the
IR remote, the <code>STOP</code> word) ends the program; hiding the Logo page ends a program started there.</td></tr>
</table>
<pre>REPEAT 4 [ FORWARD 50  RIGHT 90 ]   ; a square
FOREVER [ FORWARD 30  IF BLOCKED? [ BACK 10  RIGHT 90 ] ]</pre>
</section>
```

Run: `cd firmware/esp32 && python3 scripts/pack_web.py . && grep -c "COMING SOON" web/basic_drive.html`
Expected: `/help` ≈ 9 KB (≤ 16 KB), `/logo` unchanged; `0`.

- [ ] **Step 3: The browser check** — with the stand-in tank running (Task 3 step 5), run this Playwright script (phone size first), then the screenshots below:

```js
// Playwright (the MCP browser or `npx playwright`), stand-in tank on http://127.0.0.1:8765
async (page) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.addInitScript(() => {  // a stand-in WebSocket: records what the page sends, answers hello
    try { localStorage.clear(); } catch (e) { /* a fresh browser */ }
    window.__sent = [];
    class FakeWS {
      constructor(url) { this.url = url; this.readyState = 0; window.__ws = this;
        setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); }, 20); }
      send(s) { const m = JSON.parse(s); window.__sent.push(m);
        if (m.t === "hello") this.emit({ t: "welcome", you: "phone-1", tank: { fw: { uno: "1.0" } } });
        if (m.t === "wifi_status") this.emit({ t: "wifi_status", mode: "home", rssi: -54 });
        if (m.t === "logo_run" && window.__runReply) this.emit(Object.assign({ id: m.id }, window.__runReply)); }
      emit(o) { setTimeout(() => this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 5); }
      close() { this.readyState = 3; this.onclose && this.onclose(); }
    }
    window.WebSocket = FakeWS;
    window.__fakeEmit = (o) => window.__ws.emit(o);
  });
  const q = (f, a) => page.evaluate(f, a), st = () => q(() => document.getElementById("status").textContent);
  const out = {};
  await page.goto("http://127.0.0.1:8765/logo"); await page.waitForTimeout(1500);
  out.open = await st();                                                     // ● READY
  const ta = page.locator("#src");
  await ta.fill("TO SQUARE :size\n  REPEAT 4 [ FORWAD :size RIGHT 90 ]\nEND\nSQUARE 50\n");
  await page.waitForTimeout(1200);
  out.mistake = [await st(), await q(() => document.getElementById("diags").innerText)];  // 1 MISTAKE, line 2 quoted, ▲
  await ta.fill("TO SQUARE :size\n  REPEAT 4 [ FORWARD :size RIGHT 90 ]\nEND\nSQUARE 50\n");
  await page.waitForTimeout(1200);
  out.ready = [await st(), await q(() => document.getElementById("run").disabled)];        // READY, false
  await q(() => { window.__runReply = { t: "ack" }; });
  await page.click("#run"); await page.waitForTimeout(100);
  await q(() => { const f = window.__fakeEmit; f({ t: "ev", kind: "logo_line", line: 2, round: 2 });
    f({ t: "ev", kind: "logo_pose", x_cm: 0, y_cm: 50, heading: 0 }); f({ t: "ev", kind: "logo_pose", x_cm: 30, y_cm: 50, heading: 90 }); });
  await page.waitForTimeout(200);
  out.running = [await st(), await q(() => document.getElementById("src").readOnly),
    await q(() => document.querySelector("#hl div.run").textContent)];                      // RUNNING · line 2, true, ROUND 2
  await page.click("#stop");
  await q(() => window.__fakeEmit({ t: "ev", kind: "logo_ended", reason: 7040, line: 2, msg: "Stopped." }));
  await page.waitForTimeout(100);
  out.stopped = [await st(), await q(() => window.__sent.filter(m => m.t === "stop" || m.t === "logo_stop").map(m => m.t).join())];
  // step mode in the bridge's order: pause at 1, the move on line 1, pause at 2
  await page.click("#step"); await page.waitForTimeout(80);
  await q(() => window.__fakeEmit({ t: "ev", kind: "logo_line", line: 1, round: 0 })); await page.waitForTimeout(40);
  out.step1 = await st();                                                    // STEP · line 1
  await page.click("#run"); await page.waitForTimeout(40);
  await q(() => window.__fakeEmit({ t: "ev", kind: "logo_line", line: 1, round: 0 })); await page.waitForTimeout(40);
  out.stepMove = [await st(), await q(() => document.getElementById("run").disabled)];    // RUNNING, true
  await q(() => window.__fakeEmit({ t: "ev", kind: "logo_ended", reason: "finished", line: 4, msg: "" }));
  await page.waitForTimeout(60);
  // hidden page ends our run
  await page.click("#run"); await page.waitForTimeout(100);
  await q(() => { Object.defineProperty(document, "hidden", { configurable: true, get: () => true }); document.dispatchEvent(new Event("visibilitychange")); });
  await q(() => { Object.defineProperty(document, "hidden", { configurable: true, get: () => false }); document.dispatchEvent(new Event("visibilitychange")); });
  await page.waitForTimeout(1200); out.hidden = await st();                  // Ended (7046) … hidden
  // another device's program
  await q(() => window.__fakeEmit({ t: "ev", kind: "logo_pose", x_cm: 1, y_cm: 2, heading: 0 })); await page.waitForTimeout(60);
  out.other = [await st(), await q(() => document.getElementById("run").disabled)];       // ON ANOTHER DEVICE, true
  await q(() => window.__fakeEmit({ t: "ev", kind: "logo_ended", reason: 7040, line: 1, msg: "x" })); await page.waitForTimeout(60);
  // save as: an existing name asks twice; save while another client keeps the tank busy
  await page.click("#psave"); await page.fill("#nameI", "square"); await page.click("#nameO"); await page.waitForTimeout(300);
  out.replaceAsk = await q(() => document.getElementById("nameW").textContent);         // … press OK again to replace it.
  const hog = q(async () => Promise.all([0, 1, 2, 3, 4, 5].map(() => fetch("/logo/check", { method: "POST", body: "FD 1" }).then(r => r.status))));
  await page.fill("#nameI", "celebrate"); await page.click("#nameO");
  out.hog = await hog; await page.waitForTimeout(1500);                     // [200, 503, 503, …]
  out.saved = await q(() => document.getElementById("psel").textContent);    // … celebrate …
  await page.click("#pren"); await page.fill("#nameI", "party"); await page.click("#nameO"); await page.waitForTimeout(800);
  out.renamed = await q(() => document.getElementById("psel").textContent);  // party, no celebrate
  // a draft against a copy changed elsewhere
  await page.selectOption("#psel", "square"); await page.waitForTimeout(500);
  await q(() => { const s = document.getElementById("src"); s.value = "REPEAT 4 [ FD 70 RT 90 ]\n"; s.dispatchEvent(new Event("input")); });
  await q(async () => { await fetch("/logo/programs/square", { method: "PUT", body: "FD 5\n" }); });
  await page.selectOption("#psel", "first_square"); await page.waitForTimeout(500);
  out.exampleAsks = await (async () => { await page.click("#psave"); await page.waitForTimeout(80);
    const t = await q(() => document.getElementById("nameT").textContent); await page.click("#nameC"); return t; })();  // SAVE AS
  await page.selectOption("#psel", "square"); await page.waitForTimeout(700);
  out.draft = [await q(() => document.getElementById("src").value), await q(() => document.getElementById("log").innerText.split("\n").pop())];
  // delete
  await page.selectOption("#psel", "party"); await page.waitForTimeout(500);
  await page.click("#pdel"); await page.click("#delO"); await page.waitForTimeout(800);
  out.deleted = await q(() => document.getElementById("psel").textContent);  // no party
  // word button: inside brackets, number selected
  await q(() => { const s = document.getElementById("src"); s.value = "REPEAT 4 [  ]"; s.setSelectionRange(11, 11); s.dispatchEvent(new Event("input")); });
  await page.click("#pad button:has-text('FORWARD')"); await page.waitForTimeout(50);
  out.insert = await q(() => { const s = document.getElementById("src"); return [s.value, s.value.slice(s.selectionStart, s.selectionEnd)]; });
  // an 8 KB text and a 2000-point path
  const big = "REPEAT 4 [ FORWARD 50 RIGHT 90 ] ; a square side by side\n".repeat(140).slice(0, 8000);
  out.colourMs = await q((t) => { const s = document.getElementById("src"); s.value = t; const t0 = performance.now();
    for (let i = 0; i < 10; i++) s.dispatchEvent(new Event("input")); return (performance.now() - t0) / 10; }, big);
  await q(() => { const s = document.getElementById("src"); s.value = "; bigpath\nFOREVER [ FD 10 RT 5 ]\n"; s.dispatchEvent(new Event("input")); });
  await page.waitForTimeout(1200);
  out.pathMs = await q(() => { const t0 = performance.now(); window.dispatchEvent(new Event("resize")); return performance.now() - t0; });
  out.cutShort = await q(() => document.getElementById("pnote").textContent);  // path cut short …
  return out;
}
```

Expected (`out`):
- `open` "● READY"; `mistake` "● 1 MISTAKE - see under the program" and the diag text quoting line 2 with a ▲ under `FORWAD` and "(7001)"; `ready` ["● READY", false];
- `running` ["● RUNNING · line 2 · 0:00", true, the line text with "ROUND 2"]; `stopped` ["Ended (7040) on line 2: Stopped.", "stop,logo_stop"];
- `step1` "● STEP · line 1 - press STEP for the next line"; `stepMove` ["● RUNNING · line 1 · 0:00", true];
- `hidden` "Ended (7046): Stopped: the page was hidden or the link dropped."; `other` ["● A PROGRAM IS RUNNING ON ANOTHER DEVICE", true];
- `replaceAsk` "square is already on the tank - press OK again to replace it."; `hog` starts with 200 and has 503s; `saved` lists `celebrate`; `renamed` lists `party` and no `celebrate`;
- `exampleAsks` "SAVE AS"; `draft` ["REPEAT 4 [ FD 70 RT 90 ]\n", "… square was changed on the tank since your draft - SAVE keeps your draft."];
- `deleted` without `party`; `insert` ["REPEAT 4 [ FORWARD 50 ]", "50"];
- `colourMs` below 5 (the key handling; the colouring itself runs once per screen frame and took ≈ 11 ms for 8 KB in Chromium); `pathMs` below ≈ 10 (0.8–1.4); `cutShort` "path cut short (a limit was reached)".
- The console shows only failed-resource lines for the 503s and the stand-in's missing `/icon-192.png`.

Screenshots for the owner: phone 390 × 844 — the PROGRAM tab with a mistake, the PROGRAM tab running, the PATH tab; laptop 1440 × 900 — running with the driven path. Compare with screens 15 / 16 (no SPD, no PAUSE).

- [ ] **Step 4: Stop the stand-in tank and commit**

```bash
git add firmware/esp32/web/basic_drive.html firmware/esp32/web/help.html firmware/esp32/web/help_logo.html
git commit -m "feat(web): Logo in the Drive menu and Help; Help's Tank Logo section for v1

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: On the stand (owner present, STOP within reach)

**Files:** none (results recorded in Task 6).

Ask the owner **"stand or floor?"** — this task is **stand** only: tracks off the ground; say before each run what will move (both tracks turn; the camera may move for LOOK).

- [ ] **Step 1: Flash the bridge** — no partition change since N3, so OTA works (bridge LLD R2): read the OTA password into `CQ_OTA_PASSWORD` without printing it (`sed` from `include/secrets.h`), `export` it, then `cd firmware/esp32 && pio run -e bridge_ota -t upload 2>&1 | grep -vF -- "$CQ_OTA_PASSWORD" > /tmp/ota.log; tail -3 /tmp/ota.log; rm /tmp/ota.log`. If OTA fails, USB from the Linux machine as in N3 (dev-setup).
Expected: `[SUCCESS]`; `/info` shows the new build; `curl -s -o /dev/null -w "%{http_code} %{size_download}\n" http://<bridge>/logo` → `200` and ≈ 19 KB (served gzipped).

- [ ] **Step 2: Open the page from the phone** — Drive → ☰ MENU → LOGO.
Expected: the page opens; chips show battery, WiFi, UNO ONLINE; the program list shows the saved programs and the three examples; no "COMING SOON".

- [ ] **Step 3: A mistake, then the plan** — type `REPEAT 4 [ FORWAD 50 RIGHT 90 ]`.
Expected: within about 1 s the word is underlined, the gutter shows ▲, the sentence is under the editor; fix it → "● READY", the PATH tab shows the square.

- [ ] **Step 4: RUN, STEP, STOP** — say "both tracks will turn". RUN `REPEAT 2 [ FD 30 BK 30 ]`: the line glows, "RUNNING · line 1", the driven path grows, the program ends "Done". STEP the same program: it waits at line 1 with NEXT lit; NEXT → the tracks turn and NEXT stays off until the next pause; STOP mid-move → the tracks stop at once, "Ended (7040)".

- [ ] **Step 5: Hidden page** — RUN `FOREVER [ WAIT 1 ]`, lock the phone for 5 s, unlock.
Expected: the program ended (7046, also in Settings → Log); the page says so and reconnects.

- [ ] **Step 6: Another device** — on a second phone open `/logo`; on the first RUN `FOREVER [ WAIT 1 ]`.
Expected: the second shows "● A PROGRAM IS RUNNING ON ANOTHER DEVICE", RUN off; its STOP ends the program (7040) and the first page shows the ending.

- [ ] **Step 7: Programs and drafts** — SAVE AS `stand_test`; RENAME to `stand_test2`; edit it without saving and reload the page → the draft is back with the dot; open `first_square` and SAVE → asks for a new name; DELETE `stand_test2` (confirm).
Expected: as described; the list on a second phone matches after a reload.

- [ ] **Step 8: Long text on the phone** — paste an 8 KB program (e.g. the line `REPEAT 4 [ FORWARD 50 RIGHT 90 ] ; a square` repeated) and type in it.
Expected: typing keeps up; the tank answers the check (8 KB is the limit; one more character gives 7010).

- [ ] **Step 9: Record** — write the results (pass / what differed) for Task 6. Floor checks of the page (the square's driven path against its plan) go with N3 Task 13 after the road-wheel fix.

---

### Task 6: Docs

**Files:**
- Modify: `docs/lld/tank-logo-lld.md`, `docs/design-notes.md`, `CLAUDE.md`

- [ ] **Step 1: LLD §21.10 "As built"** — append to `docs/lld/tank-logo-lld.md`:

```markdown
### 21.10 As built (Plan N4, 2026-10-19)
- The thirteen rules of Plan N4 (owner-confirmed, design-notes §107) hold. Corrections to §21.1–§21.9:
  - **§21.2:** no `http_server.cpp` change - `pack_web.py` adds `/logo` to the asset table (≤ 16 of 20 URI handlers in use); `pack_web.py` gains `assemble_logo()` and can be loaded by the stand-in tank; Help's Logo section (`web/help_logo.html`) is rewritten for v1; `scripts/check-protocol.sh` finds `logo/` from its own folder.
  - **§21.4:** a `RequestQueue` in `logo.js` sends one `/logo` request at a time (the bridge's worker takes one job; `503 busy` retried every 0.5 s for 10 s).
  - **§21.5 / §14:** mistakes are marked in the editor (underline, ▲ in the gutter) and written just under the editor, quoting the line with a ▲ under the word (a text box cannot open a gap under a line).
  - **§21.6:** in step mode a pause is a `logo_line` with a new line number; the moves on the paused line send `logo_line` too and do not light NEXT.
  - **§21.8:** a draft is never hidden (shown even when the tank's copy changed, with a note); SAVE AS onto an existing name needs a second press.
- **Files:** `web/logo.html`, `web/logo.js`, `web/test/logo.test.js` (43 Node tests), `web/test/fake_tank.py` (the stand-in tank for the browser check); `words.yaml` `group`.
- **Sizes:** `/logo` 19.3 KB gzipped (budget 24 KB); bridge flash 44 %.
```

Fill the stand results line from Task 5 into design-notes (step 2), not here.

- [ ] **Step 2: design-notes §107** — append to `docs/design-notes.md`:

```markdown
## 107. Plan N4 — the `/logo` page (2026-10-19)
- **Rules confirmed by the owner at plan review:** the thirteen rules of the plan (`docs/superpowers/plans/2026-10-19-plan-n4-logo-page.md`): mistakes under the editor, one `/logo` request at a time, the step-mode pause rule, drafts never hidden, SAVE AS asks before replacing, level-3 words "later", sense / maths colours, the short ☰ MENU, Help's Logo section for v1, `hello` client `logo` and `?name=`, `pack_web` loadable, the hook fix, no `http_server.cpp` change.
- **Built:** `web/logo.html` + `web/logo.js` (43 Node tests), `/logo` 19.3 KB gzipped; browser check against a stand-in tank that answers one request at a time (all cases of the plan's Task 4 pass; colouring an 8 KB text ≈ 11 ms, a 2000-point path 1.4 ms in Chromium).
- **Stand (owner present):** one line per Task 5 step (1-8): passed, or what differed and what was done about it.
- **Floor:** the square's driven path against its plan, with N3 Task 13 after the road-wheel fix.
```

- [ ] **Step 3: CLAUDE.md** — in item 18 of "Plan execution order", after the N3 sentence add: `**Plan N4 — the /logo page** (editor, check as you type, path, RUN / STEP / STOP, programs and drafts; Tank Logo LLD §21) — done 2026-10-19, stand-checked (design-notes §107).` and change "**Next:** Tank Logo Plan N4 (the `/logo` page); then" to "**Next:**"; add `2026-10-19-plan-n4-logo-page.md` to the plans row of "Where things are".

- [ ] **Step 4: Commit**

```bash
git add docs/lld/tank-logo-lld.md docs/design-notes.md CLAUDE.md
git commit -m "docs: Tank Logo LLD §21.10 (N4 as built), design-notes §107, CLAUDE.md

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```


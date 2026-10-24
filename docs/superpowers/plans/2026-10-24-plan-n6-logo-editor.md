# Plan N6 — The `/logo` page as an editor on the laptop — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On a laptop (900 px wide or more) the Logo page fills the window like a code editor: the page never scrolls, only its panes do. It has a PROBLEMS / MESSAGES panel, a status bar, keyboard shortcuts and the camera above the path. The phone layout stays as it is.

**Architecture:**
- **Three pure functions in `web/logo.js`**, tested by Node:
  - `shortcut` (which key does what);
  - `panelTab` (which panel tab shows);
  - `cameraWanted` (when the camera streams).
- **In `web/logo.html`:**
  - CSS grid areas lay out the laptop page;
  - the word row moves above the editor by CSS `order`;
  - `applyLayout` moves two elements (the problems into the bottom panel, the help line into the status bar) when the width crosses 900 px, and back;
  - the camera pane streams `:81/stream` only while shown.
- **No bridge, protocol or Logo engine change.**

**Tech Stack:** plain JS / CSS in `firmware/esp32/web/logo.html` + `logo.js`; Node built-in test runner; Playwright `run_code` checks against `web/test/fake_tank.py`; `scripts/pack_web.py` packs `/logo` (24 KB gzipped budget).

**Spec:** `docs/lld/tank-logo-lld.md` §23 (revision N6, approved 2026-10-24); design-notes §147.

**Built from a tested draft:** branch `n6-draft`.
- Node 52/52; `/logo` 22.7 KB gzipped (was 20.1).
- The new laptop check passes at 1280 × 800 and 1024 × 700.
- The N4 phone check (`browser_check.js`) still passes unchanged.

## Global Constraints
- **Laptop = `(min-width: 900px)`;** under it the §21 phone layout is unchanged (it scrolls).
- **Shortcuts:**
  - **Esc = STOP** (always, even held down, even in the editor or a dialog, which it also closes);
  - **Ctrl/⌘ + Enter = RUN**;
  - **F10 = STEP**;
  - **Ctrl/⌘ + S = save**.
  - Each does exactly what its button does; a disabled button means no action. With Alt held, or a held key (repeat), no action except Esc.
- **The camera** streams only when the layout is wide, HIDE is not pressed, and the page is visible. HIDE is kept in `localStorage` `logo.cam` = `"off"`.
- **Fixed pane sizes:**
  - columns `220px minmax(0, 1fr) minmax(320px, 34%)`;
  - bottom panel `12em`;
  - camera 4:3.
- **`/logo` ≤ 24 KB gzipped** (`pack_web.py`).
- **Hardware (Task 4):**
  - the bridge flashed over WiFi; never print the OTA password;
  - before any drive, ask "stand or floor?" and say what moves;
  - camera pictures stay on screen only (nothing recorded).

## Review Focus
1. **Esc must stop from anywhere**: in the editor, with a dialog open, with the key held. Pinned by `test a held key only ever stops` (Node) and the laptop check's `esc` / `escSheet` lines.
2. **A held Ctrl+Enter or F10 must not start a program twice.** Pinned by the Node shortcut tests (repeat → null).
3. **The phone page must not change.** Pinned by the N4 `browser_check.js` passing unchanged, and the laptop check's `phone` line (no stream; problems and help back under the editor).
4. **The camera must not stream when it can't be seen** (phone width, HIDE, hidden page). Uses a viewer slot of the bridge's 2. Pinned by the `cameraWanted` tests and the check's `camHidden` / `phone` lines.
5. **No page scroll on the laptop with long content** (300 lines, 200 messages). Pinned by the check's `fits1280` / `fits1024` lines.

---

### Task 1: The three pure functions

**Files:**
- Modify: `firmware/esp32/web/logo.js` (before its `return {`, and the export list)
- Test: `firmware/esp32/web/test/logo.test.js`

**Interfaces:**
- Produces:
  - `LogoCore.shortcut(key, ctrl, meta, alt, shift, repeat) → "stop" | "run" | "step" | "save" | null`;
  - `LogoCore.panelTab({tab, picked}, ev) → {tab, picked}`, with `ev` one of:
    - `{e: "pick", tab}`;
    - `{e: "started"}`;
    - `{e: "problems", n, running}`;
  - `LogoCore.cameraWanted(wide, hiddenByUser, pageHidden) → bool`.

- [ ] **Step 1: Write the failing tests** (append to `test/logo.test.js`):

```js

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
```

- [ ] **Step 2: Run them to see them fail**

Run: `node --test firmware/esp32/web/test/logo.test.js 2>&1 | grep -E "^ℹ (pass|fail)"`
Expected: `ℹ pass 46`, `ℹ fail 6` (`L.shortcut is not a function`, …).

- [ ] **Step 3: Write the functions** in `logo.js`, just before `  return {`:

```js
  // ---------- revision N6: the laptop layout (§23) ----------
  // A key -> "stop" | "run" | "step" | "save" | null. Esc always stops, even held down; the others never repeat
  // (a held Ctrl+Enter must not start a program twice). Alt is left to the browser.
  function shortcut(key, ctrl, meta, alt, shift, repeat) {
    if (key === "Escape") return "stop";
    if (alt || repeat) return null;
    var cmd = ctrl || meta;
    if (key === "Enter" && cmd) return "run";
    if (key === "F10") return "step";
    if ((key === "s" || key === "S") && cmd) return "save";
    return null;
  }

  // The bottom panel's tab: a start shows MESSAGES; problems show PROBLEMS when idle and the owner has not picked
  // a tab since the last start. Returns a new state.
  function panelTab(s0, ev) {
    var s = { tab: s0.tab, picked: s0.picked };
    if (ev.e === "pick") { s.tab = ev.tab; s.picked = true; }
    else if (ev.e === "started") { s.tab = "messages"; s.picked = false; }
    else if (ev.e === "problems" && ev.n > 0 && !ev.running && !s.picked) s.tab = "problems";
    return s;
  }

  // The camera streams only while it can be seen.
  function cameraWanted(wide, hiddenByUser, pageHidden) { return wide && !hiddenByUser && !pageHidden; }

```

and extend the export list's last line:

```js
    runBarStart: runBarStart, runBar: runBar, canRun: canRun, locked: locked, endText: endText, NACK_TEXT: NACK_TEXT,
    shortcut: shortcut, panelTab: panelTab, cameraWanted: cameraWanted
```

- [ ] **Step 4: Run the tests**

Run: `node --test firmware/esp32/web/test/logo.test.js 2>&1 | grep -E "^ℹ (pass|fail)"`
Expected: `ℹ pass 52`, `ℹ fail 0`.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/web/logo.js firmware/esp32/web/test/logo.test.js
git commit -m "feat(logo): shortcut, panelTab and cameraWanted for the laptop layout (Tank Logo LLD §23.3)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: The laptop layout — panes, bottom panel, status bar, shortcuts

**Files:**
- Modify: `firmware/esp32/web/logo.html`
- Create: `firmware/esp32/web/test/browser_check_wide.js`

**Interfaces:**
- Consumes: `L.shortcut`, `L.panelTab` (Task 1).
- Produces (used by Task 3):
  - `wide` (bool, set by `applyLayout`);
  - `applyLayout()`;
  - the element `#pslot` and the tab buttons `#tProb` / `#tMsg`;
  - `data-kbd` hints on `#run` / `#step` / `#stop`.

- [ ] **Step 1: Write the browser check** `firmware/esp32/web/test/browser_check_wide.js`:

```js
// Revision N6 browser check (Tank Logo LLD §23.6, Playwright run_code): the laptop layout on the stand-in tank
// firmware/esp32/web/test/fake_tank.py on :8765. Each line of `out` says what it should be.
async (page) => {
  await page.addInitScript(() => {  // a stand-in WebSocket: records what the page sends, answers hello
    try { localStorage.clear(); } catch (e) { /* a fresh browser */ }
    window.__sent = [];
    class FakeWS {
      constructor(url) { this.url = url; this.readyState = 0; window.__ws = this;
        setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); }, 20); }
      send(s) { const m = JSON.parse(s); window.__sent.push(m);
        if (m.t === "hello") this.emit({ t: "welcome", you: "phone-1", tank: { fw: { uno: "1.0" } } });
        if (m.t === "logo_run") this.emit({ t: "ack", id: m.id }); }
      emit(o) { setTimeout(() => this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 5); }
      close() { this.readyState = 3; this.onclose && this.onclose(); }
    }
    window.WebSocket = FakeWS;
    window.__fakeEmit = (o) => window.__ws.emit(o);
  });
  const q = (f, a) => page.evaluate(f, a), st = () => q(() => document.getElementById("status").textContent);
  const sent = (t) => q((t) => window.__sent.filter((m) => m.t === t).length, t);
  // the page does not scroll, and STOP / RUN / STEP are inside the window
  const fits = () => q(() => {
    const inside = (id) => { const r = document.getElementById(id).getBoundingClientRect();
      return r.top >= 0 && r.bottom <= innerHeight && r.left >= 0 && r.right <= innerWidth && r.height > 0; };
    return document.scrollingElement.scrollHeight <= innerHeight && document.scrollingElement.scrollWidth <= innerWidth &&
      inside("stop") && inside("run") && inside("step");
  });
  const out = {};
  for (const [w, h] of [[1280, 800], [1024, 700]]) {
    await page.setViewportSize({ width: w, height: h });
    await page.goto("http://127.0.0.1:8765/logo"); await page.waitForTimeout(1500);
    const long = Array.from({ length: 300 }, (_, i) => "FORWARD " + (i + 1)).join("\n") + "\n";
    await page.locator("#src").fill(long); await page.waitForTimeout(1200);
    await q(() => { for (let i = 0; i < 200; i++) window.__fakeEmit({ t: "ev", kind: "logo_note", line: 1, msg: "note " + i }); });
    await page.waitForTimeout(300);
    out["fits" + w] = [await fits(),                                                         // true
      await q(() => { const s = document.getElementById("src"); return s.scrollHeight > s.clientHeight; })];  // true: the editor scrolls itself
  }
  // 1024 x 700 from here: a mistake shows PROBLEMS by itself, under the editor
  await page.locator("#src").fill("FORWAD 50\n"); await page.waitForTimeout(1200);
  out.problems = [await q(() => document.getElementById("cMsg").className),                 // card t-problems
    await q(() => document.getElementById("tProb").textContent),                            // PROBLEMS 1
    await q(() => document.getElementById("diags").parentNode.id)];                         // pslot
  await page.locator("#src").fill("REPEAT 4 [ FORWARD 50 RIGHT 90 ]\n"); await page.waitForTimeout(1200);
  // Ctrl+Enter runs (from inside the editor), a start shows MESSAGES, the status bar has the line and round
  await page.locator("#src").focus(); await page.keyboard.press("Control+Enter"); await page.waitForTimeout(100);
  await q(() => { window.__fakeEmit({ t: "ev", kind: "logo_started" }); window.__fakeEmit({ t: "ev", kind: "logo_line", line: 1, round: 3 }); });
  await page.waitForTimeout(100);
  out.run = [await sent("logo_run"), await q(() => document.getElementById("cMsg").className), await st(),   // 1, t-messages, line 1 · round 3
    await q(() => document.getElementById("help").classList.contains("hide"))];                              // true while running
  // Esc in the editor stops
  await page.keyboard.press("Escape"); await page.waitForTimeout(50);
  out.esc = [await sent("stop"), await sent("logo_stop")];                                 // 1, 1
  await q(() => window.__fakeEmit({ t: "ev", kind: "logo_ended", reason: 7040, line: 1, msg: "Stopped by you." }));
  await page.waitForTimeout(1300);
  // F10 starts in step mode; Ctrl+S saves (a new program asks for a name)
  await page.keyboard.press("F10"); await page.waitForTimeout(100);
  out.f10 = await q(() => window.__sent.filter((m) => m.t === "logo_run").map((m) => m.step).join());      // false,true
  await q(() => window.__fakeEmit({ t: "ev", kind: "logo_ended", reason: 7040, line: 1, msg: "Stopped by you." }));
  await page.waitForTimeout(1300);
  await page.keyboard.press("Control+s"); await page.waitForTimeout(100);
  out.save = await q(() => document.getElementById("nameS").classList.contains("on"));     // true
  await page.keyboard.press("Escape"); await page.waitForTimeout(50);
  out.escSheet = [await q(() => document.getElementById("nameS").classList.contains("on")), await sent("stop")];  // false, 2
  // narrow again: the phone layout, the problems and the help back under the editor
  await page.setViewportSize({ width: 390, height: 844 }); await page.waitForTimeout(300);
  out.phone = [await q(() => document.getElementById("diags").parentNode.id),               // cEdit
    await q(() => document.getElementById("help").parentNode.id),                           // cEdit
    await q(() => document.scrollingElement.scrollHeight > innerHeight)];                   // true: the phone page scrolls
  return out;
}
```

- [ ] **Step 2: Run it to see it fail**

```bash
cd firmware/esp32 && python3 scripts/pack_web.py && cd ../..
python3 firmware/esp32/web/test/fake_tank.py 8765 &      # stop it at the end of the task
mkdir -p .playwright-mcp && cp firmware/esp32/web/test/browser_check_wide.js .playwright-mcp/n6_wide.js
```
Then run Playwright `browser_run_code` with `filename: .playwright-mcp/n6_wide.js` (the tool reads files only under `.playwright-mcp/`).
Expected: `fits1280` / `fits1024` `[false, …]` (the page scrolls). `out.problems` throws or reads `null` (no `#tProb`), so the run fails.

- [ ] **Step 3: The CSS.** In `logo.html`, after `.hide { display: none !important; }` add:

```css
/* Revision N6 (Tank Logo LLD §23): parts only the laptop layout shows */
#ptabs { display: none; }
#ptabs button.on { background: rgba(51, 255, 102, .12); color: var(--active); border-color: var(--ui); }
```

and replace the whole `@media (min-width: 900px) { … }` block with:

```css
@media (min-width: 900px) {  /* §23: an editor that fills the window; only the panes scroll */
  body { padding-bottom: 0; height: 100vh; height: 100dvh; overflow: hidden; display: grid; grid-template-rows: auto minmax(0, 1fr); }
  header { position: static; }
  header .inner { display: flex; align-items: center; gap: 16px; } .top { flex: 1; } .top h1 { text-align: left; }
  .chips { display: flex; margin-top: 0; }
  main { min-height: 0; grid-template-columns: 220px minmax(0, 1fr) minmax(320px, 34%); grid-template-rows: minmax(0, 1fr) 12em auto;
    grid-template-areas: "prog edit run" "prog msg run" "bar bar bar"; }
  #cProg, #cEdit, #cMsg, #cPath { display: flex; flex-direction: column; min-height: 0; }
  #cProg { grid-area: prog; } #plist { display: block; flex: 1; min-height: 0; overflow: auto; }
  .progbar { order: 2; flex-wrap: wrap; margin-top: 8px; } .progbar span { display: inline; } .progbar button { flex: 1; }
  .progbar select, #tabs { display: none; }
  #cEdit { grid-area: edit; } #cEdit h2 { order: -2; }
  #pad { order: -1; flex: none; flex-wrap: nowrap; overflow-x: auto; margin: 0 0 8px; }
  #pad button { flex: none; white-space: nowrap; }
  .ed { flex: 1; height: auto; min-height: 0; }
  #cMsg { grid-area: msg; } #cMsg > h2 { display: none; } #ptabs { display: flex; gap: 6px; margin-bottom: 6px; flex: none; }
  #pslot, .msgs { flex: 1; min-height: 0; overflow: auto; } #pslot #diags { margin-top: 0; }
  #cMsg.t-problems .msgs, #cMsg.t-messages #pslot { display: none; }
  #log { max-height: none; } #mini { display: none; }
  #cPath { grid-area: run; }
  .pathw { flex: 1; height: auto; min-height: 120px; }
  #bar { grid-area: bar; position: static; border: 1px solid var(--edge-c); padding: 6px 8px; }
  #bar .help { flex: 1; min-width: 0; margin: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  [data-kbd]::after { content: " " attr(data-kbd); font-size: 10px; opacity: .75; font-weight: 400; }
}
```
(`#pad button { flex: none; white-space: nowrap; }` is from the draft: without it the last button, MORE WORDS ▾, squeezes and wraps onto three lines, making the row 70 px tall.)

- [ ] **Step 4: The markup.** In `#cMsg`, under its `<h2>`:

```html
  <div id="ptabs"><button type="button" id="tProb" data-t="problems">PROBLEMS</button><button type="button" id="tMsg" data-t="messages">MESSAGES</button></div>
  <div id="pslot"></div>
```

- [ ] **Step 5: The script.**
  - After the line `var driven = [], pose = null, …  // a program being opened` add:
    ```js
      var wideQ = window.matchMedia("(min-width: 900px)"), wide = false, panel = { tab: "messages", picked: false };  // §23
    ```
  - In `checkTick`, after `showDiags(); render(); draw(); act(); showBar();` add:
    ```js
          panelEv({ e: "problems", n: diags.length, running: L.locked(bar) });
    ```
  - First line of `showDiags()`: `    showPanel();`
  - In `showBar()`, the running line becomes:
    ```js
        else if (s.st === "running") { t = "● RUNNING · line " + (s.line || "-") + (s.round > 0 ? " · round " + s.round : "") + " · " + clock(Date.now() - startedAt); cls = "s-warn"; }
    ```
    and after `$("psave").disabled = … = L.locked(s);` add:
    ```js
        $("help").classList.toggle("hide", wide && L.locked(s));  // the status bar shows the running line instead
    ```
  - In `onEvent`:
    ```js
          case "logo_started": onBar({ e: "started" }); if (mine) panelEv({ e: "started" }); break;
    ```
  - After `window.addEventListener("resize", draw);` add:
    ```js
      if (window.ResizeObserver) new ResizeObserver(function () { draw(); }).observe($("path").parentNode);
    ```
  - Before `  showPad();` (the start-up lines) add:
    ```js
      // ---------- revision N6: the laptop layout (§23) ----------
      // Layout: two elements change place at 900 px (the word row and the panes move by CSS alone).
      function applyLayout() {
        wide = wideQ.matches;
        if (wide) { $("pslot").appendChild($("diags")); $("bar").insertBefore($("help"), $("step")); }
        else { $("cEdit").insertBefore($("diags"), $("pad")); $("cEdit").appendChild($("help")); }
        showBar(); draw();
      }
      if (wideQ.addEventListener) wideQ.addEventListener("change", applyLayout); else wideQ.addListener(applyLayout);

      // BottomPanel: PROBLEMS | MESSAGES under the editor
      function panelEv(ev) { panel = L.panelTab(panel, ev); showPanel(); }
      function showPanel() {
        $("tProb").textContent = "PROBLEMS" + (diags.length ? " " + diags.length : "");
        $("tProb").classList.toggle("on", panel.tab === "problems"); $("tMsg").classList.toggle("on", panel.tab !== "problems");
        $("cMsg").classList.toggle("t-problems", panel.tab === "problems"); $("cMsg").classList.toggle("t-messages", panel.tab !== "problems");
      }
      $("ptabs").addEventListener("click", function (e) { var t = e.target.dataset && e.target.dataset.t; if (t) panelEv({ e: "pick", tab: t }); });

      // Shortcuts: each is its button; Esc is STOP from anywhere, even in a dialog (which it also closes)
      function sheetOpen() { return $("nameS").classList.contains("on") || $("delS").classList.contains("on"); }
      window.addEventListener("keydown", function (e) {
        var a = L.shortcut(e.key, e.ctrlKey, e.metaKey, e.altKey, e.shiftKey, e.repeat);
        if (!a) return;
        e.preventDefault();
        if (a === "stop") { onBar({ e: "stop" }); $("nameS").classList.remove("on"); $("delS").classList.remove("on"); return; }
        if (sheetOpen()) return;
        var b = $(a === "save" ? "psave" : a);
        if (!b.disabled) b.click();
      }, true);  // capture: before the editor's own keys
      var mac = /Mac|iPhone|iPad/.test(navigator.platform || navigator.userAgent || "");
      $("run").dataset.kbd = mac ? "⌘↵" : "Ctrl↵"; $("step").dataset.kbd = "F10"; $("stop").dataset.kbd = "Esc";
      $("psave").title = "Save on the tank (" + (mac ? "⌘S" : "Ctrl+S") + ")";

    ```
  - After the last start-up line `  tab("prog");` add `  applyLayout(); showPanel();`

- [ ] **Step 6: Pack, run both browser checks and the Node tests**

```bash
cd firmware/esp32 && python3 scripts/pack_web.py && cd ../..
cp firmware/esp32/web/test/browser_check_wide.js .playwright-mcp/n6_wide.js
cp firmware/esp32/web/test/browser_check.js .playwright-mcp/n4_phone.js
node --test firmware/esp32/web/test/logo.test.js 2>&1 | grep -E "^ℹ (pass|fail)"
```
Restart `fake_tank.py` (it serves the packed page), then run `browser_run_code` with `n6_wide.js` and `n4_phone.js`.
Expected:
- `n6_wide`:
  - `fits1280 [true, true]`, `fits1024 [true, true]`;
  - `problems ["card t-problems", "PROBLEMS 1", "pslot"]`;
  - `run [1, "card t-messages", "● RUNNING · line 1 · round 3 · 0:00", true]`;
  - `esc [1, 1]`, `f10 "false,true"`, `save true`, `escSheet [false, 2]`;
  - `phone ["cEdit", "cEdit", true]`.
- `n4_phone`: the N4 values (`open "● READY"` … `cutShort "path cut short …"`). The only new text is `running "● RUNNING · line 2 · round 2 · 0:00"`.
- Node: 52 pass.
- The packed `/logo` is under 24 KB gzipped (`pack_web.py` fails otherwise).

- [ ] **Step 7: Look at it** at 1280 × 800 and 1024 × 700 (a Playwright screenshot):
  - one slim word row above the editor;
  - PROBLEMS / MESSAGES under it;
  - the help line in the status bar;
  - STOP Esc, STEP F10 and ▶ RUN ⌘↵ hints.

- [ ] **Step 8: Commit**

```bash
git add firmware/esp32/web/logo.html firmware/esp32/web/test/browser_check_wide.js
git commit -m "feat(logo): the laptop layout - fixed panes, PROBLEMS / MESSAGES panel, status bar, shortcuts (Tank Logo LLD §23.2-§23.4)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: The camera pane

**Files:**
- Modify: `firmware/esp32/web/logo.html`, `firmware/esp32/web/test/browser_check_wide.js`

**Interfaces:**
- Consumes: `L.cameraWanted` (Task 1); `wide`, `applyLayout` (Task 2); `recall` / `remember` (§21); the `tf` message's `dist_cm`.

- [ ] **Step 1: Extend the browser check.**
  - Before `  const q = (f, a) =>` add a stand-in camera on `:81`:
    ```js
      const PNG = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==", "base64");
      let streams = 0;  // a stand-in camera on :81: one small picture per request
      await page.route(/:81\/stream/, (r) => { streams++; r.fulfill({ status: 200, contentType: "image/png", body: PNG }); });
    ```
  - Before `  // narrow again:` add:
    ```js
      // the camera: wanted on the laptop; HIDE drops the stream and the path grows; SHOW brings it back
      const pathH = () => q(() => document.getElementById("path").getBoundingClientRect().height);
      out.cam = [await q(() => /:81\/stream/.test(document.getElementById("camImg").src)), await pathH(), streams > 0];  // true, h, true
      await page.click("#camHide"); await page.waitForTimeout(300);
      out.camHidden = [await q(() => document.getElementById("camImg").hasAttribute("src")), await pathH(),     // false, larger
        await q(() => localStorage.getItem("logo.cam"))];                                                       // off
      const before = streams;
      await page.click("#camShow"); await page.waitForTimeout(300);
      out.camBack = [await q(() => /:81\/stream/.test(document.getElementById("camImg").src)), streams > before];  // true, true
      await q(() => window.__fakeEmit({ t: "tf", dist_cm: 64 })); await page.waitForTimeout(50);
      out.sonar = await q(() => document.getElementById("camDist").textContent);                                // ▸ 64 cm
    ```
  - In `out.phone`, add first: `await q(() => document.getElementById("camImg").hasAttribute("src")),        // false`

- [ ] **Step 2: Run it to see it fail** (copy to `.playwright-mcp/n6_wide.js`, run).
Expected: it throws at `#camImg` (no such element).

- [ ] **Step 3: The CSS.**
  - In the N6 block after `.hide`, change `#ptabs { display: none; }` to `#ptabs, .cam, #camShow { display: none; }` and add:
    ```css
    .cam { position: relative; aspect-ratio: 4 / 3; background: #000; border: 1px solid var(--line); margin-bottom: 8px; flex: none; }
    .cam img { position: absolute; inset: 0; width: 100%; height: 100%; object-fit: contain; display: block; }  /* the box, not the picture, sets the size */
    #camNote { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; text-align: center; padding: 12px; color: var(--dim); }
    #camDist { position: absolute; left: 8px; bottom: 6px; color: var(--ui); background: rgba(0, 0, 0, .65); padding: 1px 6px; }
    #camHide { position: absolute; right: 6px; top: 6px; padding: 3px 8px; background: rgba(0, 0, 0, .65); }
    #camShow { padding: 0 6px; font-size: 11px; }
    ```
  - In the `@media (min-width: 900px)` block, `#cPath { grid-area: run; }` becomes:
    ```css
      #cPath { grid-area: run; } .cam { display: block; } body.camoff .cam { display: none; } body.camoff #camShow { display: inline-block; }
    ```
  (`position: absolute` on the picture is from the draft: otherwise the picture's own size sets the box's height, and a square test picture made the box square.)

- [ ] **Step 4: The markup.** `#cPath`'s heading and the pane under it:

```html
  <h2>[ 02 // PATH ]<span><button id="camShow" type="button">SHOW CAMERA</button> <span id="pgrid" class="badge">5 cm GRID</span></span></h2>
  <div class="cam" id="cam"><img id="camImg" alt=""><span id="camNote">NO CAMERA</span><b id="camDist">&#9656; -- cm</b><button id="camHide" type="button">HIDE</button></div>
```

- [ ] **Step 5: The script.**
  - After the `var wideQ = …` line add:
    ```js
      var camOff = recall("logo.cam") === "off", camOn = false, camTimer = 0;
    ```
  - The `tf` case in `onMessage`:
    ```js
          case "tf":
            $("sDist").textContent = m.dist_cm === null || m.dist_cm === undefined ? "--" : m.dist_cm + " cm";
            $("camDist").textContent = "▸ " + $("sDist").textContent;
            break;
    ```
  - In the `visibilitychange` handler, after its `if … else …` line: `    camUpdate();`
  - In `applyLayout`, `showBar(); draw();` becomes `camUpdate(); showBar(); draw();`
  - The `ResizeObserver` line's comment: `// HIDE / SHOW camera`
  - After the Shortcuts part (before `  showPad();`) add:
    ```js
      // CameraPane: the stream only while it can be seen (port 81, 2 viewers at most - the page cannot tell a
      // refusal from no camera, so one note for both)
      function camUpdate() {
        document.body.classList.toggle("camoff", camOff);
        var want = L.cameraWanted(wide, camOff, document.hidden);
        if (want && !camOn) camStart(); else if (!want && camOn) camStop();
      }
      function camNote(t) { $("camNote").textContent = t; $("camNote").classList.toggle("hide", !t); }
      function camStop() {
        camOn = false; clearTimeout(camTimer); camTimer = 0;
        var i = $("camImg"); i.onerror = null; i.removeAttribute("src"); camNote("NO CAMERA");  // closes the MJPEG connection
      }
      function camStart() {
        camOn = true; clearTimeout(camTimer); camTimer = 0;
        var i = $("camImg");
        camNote("");  // MJPEG streams do not reliably fire onload (as on the Drive page)
        i.onerror = function () {
          i.onerror = null; i.removeAttribute("src");
          camNote("NO CAMERA (2 viewers at most) - trying again");
          camTimer = setTimeout(function () { if (camOn) camStart(); }, 3000);
        };
        i.src = "http://" + location.hostname + ":81/stream?t=" + Date.now();
      }
      $("camHide").addEventListener("click", function () { camOff = true; remember("logo.cam", "off"); camUpdate(); });
      $("camShow").addEventListener("click", function () { camOff = false; remember("logo.cam", ""); camUpdate(); });

    ```

- [ ] **Step 6: Pack and run all the checks** (as Task 2 Step 6).
Expected:
- `n6_wide`: as Task 2, plus:
  - `cam [true, ~190 (1024 × 700), true]`;
  - `camHidden [false, ~438, "off"]`;
  - `camBack [true, true]`;
  - `sonar "▸ 64 cm"`;
  - `phone [false, "cEdit", "cEdit", true]`.
- `n4_phone`: unchanged.
- Node: 52.
- `/logo` about 22.7 KB gzipped.
- The console shows only the expected errors: `:81` refused when there is no stand-in camera (retried every 3 s), and the N4 check's deliberate 503s.

- [ ] **Step 7: Look at it** at 1280 × 800 and 1024 × 700: a 4:3 box above the path, HIDE at its top right, the sonar at its lower left, SHOW CAMERA in the path heading once hidden.

- [ ] **Step 8: Commit**

```bash
git add firmware/esp32/web/logo.html firmware/esp32/web/test/browser_check_wide.js
git commit -m "feat(logo): the camera above the path on the laptop, with the sonar and HIDE (Tank Logo LLD §23.5)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: On the tank (owner, laptop)

**Files:** none (results go to Task 5).

- [ ] **Step 1: Build and flash the bridge over WiFi** (the pages are packed into the firmware; never print the OTA password). Copy `include/secrets.h` into the worktree for the build, and delete it afterwards.

```bash
cd firmware/esp32
~/.platformio/penv/bin/pio run -e bridge 2>&1 | tail -3
export CQ_OTA_PASSWORD="$(sed -n 's/.*CQ_OTA_PASSWORD[^"]*"\([^"]*\)".*/\1/p' include/secrets.h)"
~/.platformio/penv/bin/pio run -e bridge_ota -t upload --upload-port 192.168.31.167 2>&1 | grep -vF -- "$CQ_OTA_PASSWORD" | tail -3
unset CQ_OTA_PASSWORD
```
Expected: `SUCCESS`; the bridge back on WiFi within ~15 s.

- [ ] **Step 2: The page on the laptop (tank still):**
  - open `http://192.168.31.167/logo` (a reload, with `?v=` if cached);
  - the camera shows with the sonar distance;
  - HIDE / SHOW work;
  - the page doesn't scroll;
  - the phone's `/logo` looks as before.

- [ ] **Step 3: Running by keyboard.** Ask "stand or floor?" first and say what moves: the 50 cm square on the floor.
  - Ctrl/⌘ + Enter runs it while the camera and the path follow.
  - Then `FORWARD 200`, and Esc part-way: it stops (7040).

- [ ] **Step 4: Two viewers.** With the phone's Drive page open, the laptop's camera still streams. With a third (the Settings camera tab), the Logo pane says "NO CAMERA (2 viewers at most)" and comes back when one closes.

---

### Task 5: Docs as built

**Files:**
- Modify: `docs/lld/tank-logo-lld.md` (add §23.8 As built), `docs/design-notes.md` (§148), `CLAUDE.md` (the plans list and the N line)

- [ ] **Step 1: Tank Logo LLD §23.8 As built:**
  - files and numbers: Node 52, `/logo` gzipped size, both browser checks;
  - the rulings below;
  - the tank results from Task 4.
- [ ] **Step 2: design-notes §148:** Plan N6 built and tank-checked, with what the owner saw.
- [ ] **Step 3: `CLAUDE.md`:**
  - add `2026-10-24-plan-n6-logo-editor.md` to the plans list;
  - one line in the order section.
- [ ] **Step 4: Commit and push** (`docs(logo): Plan N6 as built …`). Merge to `main` only after the owner says so.

## Rulings made while drafting (against LLD §23)
1. **Only two elements move** (`#diags` into the bottom panel, `#help` into the status bar), not five.
   - The word row moves above the editor by CSS `order`;
   - the messages and the status stay in their parents, already in place.
   - Cost if wrong: none for the phone (checked by the N4 check).
2. **`panelTab`'s problems event carries `running`** (`{e: "problems", n, running}`), so the function stays pure.
3. **The camera can't tell "max 2 viewers" from "no camera":** the stream is on another port, so the `<img>` gets no reason. There is one note for both, "NO CAMERA (2 viewers at most) - trying again", instead of LLD §23.5's two.
4. **The running status shows the round** (`· round r`) on the phone too: the same text, one more field.
5. **The camera box sets its own size** (the picture is absolutely placed), and the word buttons don't wrap. Both were found by looking at the draft's screenshots.

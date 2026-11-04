# Plan Top Bar - one top bar on every page Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every tank page (`/`, `/logo`, `/settings`, `/help`, `/ai`) has the same 🎨 look, ☰ menu and STOP at the top right, one shared menu, no "< DRIVE" link.

**Architecture:** A shared piece - `web/bar.html` (the three buttons and the menu), `web/bar.css`, `web/bar.js` (`window.cqBar`) - is put into each page at `<!--CQ:BAR-->` by `pack_web.py`, as the look is (R28). Each page keeps its own STOP handler on `#stop`; Help stops through a socket it opens only for the press.

**Tech Stack:** plain HTML/CSS/ES5 pages packed by Python, node `--test`, Python unittest, Playwright MCP `run_code` checks on `web/test/fake_tank.py` :8765 (each check in a fresh tab).

**Spec:** `docs/lld/esp32-bridge-lld.md` R29, `docs/design-notes.md` §208 (on R28 / §206-§207).

## Global Constraints
- Order 🎨, ☰, STOP; 44 px high; top right; one row at 390 px on every page; both looks; STOP solid red.
- Menu: DRIVE `/`, LOGO `/logo`, AI `/ai`, SETTINGS `/settings`, HELP `/help` in that order, icon + name + short line (the drive sheet's words: "drive and look around", "write and run programs", "find, explore, teach it a word", "WiFi · tank · tune · log", "how-to and facts"); the current page `aria-current="page"`.
- Ids: `#lookBtn`, `#menuBtn`, `#stop`, `#cqMenu`, `#cqDim`; window event `cqmenu` when the menu opens.
- STOP behaviour unchanged on Drive (latch, hide-STOP §201), `/logo` and `/ai` (program stop). Settings: `stop` + `logo_stop`. Help: `cqBar.stopOnce` - hello (client `"help"`), `stop`, `logo_stop`, done on the first ack, 2 s at most, socket always closed; messages "Stopped" / "Couldn't reach the tank - use another STOP".
- Never weaken safety: a page's STOP may only gain, never lose, a path to the tank.
- Pages stay ES5; no new dependencies; the bar's colours come from the R28 variables (no new fixed colours); budgets as now (`/logo` 28 KB, `/` 25, `/settings` 36, `/help` 16, `/ai` 24).
- Commits: Conventional Commits, ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus
1. Drive page: the menu opened while another drive sheet (sounds, more, camera) is open - only one shows; STOP still above the dim layer and still works while the menu is open.
2. Help's STOP when the bridge already has 4 clients (the socket is refused) - says it couldn't reach the tank within 2 s, never "Stopped".
3. A double tap on Help's STOP - two short sockets, both closed; no socket left open.
4. The menu on a short laptop window (1280 × 600) and a phone in landscape - all five entries reachable.
5. Keyboard: Esc closes the menu on every page, and on `/logo` Esc still stops the program as before.

---

### Task 1: The shared bar, and `/ai` on it

**Files:** Create `web/bar.html`, `web/bar.css`, `web/bar.js`, `web/test/bar.test.js`, `scripts/test_bar_pack.py`, `web/test/bar_check.js`. Modify `scripts/pack_web.py`, `web/ai.html`.

**Interfaces:**
- Produces: `window.cqBar = { PAGES, here(path) -> {path,name,line,icon}|null, open(), close(), isOpen() -> bool, stopOnce(url, done, opts?) }` (`opts` lets the node test pass a socket class and a timer); `bar.js` ends `if (typeof module !== "undefined") module.exports = cqBar;` and touches `document` only when it exists.
- Produces: `pack_web.with_bar(text, project, page) -> str`, `BAR_MARKER = "<!--CQ:BAR-->"`; until Task 6 a page without the marker is left as it is.

- [ ] **Step 1: Failing tests.** `bar.test.js`: `here("/").name === "DRIVE"`, `here("/logo").name === "LOGO"`, `here("/nope") === null`, `PAGES.map(p => p.path).join() === "/,/logo,/ai,/settings,/help"`; `stopOnce` with a fake socket that answers `ack` → `done(true)`, sent types `hello,stop,logo_stop`, closed; one that errors → `done(false)`, closed; one that stays silent → `done(false)` after the 2000 ms fake timer, closed; `done` called once in every case. `test_bar_pack.py`: `with_bar` on a page with the marker contains `id="lookBtn"`, `id="menuBtn"`, `id="stop"`, `id="cqMenu"`, `window.cqBar =` once and no `</` inside its script; `assemble_page(PROJECT, "/ai")` contains `id="lookBtn"` once and no `id="themeBtn"`.
- [ ] **Step 2:** Run `node --test web/test/bar.test.js`, `python3 -m unittest discover -s scripts -p 'test_*.py'`. Expected: FAIL.
- [ ] **Step 3:** Write the shared piece (markup with `<!--icon:palette-->`, `<!--icon:menu-->`, `<!--icon:hand-->`; menu icons as the drive sheet's: code, sparkles, settings, circle-help, plus `joystick` for DRIVE). `with_bar` runs before the icon expansion. In `ai.html`: `<!--CQ:BAR-->` in place of `#themeBtn` and `#stop` in the header; remove "< DRIVE"; its existing STOP handler stays bound to `#stop`.
- [ ] **Step 4:** `bar_check.js` for `/ai` (390 and 1440 px): the three in one row, order 🎨 ☰ STOP, equal heights 44 px, right edge within 16 px of the window; the title one line; the menu opens with five entries in order, AI marked; Esc and the dim layer close it; 🎨 flips. Run it, `ai_check.js` (its `#themeBtn` becomes `#lookBtn`), `look_check.js` (its compare against 544a09d skips `header` on both sides - the bar changed on purpose - and its Sunny parts use `#lookBtn`). Expected: PASS.
- [ ] **Step 5:** Commit `feat(web): one shared top bar (bar.html, bar.css, bar.js, <!--CQ:BAR-->); /ai on it (bridge LLD R29)`.

### Task 2: `/settings`

**Files:** Modify `web/settings.html`, `web/test/bar_check.js`, `web/test/look_check.js`.
- [ ] **Step 1:** `bar_check.js` for `/settings`: the row, the menu (SETTINGS marked), and STOP sends `stop` then `logo_stop` (a stand-in socket records them). Run: FAIL.
- [ ] **Step 2:** The marker in its header in place of `#themeBtn`; remove "< DRIVE"; `#stop` click sends `{t:"stop",id}` and `{t:"logo_stop",id}` on its socket.
- [ ] **Step 3:** Run `bar_check.js`, `camera_centre_check.js`, `camera_tuning_check.js`, `look_check.js`. Commit `feat(web): /settings on the shared top bar, with STOP (bridge LLD R29)`.

### Task 3: `/logo`

**Files:** Modify `web/logo.html`, the checks.
- [ ] **Step 1:** `bar_check.js` for `/logo`: the row at 390 and 1440 px, the title `[ TANK LOGO ]` on one line, the menu (LOGO marked), no `#mbtn`, no `nav#menu`, no `#miLook`; Esc with the menu open closes it, Esc again stops (sends `stop` + `logo_stop`). Run: FAIL.
- [ ] **Step 2:** The marker in place of `#mbtn` and its `#stop`; remove `nav#menu`, its toggle code and "< DRIVE"; its `#stop` handlers stay. The laptop layout's "menu button at the far right" rule (§149) becomes "the three at the far right".
- [ ] **Step 3:** Run `bar_check.js`, `browser_check.js`, `browser_check_wide.js` (its `menuRight` becomes: `#stop` is the right-most), `look_check.js`, `logo.test.js`. Commit `feat(web): /logo on the shared top bar (bridge LLD R29)`.

### Task 4: Drive

**Files:** Modify `web/basic_drive.html`, the checks.
- [ ] **Step 1:** `bar_check.js` for `/` at 844 × 390: the row ends [🔈] [STICK] [🎨] [☰] [■ STOP] at the top right; the menu (DRIVE marked) over the dim layer with STOP still clickable above it; opening the menu closes an open sounds sheet, and opening the sounds sheet closes the menu; no `#menuSheet`, no `#miLook`. Run: FAIL.
- [ ] **Step 2:** The marker at the end of the right-hand chip row in place of the MENU chip; remove `#menuSheet`, the `menuBtn` entry of `sheets`, `closeMenu`, the LOOK item; on `cqmenu` the page closes its own sheets; opening one of its sheets calls `cqBar.close()`. `#stop` keeps its id, its handlers, its hide-STOP and latch code untouched.
- [ ] **Step 3:** Run `bar_check.js`, `hide_stop_check.js`, `look_check.js`. Commit `feat(web): the drive page on the shared top bar (bridge LLD R29)`.

### Task 5: `/help`

**Files:** Modify `web/help.html`, the checks.
- [ ] **Step 1:** `bar_check.js` for `/help`: a top bar (title `HELP` + the three), the menu (HELP marked); STOP opens one socket, sends `hello`, `stop`, `logo_stop`, shows "Stopped" on the ack and closes it; with a socket that never answers it shows "Couldn't reach the tank - use another STOP" after 2 s and closes it; a double tap leaves no socket open. Run: FAIL.
- [ ] **Step 2:** A top bar with the marker; `#stop` click → `cqBar.stopOnce("ws://" + location.host + "/ws", show)`; the message in a small line under the bar.
- [ ] **Step 3:** Run `bar_check.js`, `look_check.js`. Commit `feat(web): /help on the shared top bar, STOP through a short connection (bridge LLD R29)`.

### Task 6: Every page together, on the tank

**Files:** Modify `scripts/pack_web.py` (the marker now required), `scripts/test_bar_pack.py`, `web/test/bar_check.js`, `docs/design-notes.md` (§208 as built), `docs/lld/esp32-bridge-lld.md` (R29 status), `CLAUDE.md`.
- [ ] **Step 1:** Test: a page without `<!--CQ:BAR-->` fails the build naming it. Run: FAIL; make the marker required; PASS.
- [ ] **Step 2:** `bar_check.js` across pages: the three at the same place and size on `/logo`, `/settings`, `/help`, `/ai` (within 1 px); the menu reaches every page; 1280 × 600 and phone landscape show all five entries; no sideways scroll; all the older checks above pass.
- [ ] **Step 3:** Screenshots of every page in both looks, phone and laptop.
- [ ] **Step 4:** OTA flash; `curl --compressed` every page shows `window.cqBar =`. Owner on the iPhone (nothing moves): the same three on every page, the menu, 🎨; STOP on Settings and Help with the tank on the stand and nothing running ("Stopped").
- [ ] **Step 5:** Docs as built; commit `docs: Plan Top Bar as built (design-notes §208, bridge LLD R29)`; push.

# Plan Sunny - two looks on every page Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drive (`/`), `/logo`, `/settings`, `/help` and `/ai` each show either the Phosphor Cockpit or Sunny Play, one remembered choice per device, with canvas drawings taking their colours from CSS.

**Architecture:** Each page keeps its cockpit values in its own `:root`. A shared `web/theme.css` holds only the Sunny values under `:root[data-theme="play"]`, and a shared `web/theme.js` (`window.cqTheme`) picks, flips and reads colours. `pack_web.py` puts both into every page's `<head>` at the `<!--CQ:THEME-->` marker. Canvas code calls `cqTheme.colour("--name")` and redraws on the `cqtheme` event.

**Tech Stack:** plain HTML/CSS/ES5 pages packed by Python (`firmware/esp32/scripts/pack_web.py`), node `--test`, Python unittest, Playwright MCP `run_code` checks against `web/test/fake_tank.py` on :8765.

**Spec:** `docs/lld/esp32-bridge-lld.md` R28 and `docs/design-notes.md` §206-§207 (the Stitch design system "Sunny Play", project 9914699709114202518, `assets/9779248838636593021`).

## Global Constraints
- Cockpit unchanged: every element's computed colours, fonts and shadows on every page equal the baseline taken before Task 2 (`web/test/look_baseline.json`).
- Layout, words, order and behaviour identical in both looks; a theme never adds, removes or moves anything (the switch buttons are the one addition).
- STOP solid red in both looks: cockpit `#ff3b30`, Sunny `#e8382e`.
- One key: `localStorage` `cq_theme`, values `"play"` / `"cockpit"`; default `"play"` when `matchMedia("(pointer: coarse)").matches`, else `"cockpit"`; storage errors count as nothing saved.
- Sunny palette (design-notes §206): page `#fff8ec`, cards `#ffffff`, headings / main colour `#1f6fd1`, lines `#4aa3ff`, text `#33415c`, strong text `#1d2b44`, quiet text `#6b7a90`, main action `#34b36a`, careful / never seen `#f08c00`, STOP `#e8382e`, robot face `#ffd84d`, cheeks `#ff7a90`; rounded font stack `ui-rounded, "SF Pro Rounded", "Nunito", "Arial Rounded MT Bold", system-ui, sans-serif`; radius 12 px, cards 18 px; no scanlines, glow or corner brackets.
- No variable name means two things across pages.
- Budgets (gzipped): `/` 25 KB, `/settings` 36 KB, `/logo` **28 KB** (raised from 26, design-notes §207), `/help` 16 KB, `/ai` 24 KB.
- Pages stay ES5 (`var`, `function`), no new dependencies; generated files never edited by hand.
- Commits: Conventional Commits, ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus
1. A page opened in private mode (storage throws) still picks a look and the button still flips it for that visit.
2. A canvas drawn before the flip shows the new colours right after it (not only after new data arrives) - heading tape, path, Tune graph, camera boxes.
3. Sunny on the drive page over a dark or bright camera picture: chip text stays readable (frosted white glass, navy text).
4. `/logo` code colours (move, turn, repeat, show, sense, maths, number, own word, comment) readable on white.
5. A page loaded with `cq_theme` set to a junk value falls back to the default, not to no theme.

---

### Task 1: The cockpit baseline (before anything changes)

**Files:**
- Create: `firmware/esp32/web/test/look_check.js`, `firmware/esp32/web/test/look_baseline.json`

**Interfaces:**
- Produces: `look_check.js` - a Playwright `run_code` function. With `MODE = "baseline"` it returns `{page: {hash, n}}` for `/`, `/logo`, `/settings`, `/help`, `/ai` loaded at 390×844 and 1440×900 with `localStorage cq_theme = "cockpit"`; `hash` is an FNV-1a hash (in the page) of, for every element in document order, its tag, id, and computed `color, background-color, border-top-color, border-right-color, border-bottom-color, border-left-color, box-shadow, text-shadow, font-family, outline-color, fill, stroke, opacity`; `n` is the element count. With `MODE = "check"` it compares against the baseline object pasted into the file and adds the Sunny checks of Task 7.

- [ ] **Step 1:** Write `look_check.js` with the baseline mode; copy it to `.playwright-mcp/` (Playwright MCP only reads files there).
- [ ] **Step 2:** Run it twice on `fake_tank.py` from this worktree (`cd firmware/esp32 && python3 web/test/fake_tank.py 8765`).
  Expected: identical hashes both runs. A page whose hash differs between runs is unstable (live data): fix the check by waiting for the page to settle or by excluding the changing element by id, and note it in the file.
- [ ] **Step 3:** Save the result as `look_baseline.json` and paste it into `look_check.js`'s `BASELINE`.
- [ ] **Step 4:** Commit: `test(web): the cockpit look baseline of every page (bridge LLD R28.5)`.

### Task 2: Shared theme - `theme.js`, `theme.css`, the pack marker; `/ai` moved onto it

**Files:**
- Create: `firmware/esp32/web/theme.js`, `firmware/esp32/web/theme.css`, `firmware/esp32/web/test/theme.test.js`, `firmware/esp32/scripts/test_theme_pack.py`
- Modify: `firmware/esp32/scripts/pack_web.py`, `firmware/esp32/web/ai.html`, `firmware/esp32/web/test/theme_check.js`

**Interfaces:**
- Produces: `window.cqTheme = { KEY: "cq_theme", pick(saved, coarse) -> "play"|"cockpit", current() -> string, flip() -> string, colour(name) -> string }` as R28.2; `flip()` dispatches `new Event("cqtheme")` on `window`. `theme.js` ends with `if (typeof module !== "undefined") module.exports = cqTheme;` and only touches `document` / `localStorage` / `matchMedia` when they exist (Node loads it for tests).
- Produces: `pack_web.with_theme(text: str, project: pathlib.Path, page: str) -> str` - replaces `<!--CQ:THEME-->` with `<style>` + theme.css + `</style><script>` + theme.js (every `</` written `<\/`) + `</script>`; raises `SystemExit("pack_web: <!--CQ:THEME--> missing in <page>")` when absent. `assemble_page` applies it to every page.

- [ ] **Step 1: Failing tests.**
  `theme.test.js`: `pick(null, true) === "play"`, `pick(null, false) === "cockpit"`, `pick("cockpit", true) === "cockpit"`, `pick("play", false) === "play"`, `pick("junk", false) === "cockpit"`, `pick("", true) === "play"`.
  `test_theme_pack.py`: `with_theme("<head><!--CQ:THEME--></head>", PROJECT, "/x")` contains `<style>` with `data-theme="play"` and `<script>` with `cqTheme` and no `</script>` before the closing one; a text without the marker raises `SystemExit` naming `/x`; `assemble_page(PROJECT, uri)` for each of `/`, `/settings`, `/logo`, `/ai`, `/help` contains `cqTheme` exactly once.
- [ ] **Step 2:** Run `node --test web/test/theme.test.js` and `python3 -m unittest discover -s scripts -p 'test_*.py'` (from `firmware/esp32`). Expected: FAIL (no theme.js / no `with_theme`).
- [ ] **Step 3:** Write `theme.js` and `theme.css`. `theme.css` starts with `/ai`'s current Sunny block (moved out of `ai.html`), its `--line` renamed `--tint`. Add `with_theme` to `pack_web.py`, and the marker `<!--CQ:THEME-->` right after `<title>` in all five pages (the pages other than `/ai` get no other change in this task).
- [ ] **Step 4:** In `ai.html`: remove its head theme script and Sunny block; rename `--line` to `--tint` everywhere in its CSS; the palette button calls `cqTheme.flip()`; the box colours in its canvas code come from `cqTheme.colour("--g")` (things), `cqTheme.colour("--amber")` (person), and the label background from a new variable `--box-tag` (cockpit `rgba(5,8,5,.75)`, Sunny `rgba(255,255,255,.88)`); on `cqtheme` it redraws the last frame.
- [ ] **Step 5:** Update `theme_check.js` to the shared key and run it, `ai_check.js`, the unit tests and `look_check.js` (baseline mode, compare by eye with `look_baseline.json`). Expected: all pass, all five baseline hashes unchanged.
- [ ] **Step 6:** `python3 scripts/pack_web.py` - every page within budget. Commit: `feat(web): one shared theme (theme.css, theme.js, <!--CQ:THEME-->); /ai uses it (bridge LLD R28)`.

### Task 3: Drive page

**Files:** Modify `firmware/esp32/web/basic_drive.html`, `firmware/esp32/web/theme.css`

**Interfaces:** Consumes `cqTheme.flip()`, `cqTheme.colour(name)`, event `cqtheme`.

- [ ] **Step 1:** Add to `look_check.js` (check mode) for `/` (its background is the camera picture, so the glass is checked, not the page colour): in Sunny `#menuBtn` text colour is `rgb(29, 43, 68)` and its background is white glass (`rgba(255, 255, 255, 0.82)`), STOP is `rgb(232, 56, 46)`, a `#miLook` item exists in `#menuSheet` and clicking it flips `data-theme`; the heading tape's pixel at its centre caret changes colour after the flip. Run: FAIL.
- [ ] **Step 2:** Every fixed colour in the page's CSS becomes a variable in its `:root` (same cockpit value); their Sunny values go into `theme.css` (glass `rgba(255,255,255,.82)`, glass text `#1d2b44`, edges `rgba(74,163,255,.45)`). `drawTape` takes its colours from `cqTheme.colour(...)` and the page redraws the tape on `cqtheme`. Add `<a class="mi" id="miLook" href="#"><!--icon:palette--><span><b>LOOK</b><small>sunny or cockpit</small></span><i>›</i></a>` after the HELP item; its click calls `cqTheme.flip()` and keeps the menu open.
- [ ] **Step 3:** Run `look_check.js` (cockpit hash of `/` unchanged; Sunny checks pass), `hide_stop_check.js`, `browser_check.js`. Expected: PASS. Commit: `feat(web): the drive page in both looks, LOOK in the menu (bridge LLD R28)`.

### Task 4: `/logo`

**Files:** Modify `firmware/esp32/web/logo.html`, `firmware/esp32/web/theme.css`, `firmware/esp32/scripts/pack_web.py` (`LOGO_BUDGET = 28 * 1024`)

- [ ] **Step 1:** `look_check.js` for `/logo`: Sunny body background `rgb(255, 248, 236)`, STOP `rgb(232, 56, 46)`, `#themeBtn` in the header flips; the path canvas pixel under the drawn path changes after the flip. Run: FAIL.
- [ ] **Step 2:** Variables as Task 3. Sunny code colours: move `#1a8f3c`, turn `#6b8f00`, flow `#0a8f99`, show `#c2189b`, sense `#1f6fd1`, maths `#33415c`, num `#c26a00`, own `#1d2b44`, cmt `#7d8ba0`. Palette button before STOP. `drawOn` takes every colour from `cqTheme.colour(...)` (grid, planned path, AREA circle, driven path, start mark, scale bar, tank arrow); redraw both canvases on `cqtheme`.
- [ ] **Step 3:** Run `look_check.js`, `logo.test.js`, `browser_check_wide.js`; pack within 28 KB. Commit: `feat(web): /logo in both looks (bridge LLD R28)`.

### Task 5: `/settings`

**Files:** Modify `firmware/esp32/web/settings.html`, `firmware/esp32/web/theme.css`

- [ ] **Step 1:** `look_check.js` for `/settings`: Sunny background, `#themeBtn` at the right of the header flips; the Tune graph's grid pixel changes after the flip. Run: FAIL.
- [ ] **Step 2:** Variables as Task 3 (the graph box background `#020502` becomes `--graph-bg`, Sunny `#ffffff`); the graph's grid, labels and line colours from `cqTheme.colour(...)` (lines: new variables `--g-left`, `--g-right`, … one per series, cockpit values the current ones); redraw on `cqtheme`.
- [ ] **Step 3:** Run `look_check.js`, `camera_centre_check.js`, `camera_tuning_check.js`. Commit: `feat(web): /settings in both looks (bridge LLD R28)`.

### Task 6: `/help`

**Files:** Modify `firmware/esp32/web/help.html`, `firmware/esp32/web/theme.css`

- [ ] **Step 1:** `look_check.js` for `/help`: with `cq_theme = "play"` the background is `rgb(255, 248, 236)`; no switch on the page. Run: FAIL.
- [ ] **Step 2:** Variables as Task 3; its own `--line` (`#1d3a22`, a rule line) keeps its name, Sunny value `#e6eef8`. Run: PASS, cockpit hash unchanged. Commit: `feat(web): /help follows the look (bridge LLD R28)`.

### Task 7: Every page together, on the tank

**Files:** Modify `firmware/esp32/web/test/look_check.js`, `docs/design-notes.md` (§207 as built), `docs/lld/esp32-bridge-lld.md` (R28 status), `CLAUDE.md` (R28 and Plan Sunny in the tables)

- [ ] **Step 1:** `look_check.js` check mode end to end: choose Sunny on `/logo`, then `/`, `/settings`, `/help`, `/ai` all load Sunny; choose cockpit on `/settings`, all load cockpit; storage that throws (`Object.defineProperty(window, "localStorage", {get() { throw new Error(); }})` in an init script) - the default look loads and the button still flips for the visit; a junk `cq_theme` gives the default; no sideways scroll at 390 px on any page in Sunny. Run: PASS.
- [ ] **Step 2:** Screenshots of every page in both looks (phone and laptop) to `.playwright-mcp/` for the owner.
- [ ] **Step 3:** OTA flash (`scripts/ota.sh`, `pio` from `~/.platformio/penv/bin` on the path); `curl --compressed http://192.168.31.167/<page>` shows `cqTheme` on all five.
- [ ] **Step 4:** Owner checks on the iPhone (nothing moves): each page opens Sunny; LOOK in the ☰ MENU flips the drive page; the choice holds across pages; STOP red everywhere.
- [ ] **Step 5:** Docs as built; commit `docs: Plan Sunny as built (design-notes §207, bridge LLD R28)`; push.

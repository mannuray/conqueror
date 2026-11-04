# Plan Strip - one status strip on /ai and /logo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `/ai` and `/logo` show the same status strip - battery %, AI HELPER, (pictures/s on /ai), HDG, POS, SPEED, WIFI, UNO - built once.

**Architecture:** `web/strip.html` / `strip.css` / `strip.js` (`window.cqStrip`) inlined at `<!--CQ:STRIP-->` by `pack_web.py`, like the top bar (R29). Each page hands its tank messages to `cqStrip.tank(m)`; /ai also feeds the AI helper chip, the engine pose and pictures/s.

**Tech Stack:** plain HTML/CSS/ES5 packed by Python, node `--test`, Python unittest, Playwright MCP checks on `fake_tank.py` :8765.

**Spec:** `docs/lld/esp32-bridge-lld.md` R30, `docs/design-notes.md` §209.

## Global Constraints
- Chip order: battery, AI HELPER, pictures/s (/ai only), HDG, POS, SPEED, WIFI, UNO; the same look on both pages and in both looks.
- No new connections: values from the messages each page already gets (plus `wifi_status` asked on connect).
- ES5, no new dependencies, colours from CSS variables; `/logo` budget 32 KB, `/ai` 24 KB.
- Never weaken safety: no STOP or drive code changes.
- Commits: Conventional Commits, ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus
1. /logo's chips while a program runs from another device (POS from someone else's `logo_pose`).
2. /ai's AI HELPER chip: `ai_at` must not override the feed state the page sets.
3. A dropped socket and reconnect on both pages: RECONNECTING, then the values come back.
4. The strip on a phone (390 px): wraps cleanly, no sideways scroll, the top bar stays one row.
5. USB power and a critical battery: "USB" and the red cell on both pages.

---

### Task 1: The shared strip, and /ai on it
**Files:** Create `web/strip.html`, `strip.css`, `strip.js`, `web/test/strip.test.js`, `scripts/test_strip_pack.py`, `web/test/strip_check.js`. Modify `scripts/pack_web.py`, `web/ai.html`, `web/ai.js`, `web/test/ai.test.js`.
- [ ] Step 1: failing tests - `strip.test.js` (battState / BattMean moved from `ai.test.js`; `wifiText("home", -50)` → icon `wifi`, text `HOME`, warn false; `("home", -80)` → warn true; `("ap")` → `OWN WIFI`; `posText(0.3, 0.5)` → `POS 0.3,0.5`; `tank()` routing); `test_strip_pack.py` (marker on /ai once; `window.cqStrip =` once; no `</` in its script).
- [ ] Step 2: run - FAIL.
- [ ] Step 3: write the strip; `with_strip` (after `with_bar`, before icons); /ai: marker in place of its chips, its tank handler calls `cqStrip.tank(m)`, `showFps()`, `aiHelper(on)`, `pose()`, `fps()`; it asks `wifi_status` on connect; ai.js loses the moved functions.
- [ ] Step 4: `strip_check.js` for /ai (chips in order, values from fake messages); run it, `ai_check`, `look_check` (skips `.chips`), `bar_check`. PASS.
- [ ] Step 5: commit `feat(web): one shared status strip (strip.html, strip.css, strip.js, <!--CQ:STRIP-->); /ai on it (bridge LLD R30)`.

### Task 2: /logo on the strip
**Files:** Modify `web/logo.html`, `web/logo.js`, `web/test/logo.test.js`, `scripts/pack_web.py` (LOGO_BUDGET 32 KB), the checks.
- [ ] Step 1: `strip_check.js` for /logo: the same chips and computed look as /ai (fps hidden), `ai_at` turns AI HELPER on, `logo_pose` sets POS in m. Run: FAIL.
- [ ] Step 2: marker in place of /logo's chips; its `onMessage` calls `cqStrip.tank(m)`, its socket close `cqStrip.linkDown()`; logo.js loses the moved functions (tests moved).
- [ ] Step 3: run `strip_check`, `browser_check`, `browser_check_wide` (its battery / WiFi lines check the strip), `look_check`, `bar_check`, node and Python tests. Commit `feat(web): /logo on the shared status strip (bridge LLD R30)`.

### Task 3: On the tank, docs
- [ ] Step 1: screenshots of /ai and /logo, phone and laptop, both looks.
- [ ] Step 2: OTA flash; `curl --compressed` /ai and /logo show `window.cqStrip =`. Owner on the iPhone (nothing moves): the same strip on both pages.
- [ ] Step 3: docs as built (design-notes §209, R30); commit `docs: Plan Strip as built (design-notes §209, bridge LLD R30)`; push.

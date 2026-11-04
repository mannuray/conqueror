# Web pages architecture review (2026-11-04, branch plan-strip)

Asked by the owner after Plan Strip: "review the web arch, if we can improve it, does it follow the standard or has it drifted".
Read-only review of `firmware/esp32/web/`, `scripts/pack_web.py`, `cq_icons.py`, the tests, `docs/standards/coding-standards.md`
and bridge LLD R2-R30. Python 36 and node 86 tests passed at review time. Nothing here is decided yet: each plan below
needs the owner's yes and its own LLD revision first.

## Verdict
The shared parts (theme R28, top bar R29, status strip R30, icons R25) follow their LLDs and are tested at packing level.
Drift is in three places: the pages' **link plumbing was never shared** (four copies of connect / ping / reconnect / send /
STOP that now differ, some in STOP); the **coding standards have no web-pages section**; the **page budgets** are raised each
revision and no longer limit anything.

## (a) Bugs and safety-relevant differences
1. STOP fires on `pointerdown` on Drive and /logo, on `click` on /ai, Settings and Help - a sliding tap can miss it there.
2. STOP is silently lost while /ai, /logo (and Drive) are reconnecting; Settings and Help fall back to `cqBar.stopOnce`.
3. Only Drive closes a silent socket (4 s); elsewhere a WiFi drop without a close leaves the page "live" and STOP goes nowhere.
4. The strip's WiFi chip is asked for once on connect and never refreshed (the bridge answers `wifi_status` only on request).
5. `strip.js`: a comment swallowed the starting `fps` / `hdg` values - **fixed** with a test (this commit).
6. `assemble_logo` does not escape `</` in logo.js (no live bug today).

## (b) Drift from docs and standards
- No web-pages rules in the coding standards (practice: ES5-style, one IIFE, `"use strict"`, a pure core with node tests);
  `size_check.py` covers C++ only (settings.html 1008 lines, basic_drive.html 983).
- Budgets: /logo 24 -> 26 -> 28 -> 30 -> 32 KB; headroom now / 1.1 KB, /logo 1.0 KB, /ai 1.3 KB, /settings 6.7 KB, /help 1.8 KB.
  The budget guards load time over WiFi (the app partition is 2.875 MB); no policy is written.
- Doc mismatches: LLD R2 says the icon PNG is gzipped from a file (it is drawn); /help budget 12 KB in the LLD, 16 KB in code;
  pack_web names `icons.py` (it is `cq_icons.py`); R28.2 lists an `assemble_theme()` that does not exist; R27.4's /ai modules
  were built as one IIFE without an as-built note; dead `FILTERS-BEGIN/END` markers in basic_drive.html.
- Drive's and Settings' safety logic (`mightDrive`, `hideStop`, the latch) has no node-tested core.

## (c) Duplication worth sharing
- The tank link (~15 lines x 4): one `link.js` part (`cqLink.open({client, onMessage, onOpen, onClose})`) with the silence
  rule, the backoff, hidden = no link, and the STOP wiring with the `stopOnce` fallback; fixes (a)1-3.
- The `<head>` lines (charset, viewport, theme-color, icons) in five pages - how /ai lost its icon (fixed, `test_page_head.py`).
- The cockpit CSS variables repeated per page with small differences (`--panel` three values; /ai's own names) - sharing them
  changes what users see slightly: owner's call.
- Helpers: `$`, `toast`, the battery curve (Drive and strip.js), the RSSI thresholds (three places), bar.js `PAGES` vs bar.html.

## (d) Build and test
- pack_web: `with_theme` / `with_bar` / `with_strip` and three `assemble_*` are near copies - one parts table and one pages +
  budgets table would replace them, with escaping and checks applied the same way.
- 11 Playwright checks run by hand, ~10 with their own fake WebSocket and fixed sleeps - a shared `fake_ws.js` and one runner.
- The pre-commit hook does not run the node or Python web tests (~1 s).

## (e) Size
Stripping whole-line `//` comments inside `<script>` (and `/* */` in `<style>`) at pack time, after all parts and icons,
guarded by `node --check` on each packed script: about /logo 2.2-2.8 KB, Drive 2.0-2.3 KB, /ai 1.6-2.0 KB, /settings 1.1-1.6 KB.

## Recommended order (each a plan and branch, LLD revision first)
1. **One link** - shared `link.js` (STOP on `pointerdown` + fallback, silence rule, backoff, hidden = no link, WiFi refresh);
   /ai, /logo, /settings, then Drive with its §201 / R11 rules as hooks. Stand check of STOP on every page.
2. **Packer** - parts and pages tables, `</` everywhere, comment stripping with the `node --check` guard, a written budget
   policy, the doc fixes.
3. **Shared base** - a head part, one cockpit `:root` (owner to agree on the visible colour changes), Drive using the strip's
   battery / WiFi rules, a web-pages section in the coding standards.
4. **Page tests** - shared fake socket, one runner, condition waits, web tests in the pre-commit hook, a node-tested core for
   Drive's safety logic.

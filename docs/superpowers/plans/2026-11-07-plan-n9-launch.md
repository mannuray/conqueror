# Plan N9 — `LAUNCH`: a second part alongside Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `LAUNCH [ block ]` runs a second, non-moving part of a Tank Logo program alongside the main one, so lesson 5 can beep while WANDER drives.

**Architecture:** the block compiles to its own code entry and `LAUNCH` to an effect (`EffectKind::kLaunch`, its entry as the argument); the checker refuses any moving word reachable from a `LAUNCH` block (7057); the `RunController` keeps per-part state (`Part`) for two `Vm`s over one `Program` and gives each a slice in turn; the page marks the launched part's line.

**Tech Stack:** Tank Logo spec YAML + `logogen`, C++17 ESP32 bridge (PlatformIO, Unity native tests), plain JS (node:test).

**Spec:** Tank Logo LLD §26 (`docs/lld/tank-logo-lld.md`), design-notes §219. Branch `plan-n9-launch` (worktree `scratchpad/plan-n9-launch`, on `plan-l1-lessons`).

## Global Constraints
- **Only the main program moves the tank** (owner). Refused inside a LAUNCH block, followed through words of your own: the `move` and `turn` groups, `GO`, `HOLD`, `HOME`, `SPEED`, `LOOK`, `FIND`, `WANDER`, `EXPLORE`, `SETXY`, `AREA` → 7057 "{word} moves the tank - only the main program can do that, not LAUNCH." at check time; the runner refuses a motion effect from part 1 with 7057 too (a guard).
- One launched part at a time → 7058 "Only one LAUNCH can run at a time." (run time).
- `STOP` in either part ends both as finished (§25.6); an error in either ends both; the STOP key, 7041, the deadman and every safety stop unchanged.
- STEP: the main part steps; part 1 is paused while the main is paused.
- `LAUNCH` level 2, kind control, group flow, input `block`.
- Generated files never edited: change `logo/spec/*`, run `cd logo && uv run logogen write`.
- `/logo` within 32 768 B gz.
- Commits: Conventional Commits ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus
1. A moving word reached only through a chain of your own words (A calls B calls FORWARD, A inside LAUNCH) must be 7057 at A's call.
2. A recursive word of your own inside LAUNCH must not make the 7057 check loop for ever.
3. Part 1 waiting on a question (`HOWFAR`) while the main runs a skill: the answer goes to the part that asked; the main's skill end goes to the main.
4. STOP key / page gone / 7041 during both parts: one ending, the tank stopped, the lease ended, both `Vm`s idle.
5. A `LAUNCH` inside a word of your own, called twice: the second → 7058 while the first runs; after the first ended on its own a new LAUNCH is allowed.

---

### Task 1: the word, the checker and the compiler

**Files:** `logo/spec/words.yaml` (LAUNCH, errors 7057 / 7058), `logo/spec/tests/reading.yaml` or `errors.yaml` (cases), generated files via `logogen write`; `firmware/esp32/src/logo/program.h` (`EffectKind::kLaunch`), `compiler.cpp` / `compiler_stmt.cpp` (role `kLaunch`), `checker.{h,cpp}` (7057), `effects_text.cpp` (`effectWord` "LAUNCH").

**Interfaces:**
- Produces: `EffectKind::kLaunch` with the block's code entry as its one argument (`Effect` value 0 = entry); the block's code ends with `kHalt`; `Checker` 7057 rule.

- [ ] **Step 1: Failing cases** (`errors.yaml`): `LAUNCH [ FORWARD 10 ]` → 7057 line 1 col 10; `TO GO2 FD 10 END  LAUNCH [ GO2 ]` → 7057 at `GO2` inside the block; `TO A B END  TO B LT 90 END  LAUNCH [ A ]` → 7057 at `A`; `TO R R END  LAUNCH [ R ]` → no 7057 (recursion, Review Focus 2; whatever else the checker says about R stays as today); `LAUNCH [ LOOK LEFT ]` → 7057; `LAUNCH [ AREA 2 ]` (ai tank) → 7057; `LAUNCH` → 7002. Reading: `LAUNCH [ BEEP ]  FD 10` fine.
- [ ] **Step 2:** `cd logo && uv run logogen write && cd ../firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_conformance` → the new cases FAIL (unknown word / no 7057).
- [ ] **Step 3: Implement.** words.yaml entry and errors; checker: for each word of your own, "moves" = it calls a moving word or a moving word of your own (fixed point over the definitions, so recursion ends); inside a LAUNCH block (the block node and nested blocks), a moving built-in or a moving word of your own → 7057 at that call; compiler: `LAUNCH` emits `kPushN <entry>`, `kEffect kLaunch 1`, and the block's code is placed after the main code (like a word of your own's body) ending `kHalt`.
- [ ] **Step 4:** the conformance tests, `test_logo_compiler`, `test_logo_checker` and `logogen check` → PASS; the whole native suite.
- [ ] **Step 5: Commit** `feat(logo): LAUNCH reads, checks (7057) and compiles (Tank Logo LLD §26)`.

### Task 2: the runner takes turns

**Files:** `firmware/esp32/src/logo/run_controller.{h,cpp}`, `simulator.cpp` (kLaunch ignored), `logo/spec/tests/running.yaml`, `firmware/esp32/test/test_logo_run/test_main.cpp`, `test/test_logo_running/test_main.cpp` (a second `Vm`).

**Interfaces:**
- Consumes: `EffectKind::kLaunch` (Task 1).
- Produces: `RunController(Vm& main, Vm& launched)`; `RunOut::part` (0 / 1) with `line` / `line_no`; `Vm::start(const Program&, uint32_t seed, uint16_t entry)` (or `load` + entry).

- [ ] **Step 1: Failing cases** (`running.yaml`; the `does` order is the turn order the runner gives, pinned once by the first green run and then fixed): `LAUNCH [ BEEP BEEP ]  FORWARD 50` (beeps and the move, finished); `LAUNCH [ FOREVER [ BEEP WAIT 1 ] ]  WAIT 2.5` (three beeps while the main waits, finished); `LAUNCH [ STOP ]  FOREVER [ WAIT 1 ]` → finished; `LAUNCH [ BEEP WAIT 5 ]  LAUNCH [ BEEP ]` → 7058 line 1; `LAUNCH [ BEEP ]  WAIT 1  LAUNCH [ BEEP ]` → finished (the first ended on its own); lesson 5's §26.2 program with `world: {answers: [120, 80, 30], skill_ends: []}` → HOWFAR, BEEP …, SOUND SUCCESS, SAY found you, finished (the skill never ends on its own; the STOP ends it). Native: `test_part_one_cannot_move` (a hand-built Program whose launched entry has a FORWARD effect → 7057).
- [ ] **Step 2:** regenerate, run `test_logo_running` / `test_logo_run` → FAIL.
- [ ] **Step 3: Implement** a `Part` struct per `Vm` with the per-part state now in `RunController` (state, wake, the question fields, last effect); motion, GO, skill, area and speed state stay the main's. `tick`: the main's slice unless it waits, then part 1's unless it waits or the main is paused in STEP; a motion / skill / area / look effect from part 1 → 7057; a question's answer and `onSkillEnd` go to the part that is waiting for them; any ending ends both. The simulator skips `kLaunch`.
- [ ] **Step 4:** the native suite → PASS; record the second `Vm`'s size in the commit message.
- [ ] **Step 5: Commit** `feat(logo): the runner runs a launched part alongside (Tank Logo LLD §26.4)`.

### Task 3: the tank and the page

**Files:** `firmware/esp32/src/net/logo_worker.cpp` (a second `logo::Vm`, the `RunController` with both), `src/core/ws_json*.{h,cpp}` (`jsonLogoLine` gains `part`), the place that sends it (`app/logo_controller.cpp`), `web/logo.html` / `web/logo.js` (a second line marker), `web/test/logo.test.js`, `test/test_command_router` etc. where a `Vm` / `RunController` is built.

- [ ] **Step 1: Failing tests:** a ws_json test `{"t":"logo_line",...,"part":1}`; `logo.test.js`: the line marker state for a `logo_line` with `part: 1` is kept apart from the main's (a pure helper in `logo.js`).
- [ ] **Step 2:** FAIL. **Step 3:** implement; the launched line gets its own colour class in the editor gutter.
- [ ] **Step 4:** native suite, `node --test`, `pio run -e bridge` (size; `/logo` budget in the commit message).
- [ ] **Step 5: Commit** `feat(bridge): a launched part's line on /logo (Tank Logo LLD §26.4)`.

### Task 4: lesson 5 with LAUNCH

**Files:** `firmware/esp32/src/logo/program_names.cpp` (`hot_and_cold_hunt` per §26.2 / §26.6), `test/test_logo_box` (unchanged rules still pass), `running.yaml` (lesson 5's code case kept equal to the lesson's code lines).

- [ ] **Step 1:** change the lesson; run `test_logo_box` and `test_logo_running` → PASS (the built-in checks with the AI caps; the lesson 5 case from Task 2 matches its code).
- [ ] **Step 2: Commit** `feat(logo): lesson 5 hunts with LAUNCH - it beeps while it wanders (Tank Logo LLD §26.6)`.

### Task 5: docs as built, then the tank
- [ ] LLD §26.7 as built, design-notes §219 "Built", CLAUDE.md plans row; commit, push.
- [ ] With the owner (floor, STOP in hand, camera check): flash over OTA; lesson 5 with `"toy` - smooth wandering while it beeps, "found you", Done; the STOP key during it; a program with a moving word inside LAUNCH shows 7057 in the editor.

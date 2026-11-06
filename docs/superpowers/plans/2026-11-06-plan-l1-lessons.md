# Plan L1 — Lessons in `/logo` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** five built-in, read-only lesson programs on the tank, listed as `[ LESSONS ]` 1-5 on `/logo`.

**Architecture:** the lessons join the built-in examples table on the ESP32 (`logo/program_names`) with a `lesson` flag; the program list JSON carries the flag; the page's list is drawn from a small pure function in `logo.js`. No new words, no runner change.

**Tech Stack:** C++17 (ESP32 bridge, PlatformIO native tests with Unity), Tank Logo spec YAML + `logogen`, plain JS (node:test).

**Spec:** Tank Logo LLD §25 (`docs/lld/tank-logo-lld.md`), design-notes §214, §216. Branch `plan-l1-lessons` (worktree `scratchpad/plan-l1-lessons`, on top of `plan-z9-find`).

## Global Constraints
- No new Tank Logo words; lessons use level-1/2 words and the AI words only (v1: SENTENCE and other level-3 words give 7030).
- Generated files are never edited by hand: change `logo/spec/*`, run `cd logo && uv run logogen write`.
- Lessons are read-only exactly like examples: built-ins keep `"example":true`.
- Order on the list: `SAVED ON THE TANK`, `[ LESSONS ]` (numbered 1-5, in the table's order), `[ EXAMPLES ]`.
- Each lesson starts with `;` lines: a title `; Lesson N - ...`, what it does, what to watch, `; Try this: ...`.
- "Found" distances: 40 cm (lesson 4), 50 cm (lesson 5). Searching lessons start with `AREA 2`.
- `/logo` page stays within its budget (32 768 B gz; 30 583 B today).
- Commits: Conventional Commits ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus
1. A lesson text that the checker accepts only with AI caps: with no AI helper the page shows 7020 — expected, but the lesson must not fail any *other* check (the test uses AI caps with ball / toy).
2. `doList`'s buffer: 32 saved + 8 built-ins must fit (`kMaxPrograms + kBuiltInCount`), or saved programs drop off the list.
3. Saving or deleting under a lesson name must be refused (`kExample`), never overwrite.
4. Lesson 4 with the ball never seen (HOWFAR −1) must only wait, not beep or end.
5. The 300 s run limit ends lessons 3 and 5 with 7041 — the comment says so; nothing else changes.

---

### Task 1: the lessons in the built-in table

**Files:**
- Modify: `firmware/esp32/src/logo/program_names.h`, `firmware/esp32/src/logo/program_names.cpp`
- Test: `firmware/esp32/test/test_logo_box/test_main.cpp`

**Interfaces:**
- Produces: `struct Example { const char* name; const char* src; bool lesson; };` and `constexpr uint8_t kBuiltInCount = 8;` in `program_names.h`; `exampleCount()`, `example(i)`, `findExample()` unchanged in signature. Index 0-4 = lessons in order, 5-7 = `first_square`, `avoid_walls`, `look_around` (lesson = false).

- [ ] **Step 1: Write the failing test** — replace `test_every_example_checks_ok` with `test_every_built_in_checks_ok_with_the_ai_helper`:
  - `TEST_ASSERT_EQUAL(kBuiltInCount, exampleCount()); TEST_ASSERT_EQUAL(8, kBuiltInCount);`
  - names in order: `back_to_start`, `where_is_my_ball`, `treasure_hunt`, `hot_and_cold`, `hot_and_cold_hunt`, `first_square`, `avoid_walls`, `look_around`; `lesson` true for 0-4, false for 5-7;
  - each lesson's `src` starts with `"; Lesson "` + its number, and contains `"; Try this:"`;
  - every built-in passes `programName` and `checkProgram` with a `Caps` built like `test_logo_checker`'s `withThings` (ai = true, things `toy` = car / racing car, `ball`); on failure print the name (`TEST_ASSERT_TRUE_MESSAGE`);
  - `findExample("treasure_hunt")` not null, `findExample("nothing")` null.
- [ ] **Step 2: Run it** — `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_box`. Expected: FAIL to compile (`kBuiltInCount`, `lesson` not defined).
- [ ] **Step 3: Implement** — add the flag, the constant, a `static_assert(sizeof kExamples / sizeof kExamples[0] == kBuiltInCount)`, and the five lessons (LLD §25.2; draft of lesson 4 there). Lesson texts:
  1. `back_to_start`: `FORWARD 60  RIGHT 90  FORWARD 40  SAY [ back to the start ]  SETXY 0 0  SOUND SUCCESS`; comments: it never lands exactly on the spot - every turn adds a little error; Try this: tape the start, change 60 to 120.
  2. `where_is_my_ball`: `AREA 2  FIND "ball  SAY [ found it in ]  SAY ROUND TIMER`; comments: watch it look round, search, check; Try this: run it again with a home - it remembers.
  3. `treasure_hunt`: `AREA 2  FIND "car  SAY [ found the car! ]  FIND "ball  SAY [ found the ball! ]  SETXY 0 0  PLAY [ C4 E4 G4 C5 2 ]`; comments: after 5 minutes any program stops by itself; Try this: swap car and ball, or add a third thing.
  4. `hot_and_cold`: the LLD §25.2 draft (TO HOTCOLD, found under 40 cm).
  5. `hot_and_cold_hunt`: `AREA 2` then `FOREVER [ WANDER 2  LISTEN HOWFAR "ball ]` with `TO LISTEN :cm` - not seen: `WAIT 0.5`; under 50: `SOUND SUCCESS  SAY [ found you! ]  STOP`; else `REPEAT 3 [ BEEP WAIT :cm / 200 ]`; comments: it can't beep while it drives - a program does one thing at a time; Try this: change WANDER 2 to WANDER 4.
  Lay each out one statement per line, as the examples do.
- [ ] **Step 4: Run it** — same command. Expected: PASS. If the checker rejects a lesson, read its diagnostic and fix the text with today's words (e.g. `SAY ROUND TIMER` not accepted → lesson 2 says `[ found it! ]` only, LLD §25.2). If a lesson cannot be written without a new word, stop and ask the owner.
- [ ] **Step 5: Run the whole native suite** — `~/.platformio/penv/bin/pio test -e native`. Expected: all pass.
- [ ] **Step 6: Commit** — `feat(logo): five built-in lessons beside the examples (Tank Logo LLD §25)`.

### Task 2: lesson 4 runs as it should

**Files:**
- Modify: `logo/spec/tests/running.yaml` (then `cd logo && uv run logogen write` regenerates `firmware/esp32/test/test_logo_running/cases.h`)

**Interfaces:**
- Consumes: lesson 4's program text from Task 1 (its code lines, comments may be left out).

- [ ] **Step 1: Write the failing cases** at the end of the AI-words block:
  - `lesson 4 - hot and cold beeps, then finds you`: source = lesson 4's code; `world: {answers: [120, 30]}`; `does: [HOWFAR ball, BEEP, "WAIT 1.2", HOWFAR ball, PLAY ..., SAY ...]` (the exact `does` strings as the runner reports them - copy the forms used by the cases above); `ends: finished` or the form `STOP` ends with in existing cases.
  - `lesson 4 - a ball not seen only waits`: `world: {answers: [null, null]}`; `does` shows only HOWFAR and waits until the answers run out (then -1 forever → the run-time limit; bound the case the way existing FOREVER cases do).
- [ ] **Step 2: Regenerate and run** — `cd logo && uv run logogen write && cd ../firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_running`. Expected: the new cases fail only if the `does` lines are wrong; fix the expectations from the runner's own output **only where they describe the same behaviour** (beep, wait scaled by distance, tune + say + end at < 40 cm, silence when not seen). A behaviour difference is a lesson-text bug: fix Task 1's text.
- [ ] **Step 3: Run** `uv run logogen check` (in `logo/`) and the native suite. Expected: clean, all pass.
- [ ] **Step 4: Commit** — `test(logo): lesson 4 hot and cold running cases (Tank Logo LLD §25.4)`.

### Task 3: the list says which are lessons

**Files:**
- Modify: `firmware/esp32/src/net/program_store.h`, `firmware/esp32/src/net/program_store.cpp`, `firmware/esp32/src/net/logo_http.cpp`

**Interfaces:**
- Consumes: `Example::lesson`, `kBuiltInCount` (Task 1).
- Produces: `GET /logo/programs` items `{"name":..,"bytes":..,"example":bool,"lesson":bool}`.

- [ ] **Step 1:** `ProgramInfo` gains `bool lesson`; `programList` sets it from `example(i).lesson` for built-ins, `false` for saved ones. `doList`'s buffer becomes `ProgramInfo list[logo::kMaxPrograms + logo::kBuiltInCount]`; the item format adds `,\"lesson\":%s` (check the 112-byte item buffer still fits: 24-char name + fields < 112).
- [ ] **Step 2: Build** — `cd firmware/esp32 && ~/.platformio/penv/bin/pio run -e bridge`. Expected: SUCCESS (net/ is not in the native build; the compiler with `-Werror` is the check here, the tank check is in Task 5).
- [ ] **Step 3: Commit** — `feat(bridge): the program list marks lessons (Tank Logo LLD §25.3)`.

### Task 4: `[ LESSONS ]` on `/logo`

**Files:**
- Modify: `firmware/esp32/web/logo.js`, `firmware/esp32/web/logo.html`
- Test: `firmware/esp32/web/test/logo.test.js`

**Interfaces:**
- Produces: `L.programSections(progs) -> [{title: string, items: [{name: string, label: string}]}]` exported from `logo.js`.

- [ ] **Step 1: Write the failing test** `programSections orders saved, lessons, examples`: input `[{name:"first_square",example:true,lesson:false},{name:"back_to_start",example:true,lesson:true},{name:"my_sq",example:false},{name:"where_is_my_ball",example:true,lesson:true}]` → `[{title:"SAVED ON THE TANK", items:[{name:"my_sq",label:"my_sq"}]}, {title:"[ LESSONS ]", items:[{name:"back_to_start",label:"1 back_to_start"},{name:"where_is_my_ball",label:"2 where_is_my_ball"}]}, {title:"[ EXAMPLES ]", items:[{name:"first_square",label:"first_square"}]}]`. Second test: no lessons in the list (an old bridge) → no `[ LESSONS ]` section.
- [ ] **Step 2: Run** — `node --test firmware/esp32/web/test/logo.test.js`. Expected: FAIL (`programSections` is not a function).
- [ ] **Step 3: Implement** `programSections` in `logo.js` (lessons keep the server's order); `showProgs` in `logo.html` draws from it, keeping `(new program)` as the first item of the saved section and the existing `on` / `dot` / click behaviour.
- [ ] **Step 4: Run** the test file and `node --test firmware/esp32/web/test/` . Expected: all pass.
- [ ] **Step 5: Budget** — `pio run -e bridge` (the web packer checks the page budgets). Expected: SUCCESS, `/logo` under 32 768 B gz; note the new size in the commit message.
- [ ] **Step 6: Commit** — `feat(web): /logo lists the lessons, numbered (Tank Logo LLD §25.3)`.

### Task 5: docs as built, then the tank (owner present)
- [ ] **Step 1:** LLD §25.5 "As built" (final lesson texts' differences from §25.2, sizes); design-notes §216 "Built" line; CLAUDE.md plans row for `2026-11-06-plan-l1-lessons.md`. Commit `docs: Plan L1 as built (Tank Logo LLD §25.5, design-notes §216)`, push.
- [ ] **Step 2 (with the owner; say what moves, stand or floor, STOP in hand, camera check):** flash the bridge over OTA (`CQ_OTA_PASSWORD` read into the environment, never echoed), engine running with a home; open `/logo`: `[ LESSONS ]` 1-5 shown; open each, its comments read well; RUN lesson 4 (nothing drives) with the ball by hand; lesson 1 on the floor; lessons 2, 3, 5 on the floor with the ball (and the car for 3). Record results in design-notes §216.

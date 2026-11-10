# Plan G3 — the game words, the lesson and the GAMES card Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A child can play the baby game: the Logo words `FACE`, `ESCAPE`, `FOLLOW` and the person in `SEE?` / `HOWFAR` run on the tank through the engine, lesson 6 "Chase with the baby" is the ready-made game, and `/ai`'s PLAY tab starts it from a GAMES card.

**Architecture:** Tank Logo gains a `person` input type (a fixed list of person words in `words.yaml`), three level-2 skill words and the game flag for them; the bridge sends them as `skill_start face | escape | follow` and person questions as `skill_ask … "game":true` (the engine side is Plan G2). Lesson 6 is a built-in program. The GAMES card reads the bridge's game events and the engine's AI feed (the person's distance, the seeing pass) and runs the lesson through the page's existing one-line run path.

**Tech Stack:** `logo/spec` + `logogen` (Python, generated C++ tables and conformance cases), the bridge (C++17, PlatformIO native Unity tests, the size check: CCN ≤ 10, nloc ≤ 40, files ≤ 400 lines), the engine's feed (Python 3.12, ruff, pyright strict), `web/ai.html` / `ai.js` (Node tests, `/ai` budget 34 KB).

**Spec:** Tank Logo LLD §27 (the game words, the flag, the lesson), bridge LLD R34 (game mode, the messages R34.3), AI engine LLD §28 (§28.9 as built), Spec 10 (`docs/superpowers/specs/2026-11-08-10-people-games-design.md`), design-notes §223-§224 (the UX: everything on PLAY, linked to its lesson), §229 (ESCAPE sidesteps).

**Base:** build only after `plan-g1-game-mode` and `plan-g2-person-watch` are merged into `main` (G3 uses G1's `CAUGHT?`, game flag, `GameController`, `jsonGameEvent`, and G2's skill server words); rebase this branch on that `main` first.

## Global Constraints
- Generated files are never edited by hand: change `logo/spec/*`, run `cd logo && uv run logogen write` (the pre-commit hook checks).
- The game words are **level 2** (the tank checks programs as Logo v1; level 3 is refused 7030 - Tank Logo LLD §27.7).
- Person words: `baby`, `person`, `kid` - the same list as the engine's `PERSON_WORDS` (AI engine LLD §28.9).
- Inputs: `ESCAPE` cm 20-100; `FOLLOW` seconds 1-60 (the follow distance is the engine's default 100 cm, never under 50).
- New error: 7062 "FACE, ESCAPE and FOLLOW are for a person in a game." (Tank Logo LLD §27.1). 7053 stays for a person in `FIND` / `SETXY` / a thing input.
- Caught is not an error: a word ending "caught" ends normally; `CAUGHT?` tells the program.
- `/ai` page budget 34 KB (bridge LLD R34.7); each part of PLAY links to its lesson on `/logo` (design-notes §224).
- Safety is never weakened: no change to the keep-out, the envelope, held or the deadman; every game word runs under them.

## Review Focus
1. `FACE "toy`, `ESCAPE :x 40` with `:x` = "ball" at run time, `FOLLOW "Baby` (case): a thing never reaches a game skill (7062), a person word in any case is a person.
2. A program whose only game word is in an unreached `TO` or a `LAUNCH` part: still a game run (the envelope on before the first move).
3. `SEE? "baby` outside a game (no flag possible: the word itself flags it) and `SEE? :who` computed: the person question carries `"game":true` only in a game run; a computed person in a non-game run is 7053.
4. The GAMES card while the engine is gone, the seeing pass missing, or the run held: START locked or the card says why; no stale "playing".
5. Lesson 6 with the 15-minute game limit (7063) and a caught pause: the program goes on after caught, never errors.

---

### Task 1: the spec - the person type, three words, 7062, conformance cases
**Files:** `logo/spec/words.yaml` (input type `person`; `person_words: [baby, person, kid]`; `FACE` (person), `ESCAPE` (person, cm 20-100), `FOLLOW` moved from its level-3 reservation to level 2 (person, seconds 1-60), each `needs: [ai, encoders]`, group `move`, with does / help / example as Tank Logo LLD §27.1; `SEE?` / `HOWFAR` thing input documented as "thing or person"; error 7062), `logo/spec/grammar.md` (the person input type), `logo/generator/logogen/*` (emit `InType::kPerson` and the person-word table), `logo/spec/tests/` conformance cases; then `logogen write`.
- [ ] **Failing cases first** (the generator's own tests and new conformance cases): `FACE "baby` ok and a game; `FACE "toy` → 7062; `FIND "baby` → 7053; `ESCAPE "baby 10` → 7025 (20-100); `FOLLOW "kid 5` ok; `SEE? "baby` ok and a game; the game flag from `FACE` in an unreached `TO` body and in a `LAUNCH` part; `FOLLOW "Baby 5` ok (case-folded).
- [ ] `uv run pytest` in `logo/` FAIL → implement → PASS; `uv run logogen write`; commit `feat(logo): the person input and the game words FACE / ESCAPE / FOLLOW (Tank Logo LLD §27)`.

### Task 2: the checker, compiler and runner on the tank
**Files:** `firmware/esp32/src/logo/checker.cpp` (a `person` input: a quoted word must be a person word, else 7062; the thing check (7053) lets a person word through only for `SEE?` / `HOWFAR`, which then flag the game; `FACE` / `ESCAPE` / `FOLLOW` flag the game), `program.h` (`SkillKind` gains `kFace`, `kEscape`, `kFollow`), `compiler.cpp` (the three words as `WordRole::kSkill`), `effects.cpp` (run time: a computed person input that is not a person word → 7062; a person word reaching a thing input in a non-game run → 7053, Tank Logo LLD §27.2), `run_controller` (a skill word ended "caught" ends normally).
- [ ] **Failing tests:** the generated conformance cases of Task 1 (`test_logo_conformance`); running cases (`test_logo_running`): `FACE "baby` makes one skill effect `kFace` with the word; `ESCAPE :x 40` with `:x` "ball" → 7062 at run time; `SEE? :who` with "baby" in a non-game program → 7053; a skill ended caught → the program goes on, `CAUGHT?` TRUE.
- [ ] `pio test -e native` FAIL → implement → PASS (the size check clean); commit `feat(logo): the game words run on the tank (Tank Logo LLD §27.2)`.

### Task 3: the bridge sends them
**Files:** `core/ws_json_skill.cpp` (`jsonSkillStart`: `"skill":"face"|"escape"|"follow"` with `"thing"`, escape `"cm"`, follow `"s"`; `jsonSkillAsk` gains `"game":true` when the run is a game and the thing is a person word), `app/logo_skills.cpp` (passes the run's game flag), tests `test_ws_json_skill`, `test_logo_skills`.
- [ ] **Failing tests:** the exact JSON for each word (within the 200-byte WS frame), `"game":true` only in a game run with a person word; `FACE` in a game starts a lease as `FIND` does and its `skill_done` ends the word.
- [ ] FAIL → implement → PASS; build the bridge (`pio run -e esp32s3`); commit `feat(bridge): skill_start face / escape / follow, person questions in a game (bridge LLD R34.3)`.

### Task 4: lesson 6 "Chase with the baby"
**Files:** `firmware/esp32/src/logo/program_names.{h,cpp}` (lesson 6 `chase_with_the_baby` after the five; `kBuiltInCount` 9), its text: comment lines explaining the game in plain words and what to change (the distances), then Tank Logo LLD §27.4's `BABYGAME` (`REPEAT :minutes * 60` over ~1 s steps, `IF CAUGHT? [ WAIT 3 ]`, `ESCAPE "baby 40` inside the escape distance, `FOLLOW "baby 2` beyond the follow distance, `FACE "baby`), ending `BABYGAME 70 100 5`; `web/test/logo.test.js` (the lessons list).
- [ ] **Failing tests:** the built-in list has 9 with lesson 6 sixth; its text checks clean on the tank's checker (a running-case or a check test) and is a game run; it stays under 15 minutes for `:minutes` ≤ 14 (the 7063 limit).
- [ ] FAIL → implement → PASS; commit `feat(logo): lesson 6, chase with the baby (Tank Logo LLD §27.4)`.

### Task 5: the AI feed tells the page about the person and the pass
**Files:** `ai-engine/src/conqueror_ai/app/feed.py` (the `status` message gains `"people": bool` - the seeing pass - and while a game is on `"person": {"seen", "d_cm", "coming"}` from `PersonWatch`), `app/serve.py` (hands the pass and the stream's on/off to the publisher), test `tests/test_feed.py`.
- [ ] **Failing tests:** status carries `people`; `person` only while the bridge's game is on, and its `d_cm` is the same pessimistic number the bridge gets.
- [ ] FAIL → implement → PASS (ruff, pyright); commit `feat(ai): the AI feed tells /ai the person and the seeing pass (AI engine LLD §28.1)`.

### Task 6: the GAMES card on PLAY
**Files:** `web/ai.html` (a GAMES card under JOBS: the baby game; moments ready / playing / caught / holding still / locked (🔒 "needs the seeing test first"), design-notes §224; a grown-ups fold with escape distance, follow distance and minutes - the fixed lines shown, never settable: child-play 25 cm/s, never within 30 cm; "📜 how it works ›" opening lesson 6 on `/logo`), `web/ai.js` (pure helpers, tested: `gameMoment(gameEv, feedStatus)`, `babyLine(escape, follow, minutes)` - the lesson's text with the call line replaced, run through the existing `runLine`), `web/test/ai.test.js`, `tools/pack_web` budget check (34 KB).
- [ ] **Failing tests (Node):** `gameMoment` for each moment, locked without `people`, "holding still" on a held event until a STOP, ready again after the run ends; `babyLine` keeps the TO body and puts the three numbers in the call, clamped to the words' ranges.
- [ ] FAIL → implement → PASS; `/ai` packed under 34 KB; commit `feat(web): the GAMES card on PLAY (bridge LLD R34, design-notes §224)`.

### Task 7: docs as built, the review, the tank checks
- [ ] Tank Logo LLD §27.8 as built, bridge LLD R34.10, AI engine LLD §28.10, design-notes, CLAUDE.md plans row; commit, push. Opus final review of the branch (the Review Focus above); fixes with a failing test first, minors too.
- [ ] With the owner, after G1's stand checks, the floor cap measurement and the seeing test passed (the lock): flash over WiFi; on the stand `FACE "baby` refused 7061 without the pass and accepted with it; then on the floor with an adult crawling, STOP in hand: lesson 6 from the GAMES card - face, escape (sidestep), follow, caught at 30 cm with the beep, the pause, held when the engine is stopped.

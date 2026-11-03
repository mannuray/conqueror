# Plan Z2a — the AI words, the skill server and the drive lease Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Tank Logo program on the tank can say `AREA 2  FIND "car  SETXY 0 0`: the ESP32's runner asks the laptop's AI engine (the skill server) questions and hands it action words under a drive lease that STOP, the page, the owner or 0.3 s of silence end at once.

**Architecture:**
- **Tank Logo** gains seven words. Questions become a new effect `kAsk`, whose answer comes back through `kResume`. Action words become a new effect `kSkill`, with two new runner states, `kAsking` and `kSkill`. `AREA` is a runner setting, like `SPEED`.
- **The bridge** gains a pure `SkillHub` (registration, the list of things and names, one lease with its deadman) and a motion primitive `PrimKind::kSkill`. `LogoController` sends places relative to the tank, worked out from its odometry.
- **The AI engine** gains `cq-drive serve`: one `Perception` loop, `Questions` answered from the `World`, and one `SkillRunner` per action word with a `LeaseDriver`. It also adds `Names` (loose names), a `PersonGuard`, and a `RoamSkill` point mode with an area centre.

**Tech Stack:**
- Bridge: C++17 for the ESP32-S3 (Arduino / ESP-IDF), PlatformIO with host tests on `native` (Unity).
- Generator: Python 3.12 via uv (`logogen`).
- AI engine: Python 3.12 via uv, numpy, websockets, pytest, ruff, pyright strict.

**Spec:**
- `docs/superpowers/specs/2026-10-25-08-drive-while-thinking-design.md` Revision 2 §4.3–§4.4.
- `docs/lld/tank-logo-lld.md` §24.
- `docs/lld/esp32-bridge-lld.md` R26.
- `docs/lld/ai-engine-lld.md` §21.
- `docs/design-notes.md` §191, §196, §197.
- The `/ai` page is Plan Z2b, not here.

## Global Constraints

- **Safety:**
  - Nothing on the Uno changes.
  - Every skill frame goes out as `DRIVE` (source kAuto) through the safety supervisor.
  - The brake, stall, battery, tilt, link loss and child-play limits end a skill as they end a `GO`.
  - The engine never drives outside a lease.
  - The lease deadman is `kRateDeadmanMs` (300 ms).
  - Never weaken a safety rule to make a test pass.
- **Never a person:** `person` is never offered in `things` and never resolves. `PersonGuard` drops any non-person box overlapping a person box by ≥ 0.3 of its own area, before the tracker.
- **Words:** these exact names, levels, kinds, inputs and ranges, from Tank Logo LLD §24.1:

  | Word | Level / kind | Input | needs |
  |---|---|---|---|
  | `FIND` | level 2 command | thing | ai, encoders |
  | `WANDER` | level 2 command | seconds 1–600 | ai, encoders |
  | `EXPLORE` | level 2 command | — | ai, encoders |
  | `SETXY` | level 2 command | x cm, y cm, each −1000..1000 | ai, encoders |
  | `AREA` | level 2 command | metres 0–10 | ai |
  | `SEE?` | level 2 reporter | thing | ai |
  | `HOWFAR` | level 2 reporter | thing | ai |

- **Errors, verbatim:**
  - 7020 "{word} needs the AI helper (it runs on the laptop)."
  - 7050 "I couldn't find the {thing}."
  - 7051 "I lost sight of the {thing}."
  - 7052 "The AI helper stopped answering."
  - 7053 "I don't know what a {thing} is."
  - 7054 "I saw the {thing}, but it's outside the play area."
  - 7055 "I can't find a way there."
- **Wire:**
  - Messages exactly as bridge LLD R26.2: `hello ai-skills` with `things`, `skill_things`, `skill_ask` / `skill_answer`, `skill_start` (`find`+`thing`, `wander`+`s`, `explore`, `setxy`+`to{fwd_cm,left_cm}`, any + `area{r_m,fwd_cm,left_cm}`), `skill_drive`, `skill_done`, `skill_failed`, `skill_end`.
  - Failure reasons: `not_found`, `lost`, `outside_area`, `no_way`, `blocked`, `unknown_thing`, `stale`.
  - `skill_end` reasons: `stop`, `deadman`, `page`, `owner`, `ended`.
- **Bounds:** at most 48 words, at most 8 names each, at most 24 bytes per name, lower case. Over the bounds gets `nack TOO_BIG` and the old list is kept. No heap after start on the ESP32.
- **Timing:**
  - A question waits at most 300 ms; no answer gives FALSE / −1.
  - `SEE?` means seen within 0.5 s.
  - SETXY is done within 0.15 m; with no route for 5 s it ends `no_way`.
- **Generated files** (`firmware/esp32/src/logo/generated/`, `firmware/esp32/test/test_logo_conformance/cases.h`, `firmware/esp32/test/test_logo_running/cases.h`, `firmware/esp32/web/generated/`) are never edited by hand: change `logo/spec/*` or `logo/tests/*`, then run `cd logo && uv run logogen write`.
- **Checks before every commit:**
  - Bridge: `cd firmware/esp32 && pio test -e native` all pass; `pio run -e bridge` builds.
  - Logo: `cd logo && uv run pytest -q` and `uv run logogen check`.
  - AI engine: `cd ai-engine && uv run pytest -q`, `uv run ruff check .`, `uv run ruff format --check .`, `uv run pyright`. Run them **unpiped**; each must exit 0.
  - Engine rule: `domain/` never imports `app/` / `adapters/`, reads no clock, prints nothing.
- **Commits:** Conventional Commits ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Branch `plan-z2a` from `main`.
- **Hardware:** the board machine is `mannu@yantra.lan`, with the ESP32-S3 on `/dev/ttyACM0`. The protocol version is unchanged: the Uno is not reflashed. Before every drive, say what moves and confirm stand or floor. Floor runs only with the owner present and STOP in hand, after a camera check. Recordings and decision logs stay local and are deleted after use.

## Review Focus

1. **Threads.** The checker runs on the Logo worker's task, while `SkillHub` lives on the link task. Reading the things list while `skill_things` replaces it must never tear. The worker takes a copy under a lock when a check starts. Test in Task 4.
2. **Ordering.** A `skill_done` / `skill_failed` / `skill_answer` that arrives after the runner already moved on is dropped. Cases: a late answer, an end after STOP, an end with an old `id`. It never ends or resumes the next word. Test in Task 4.
3. **The server dies mid-skill.** The engine is killed with Ctrl-C or the WiFi drops while the tank drives. The tank must stop within 300 ms (the lease deadman), the program ends 7052, and a reconnecting engine must not get the old lease back. Tests in Task 5 and Task 9.
4. **Odometry signs.** `FORWARD 50` then `SETXY 0 0` must go back, not on. After `RIGHT 90` the place must be on the correct side (the tank's yaw is + = left; Logo's RIGHT is −). Test in Task 6.
5. **Unknown thing at run time.** A thing computed at run time (`FIND FIRST [toy ball]`, `FIND "dragon` built from a list) is checked by the engine. It ends 7053 with no drive frame sent. Test in Task 9.

---

### Task 1: The words and errors in `words.yaml`, and the generator's `thing` input

**Files:**
- Modify: `logo/spec/words.yaml` (the seven words, `SEE?` / `FIND` moved to level 2, the errors), `logo/generator/logogen/*` (the input type `thing`; the conformance key `tank: {ai: bool, things: {word: [names]}}`), `logo/tests/reading.yaml`, `logo/tests/errors.yaml`
- Generated (by `logogen write`): `firmware/esp32/src/logo/generated/words.h`, `firmware/esp32/test/test_logo_conformance/cases.h`, `firmware/esp32/web/generated/*`
- Test: `logo/tests/test_logogen.py`

**Interfaces:**
- Produces:
  - word table entries `FIND`, `WANDER`, `EXPLORE`, `SETXY`, `AREA`, `SEE?`, `HOWFAR`, with input type `thing` (a quoted word or a reporter giving a word);
  - error codes 7020 (reworded) and 7050–7055 in the generated error table;
  - conformance cases may carry `tank: {ai: true, things: {toy: [car, racing car]}}`.

- [ ] **Step 1: Write the failing tests:**
  - `test_logogen.py::test_ai_words_are_level_2` — each of the seven words is level 2, with the needs from Global Constraints;
  - `test_logogen.py::test_new_errors_verbatim` — 7050–7055 and 7020 texts;
  - `test_logogen.py::test_tank_things_key` — a case with `tank.things` emits a things table into `cases.h`.
  - In `reading.yaml`, add cases for: each word parsing with its inputs; `FIND` with no input → 7002; `SETXY 10` → 7002; `WANDER 0` → 7025; `AREA 11` → 7025; `DISTANCE` unchanged.
- [ ] **Step 2:** `cd logo && uv run pytest -q`. Expected: FAIL (the words are missing).
- [ ] **Step 3:** Edit `words.yaml` and the generator. The `does` / `gives` / `help` / `example` text comes from Tank Logo LLD §24.1.
- [ ] **Step 4:** `uv run pytest -q` passes. Then `uv run logogen write`, then `uv run logogen check` exits 0. Then `cd ../firmware/esp32 && pio test -e native -f test_logo_words` passes; the conformance cases for unknown things are expected to fail until Task 2 (note them in the ledger).
- [ ] **Step 5: Commit** `feat(logo): the AI words FIND WANDER EXPLORE SETXY AREA SEE? HOWFAR and errors 7050-7055 (Tank Logo LLD §24.1)`.

### Task 2: Loose names in the checker — `ThingNames` and `Caps::things`

**Files:**
- Create: `firmware/esp32/src/logo/thing_names.h`, `thing_names.cpp`
- Modify: `firmware/esp32/src/logo/checker.h` / `checker.cpp`
- Test: `firmware/esp32/test/test_logo_checker/` (new cases); the generated conformance cases from Task 1

**Interfaces:**
- Produces:
  - `namespace logo { class ThingNames { public: static constexpr uint8_t kMaxWords = 48, kMaxNames = 8, kMaxLen = 24; bool set(const char* const* words, const uint8_t* n_names, const char* const* names, uint8_t n_words); const char* resolve(const char* name) const; uint8_t size() const; void clear(); }; }`
    - `set` returns false and keeps the old list when over the bounds.
    - `resolve` lower-cases and squeezes spaces, and returns the word or nullptr.
    - Fixed arrays only.
  - `Caps` gains `const ThingNames* things = nullptr;`.
  - Checker rule: for a literal thing input, with `caps.ai` false → 7020 (as today); with `things` set and `resolve` null → 7053 with `{thing}` = the literal; a non-literal is not checked.

- [ ] **Step 1: Write the failing tests:**
  - `test_logo_checker`: `test_find_known_name_ok` (`FIND "Racing  Car` with `toy: [car, racing car]`); `test_find_unknown_is_7053`; `test_find_without_server_is_7020`; `test_see_and_howfar_resolve_too`; `test_computed_thing_not_checked` (`FIND FIRST [dragon]` passes the checker).
  - `test_thing_names`: `test_bounds_refused_old_kept` (49 words → false, the old list resolves); `test_name_over_24_bytes_refused`.
- [ ] **Step 2:** `pio test -e native -f test_logo_checker -f test_logo_conformance`. Expected: FAIL.
- [ ] **Step 3:** Implement `ThingNames` and the checker rule.
- [ ] **Step 4:** `pio test -e native` all pass.
- [ ] **Step 5: Commit** `feat(logo): loose names - ThingNames and the checker's 7053 / 7020 for things (Tank Logo LLD §24.1)`.

### Task 3: Running the words — `kAsk`, `kSkill`, `AREA`, the ends

**Files:**
- Modify: `firmware/esp32/src/logo/program.h` (`EffectKind::kAsk`, `kSkill`, `kArea`; `OpCode::kResume`), `compiler*.cpp`, `vm.h` / `vm.cpp`, `effects.h` / `effects.cpp` (`ActKind::kAsk`, `kSkill`; `Action` fields below), `run_controller.h` / `run_controller.cpp`, the preview simulator (`firmware/esp32/src/logo/` sim code tested by `test_logo_sim`, drawn by `firmware/esp32/web/logo.js`), `logo/tests/running.yaml`
- Generated: `firmware/esp32/test/test_logo_running/cases.h`
- Test: `firmware/esp32/test/test_logo_run/`, `test_logo_vm/`, `test_logo_running/`, `test_logo_sim/`

**Interfaces:**
- Consumes: the Task 1 word table.
- Produces:
  - **`Action` fields:**
    - `enum class AskQ : uint8_t { kSee, kHowfar }; AskQ ask_q;`
    - `enum class SkillKind : uint8_t { kFind, kWander, kExplore, kSetxy }; SkillKind skill;`
    - `char thing[25]`, `uint16_t seconds`;
    - `int16_t x_cm, y_cm` (run frame, SETXY);
    - `bool has_area; uint8_t area_m; Pose area_at` (run frame).
  - **`RunState`** gains `kAsking`, `kSkill`.
  - **`RunController`:**
    - `RunOut onAskAnswer(int32_t value, uint32_t now)`: TRUE = 1 / FALSE = 0 for `SEE?`, cm for `HOWFAR`. With no answer by `m_ask_deadline` (300 ms), `tick` pushes FALSE / −1.
    - `enum class SkillEnd : uint8_t { kDone, kNotFound, kLost, kOutsideArea, kNoWay, kBlocked, kUnknownThing, kStale }`.
    - `RunOut onSkillEnd(SkillEnd e, uint32_t now)`, mapping each end to its code as in Tank Logo LLD §24.2's table. `kBlocked` behaves as a move's block (7024, or `m_blocked` for a `BLOCKED?` program).
    - Tank-side ends (STOP, page, owner, deadman, stall) stay on the existing `kByEndReason` path.
  - **`AREA n`** sets `m_area_m` and `m_area_at` (the odometry pose at that moment, from `tick`'s telemetry); `AREA 0` clears it. Every later `kSkill` action carries it.
  - **While `GO` runs,** a `kSkill` ends the GO first (as a motion word, §22.2).

- [ ] **Step 1: Write the failing tests** (running.yaml cases, with the fake World gaining `answers:` and `skill_ends:`):
  - `IF SEE? "ball [ BEEP ]` with true → beep; with no answer → no beep after 300 ms;
  - `SAY HOWFAR "ball` with 120 → says "120", with no answer → says "-1";
  - `FIND "ball  BEEP` with done → beep, finished; with not_found → 7050; with outside_area → 7054; with no_way on `SETXY 0 0` → 7055; with unknown_thing → 7053; with stale → 7052;
  - with blocked in a `BLOCKED?` program → goes on;
  - STOP during a skill → 7040;
  - `GO  WAIT UNTIL SEE? "ball  HOLD  FIND "ball` → asks each tick, then holds, then the skill;
  - `AREA 2  RIGHT 90  FORWARD 30  WANDER 10` → the action carries area 2 with `area_at` = the pose after `AREA` (0, 0);
  - `AREA 0` clears it;
  - **(Review Focus 2)** `test_logo_run::test_late_answer_and_old_end_dropped` — an `onAskAnswer` after the timeout and an `onSkillEnd` while not in `kSkill` change nothing.
- [ ] **Step 2:** `cd logo && uv run logogen write && cd ../firmware/esp32 && pio test -e native -f test_logo_run -f test_logo_vm -f test_logo_running`. Expected: FAIL.
- [ ] **Step 3:** Implement. Update the simulator preview:
  - `SETXY` draws a dashed straight line to the point;
  - after `FIND` / `WANDER` / `EXPLORE` the rest of the path is dashed;
  - `SEE?` is FALSE and `HOWFAR` is −1;
  - `AREA` draws its circle.
- [ ] **Step 4:** `pio test -e native` all pass; `pio run -e bridge` builds.
- [ ] **Step 5: Commit** `feat(logo): run the AI words - kAsk / kSkill, the Asking and Skill states, AREA, the skill ends (Tank Logo LLD §24.2)`.

### Task 4: `SkillHub` and the messages

**Files:**
- Create: `firmware/esp32/src/core/skill_hub.h`, `skill_hub.cpp`; `firmware/esp32/src/core/ws_json_skill.cpp`
- Modify: `core/ws_codec.h` / `ws_codec.cpp` / `ws_codec_keys.cpp` (the new message kinds and fields), `net/logo_worker.cpp` (`caps()` copies the things list under the worker's lock)
- Test: `firmware/esp32/test/test_skill_hub/` (new), `test_ws_codec/`

**Interfaces:**
- Consumes: `logo::ThingNames` (Task 2), `logo::Action` / `SkillKind` / `AskQ` (Task 3).
- Produces (`namespace core`):
  - `class SkillHub` with:
    - `Decision onHello(int fd, const ThingList& t)` (`kBusy` when another fd holds it);
    - `Decision onThings(int fd, const ThingList& t)` (`kTooBig`, old kept);
    - `int32_t onClose(int fd)` (the lease id it ended, or −1);
    - `int server() const` (−1 = none);
    - `const logo::ThingNames& things() const`;
    - `int32_t ask(logo::AskQ q, const char* thing, uint32_t now)`, returning the new id;
    - `bool start(int32_t id, uint32_t now)` (grants the lease to the server);
    - `bool answerFor(int fd, int32_t id) const` (true only for the pending ask's id);
    - `bool endFor(int fd, int32_t id) const`;
    - `bool leaseOk(int fd, int32_t id, uint32_t now)` (also refreshes `last_drive_ms`);
    - `int32_t tick(uint32_t now)` (a lease id whose 300 ms deadman ran out, else −1);
    - `void endLease()`.
  - `struct ThingList { uint8_t n; const char* word[48]; uint8_t n_names[48]; const char* names[48 * 8]; }` (views into the parsed frame).
  - **`WsCodec`:**
    - parses `hello` (with `client:"ai-skills"`, `things`), `skill_things`, `skill_answer`, `skill_drive`, `skill_done`, `skill_failed`;
    - writes `skill_ask`, `skill_start`, `skill_end`;
    - `nack BAD_FIELD` for a malformed field.
  - **`logo_worker caps()`:** `ai = hub.server() >= 0`, `things` = a copy in the worker (`static logo::ThingNames s_copy`), copied under the worker's mutex when a check starts; `SkillHub` updates take the same mutex.

- [ ] **Step 1: Write the failing tests:**
  - `test_skill_hub`: `test_one_server_only`; `test_things_set_replaced_too_big_kept`; `test_ask_answer_by_id`; `test_late_answer_dropped` (Review Focus 2); `test_end_with_old_id_dropped` (Review Focus 2); `test_lease_only_for_holder_and_id`; `test_lease_deadman_300ms`; `test_close_ends_lease`; `test_reconnect_does_not_get_old_lease` (Review Focus 3).
  - `test_ws_codec`: each new message parses / writes exactly as R26.2; bad fields → BAD_FIELD.
  - `test_logo_check` / worker: `test_caps_copy_not_torn` (Review Focus 1: replacing the list during a check leaves the check's copy whole — the copy is taken before the check runs).
- [ ] **Step 2:** `pio test -e native -f test_skill_hub -f test_ws_codec`. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `pio test -e native` all pass.
- [ ] **Step 5: Commit** `feat(bridge): SkillHub - the skill server, its things, the one lease and its deadman; the skill messages (bridge LLD R26.1-R26.3)`.

### Task 5: The drive under the lease — `PrimKind::kSkill`, `onSkillDrive`, the camera

**Files:**
- Modify: `firmware/esp32/src/core/motion_service.h` / `.cpp` (`PrimKind::kSkill`; `startSkill`, `skillDrive`, `endSkill`, `stepSkill`), `app/motion_controller.h` / `.cpp` (`onSkillDrive(fd, id, v, yaw_rate, now)`), `core/link_owner.h` / `.cpp` (`setLeaseHolder(int fd)` / `clearLeaseHolder()`; `onCamera` allows the holder)
- Test: `firmware/esp32/test/test_motion/`, `test_motion_controller/`, `test_link_owner/`

**Interfaces:**
- Consumes: `SkillHub::leaseOk` (Task 4).
- Produces:
  - **`MotionService`:**
    - `void startSkill(int32_t id, uint32_t now)`;
    - `void skillDrive(int8_t v_pct, float yaw_rate_dps, uint32_t now)`, held by the bridge's gyro loop as `drive_rate` (R4.4);
    - `void endSkill(core::BridgeReason why)`.
    - `kSkill` ends on brake → blocked, stall, link, or no `skillDrive` for 300 ms → deadman, through the same `MotionOut` ends as `kGo`.
  - **`MotionController::onSkillDrive`** refuses with `nack NOT_OWNER` once per stream when `leaseOk` is false.

- [ ] **Step 1: Write the failing tests:**
  - `test_motion`: `test_skill_sends_v_and_w`; `test_skill_brake_is_blocked`; `test_skill_deadman_300ms` (Review Focus 3); `test_stop_ends_skill`.
  - `test_motion_controller`: `test_skill_drive_without_lease_nacked_once`; `test_phone_stick_during_skill_ends_lease` (→ the program ends 7045).
  - `test_link_owner`: `test_lease_holder_cam_set_only_during_lease`.
- [ ] **Step 2:** `pio test -e native -f test_motion -f test_motion_controller -f test_link_owner`. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `pio test -e native` all pass; `pio run -e bridge` builds.
- [ ] **Step 5: Commit** `feat(bridge): PrimKind::kSkill - the skill server drives under the lease; the holder may move the camera (bridge LLD R26.1, R26.3)`.

### Task 6: `LogoController` — asks, skills, places relative to the tank, the wiring

**Files:**
- Modify: `firmware/esp32/src/app/logo_controller.h` / `.cpp` / `logo_actions.cpp` (new hooks; `ask`, `startSkill`, `onSkillAnswer`, `onSkillEnd`; the lease ended on STOP / page / owner / program end with `skill_end`), `app/command_router.cpp` (routes the new messages), `net/link_task.cpp` (owns `SkillHub`; `tick` feeds the deadman; a close ends the skill with 7052)
- Create: `firmware/esp32/src/logo/relative.h` (pure)
- Test: `firmware/esp32/test/test_logo_controller/`, `test_command_router/`, `test_logo_odometry/` (the relative helper)

**Interfaces:**
- Consumes: Tasks 3–5.
- Produces:
  - `namespace logo { struct FwdLeft { int16_t fwd_cm; int16_t left_cm; }; FwdLeft relative(const Pose& now, int32_t x_run_mm, int32_t y_run_mm); }`
    - The run frame is the odometry's: x right, y ahead of the start.
    - The result is in the tank's frame now: forward and left.
  - `LogoController::Hooks` gains `int32_t (*ask)(logo::AskQ, const char* thing, uint32_t now)`, `bool (*start_skill)(int fd, const logo::Action& a, logo::FwdLeft to, logo::FwdLeft area_at, uint32_t now)` and `void (*end_skill)(core::BridgeReason why, uint32_t now)`.
  - `LogoController::onSkillAnswer(int fd, int32_t id, int32_t value, uint32_t now)` and `onSkillEnd(int fd, int32_t id, logo::SkillEnd e, uint32_t now)`, which drop anything `SkillHub` says is not for the current id.

- [ ] **Step 1: Write the failing tests:**
  - `test_logo_odometry::test_relative_signs` (Review Focus 4): pose (0, 500 mm, 0°) with the run point (0, 0) → fwd −50, left 0. Pose (0, 0, heading −90° = turned right) with the run point (0, 300 mm) ahead of the start → fwd 0, left +30. With the run point (300 mm, 0) → fwd +30, left 0.
  - `test_logo_controller`:
    - `test_forward_then_setxy_back` (`FORWARD 50  SETXY 0 0` → `skill_start` `to.fwd_cm −50`);
    - `test_area_anchor_sent_relative`;
    - `test_ask_timeout_pushes_false`;
    - `test_stop_ends_lease_and_sends_skill_end_stop`;
    - `test_page_hidden_sends_skill_end_page`;
    - `test_server_close_mid_skill_is_7052` (Review Focus 3).
  - `test_command_router`: each new message reaches its controller.
- [ ] **Step 2:** `pio test -e native -f test_logo_controller -f test_logo_odometry -f test_command_router`. Expected: FAIL.
- [ ] **Step 3:** Implement and wire.
- [ ] **Step 4:** `pio test -e native` all pass; `pio run -e bridge` builds, and its size check passes.
- [ ] **Step 5: Commit** `feat(bridge): Logo asks and runs skills through SkillHub - places relative to the tank, the lease ended with skill_end (bridge LLD R26.4, Tank Logo LLD §24.2)`.

### Task 7: The engine's names and the person guard

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/names.py`, `ai-engine/src/conqueror_ai/domain/person_guard.py`
- Modify: `ai-engine/config/vocabulary.yaml`, `config/balls.yaml` (`names:`), `domain/vocabulary.py` (`Word.names: tuple[str, ...] = ()`), `adapters/detector_config.py`, `app/perception.py` (the guard before the tracker)
- Test: `ai-engine/tests/test_names.py`, `test_person_guard.py`, `test_perception.py`

**Interfaces:**
- Produces:
  - `class Names: __init__(vocab: Vocabulary, balls: Sequence[str])`;
    - `resolve(name: str) -> str | None` (lower case, spaces squeezed; `person` and its names → None);
    - `wire() -> list[dict[str, object]]` (`[{"word", "names"}]`, without `person`);
    - raises `ValueError` with a plain message when over 48 words, 8 names, or 24 bytes per name.
  - `OVERLAP = 0.3`; `def clear(dets: Sequence[Detection]) -> list[Detection]` in `person_guard.py`.
  - Starter names (owner may extend):
    - toy: `car`, `toy car`, `racing car`, `teddy`;
    - ball: `football`;
    - shoe: `trainer`, `slipper`;
    - bottle: `water bottle`;
    - box: `cardboard box`.

- [ ] **Step 1: Write the failing tests:**
  - `test_names`: `test_resolve_word_and_names`; `test_person_never_offered_or_resolved`; `test_bounds_refused`.
  - `test_person_guard`: `test_toy_inside_person_dropped`; `test_toy_beside_person_kept`; `test_person_kept`.
  - `test_perception::test_guard_runs_before_tracker` (a toy box over a person never becomes a listed thing).
- [ ] **Step 2:** `cd ai-engine && uv run pytest tests/test_names.py tests/test_person_guard.py -q`. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** All four engine checks (unpiped) exit 0.
- [ ] **Step 5: Commit** `feat(ai): Names - loose names for the AI words; PersonGuard - never a box over a person (AI engine LLD §21.4)`.

### Task 8: `RoamSkill` point mode, the area centre, the new results

**Files:**
- Modify: `ai-engine/src/conqueror_ai/domain/drive/roam.py` (`mode="point"`, `point`, `centre`), `domain/drive/skill.py` (`Result.OUTSIDE_AREA`, `NO_WAY`, `UNKNOWN_THING`; find's outside-area end becomes `OUTSIDE_AREA`), `app/drive.py` (the CLI prints the new results)
- Create: `ai-engine/src/conqueror_ai/domain/drive/places.py` (`to_map`)
- Test: `ai-engine/tests/test_drive_roam.py`, `test_drive_roam_sim.py`, `test_drive_places.py`

**Interfaces:**
- Produces:
  - `def to_map(x_m: float, y_m: float, heading_rad: float, fwd_cm: float, left_cm: float) -> tuple[float, float]` (the engine pose frame: heading 0 along +x, left = +y);
  - `RoamSkill(..., point: tuple[float, float] | None = None, centre: tuple[float, float] | None = None)`:
    - with `centre=None` the area is round the first pose, as today;
    - point mode is done within `POINT_DONE_M = 0.15`;
    - `NO_ROUTE_S = 5.0` without a route → `Result.NO_WAY`.

- [ ] **Step 1: Write the failing tests:**
  - `test_drive_places`: `test_to_map_headings` (0, 90°, −90°).
  - `test_drive_roam_sim`: `test_point_reached_free` (DONE within 0.15 m); `test_point_walled_off_is_no_way`; `test_centre_moves_the_area`; `test_find_outside_area_is_outside_area` (the §196 case now returns `OUTSIDE_AREA`).
- [ ] **Step 2:** run them. Expected: FAIL.
- [ ] **Step 3:** Implement. The point goal reuses `RoutePlanner` and the cost map exactly as wander's goals do; no new safety rule.
- [ ] **Step 4:** All four engine checks exit 0.
- [ ] **Step 5: Commit** `feat(ai): RoamSkill point mode and area centre; Result OUTSIDE_AREA / NO_WAY / UNKNOWN_THING (AI engine LLD §21.2)`.

### Task 9: `SkillLink`, `LeaseDriver`, `Questions`

**Files:**
- Create: `ai-engine/src/conqueror_ai/adapters/skill_link.py`, `ai-engine/src/conqueror_ai/adapters/lease_driver.py`, `ai-engine/src/conqueror_ai/app/questions.py`
- Modify: `ai-engine/tests/fake_bridge.py` (it speaks the skill messages)
- Test: `ai-engine/tests/test_skill_link.py`, `test_lease_driver.py`, `test_questions.py`

**Interfaces:**
- Consumes: `Driver` (domain/ports.py), `drive_msg` (adapters/drive_link.py), `Names.wire()` (Task 7).
- Produces:
  - **`SkillEvent`** (dataclasses, one per kind): `Ask(id, q, thing)`, `Start(id, skill, thing, seconds, to: tuple[float, float] | None, area: tuple[float, float, float] | None)`, `End(id, why)`, `Lost()`.
  - **`class SkillLink`:**
    - `__init__(url: str, things: list[dict[str, object]])`;
    - `async connect() -> None` (raises `SkillBusy` on `nack BUSY`);
    - `async events() -> AsyncIterator[SkillEvent]`;
    - `async answer(id: int, value: bool | int)`, `async done(id: int)`, `async failed(id: int, reason: str, thing: str)`;
    - `async drive(id: int, cmd: Command)`;
    - it answers pings.
  - **`class LeaseDriver(Driver)`:**
    - `__init__(link: SkillLink, id: int)`;
    - `end(why: str)` sets `refused`;
    - after `refused` is set, `drive` sends nothing;
    - `finish()` sends one `v 0` frame.
  - **`class Questions`:** `see(world: World, word: str, now_s: float) -> bool` (listed and seen within `SEEN_S = 0.5`); `howfar(world: World, word: str, now_s: float) -> int` (nearest, cm from the lens, −1).
  - **`def wire_reason(r: Result) -> str | None`** (in `lease_driver.py`): `None` for `DONE` and for `CANCELLED`-by-end; `REFUSED` / `FAILED` / `STALE` / `CANCELLED` → `stale`.

- [ ] **Step 1: Write the failing tests:**
  - `test_skill_link`: `test_hello_carries_things`; `test_busy_raises`; `test_events_parsed`; `test_pings_answered`.
  - `test_lease_driver`: `test_frames_carry_id`; `test_end_stops_frames` (Review Focus 3); `test_finish_once`; `test_wire_reason_per_result` (one assert per `Result`).
  - `test_questions`: `test_seen_within_half_second`; `test_howfar_nearest_or_minus_one`.
- [ ] **Step 2:** run them. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** All four engine checks exit 0.
- [ ] **Step 5: Commit** `feat(ai): SkillLink and LeaseDriver - the engine drives only under the bridge's lease; Questions for SEE? / HOWFAR (AI engine LLD §21.3, §21.5)`.

### Task 10: `cq-drive serve`

**Files:**
- Create: `ai-engine/src/conqueror_ai/app/serve.py` (`ServeApp`)
- Modify: `ai-engine/src/conqueror_ai/app/drive.py` (the `serve` sub-command → `ServeApp`; `--home`, `--record` shared)
- Test: `ai-engine/tests/test_serve.py` (with `fake_bridge.py` and fake perception)

**Interfaces:**
- Consumes: Tasks 7–9, `SkillRunner`, `World`, `Perception`, `start_home` / `end_home` (app/drive.py).
- Produces:
  - **`class ServeApp`:**
    - `__init__(link: SkillLink, perception, world: World, names: Names, home: str | None, record: Path | None)`;
    - `async run(stop: asyncio.Event) -> None`.
  - Behaviour:
    - one skill at a time (a second `Start` while driving → `failed stale`, no frames);
    - a `Start` with an unknown thing → `failed unknown_thing` with no frames;
    - the home is saved after each action word;
    - on `Lost` it stops sending, reconnects every 2 s and goes idle;
    - Ctrl-C saves the home.
  - CLI: `uv run cq-drive serve [--home NAME] [--record DIR] [--view]`.

- [ ] **Step 1: Write the failing tests:**
  - `test_serve`: `test_ask_answered`; `test_find_drives_then_done`; `test_unknown_thing_fails_without_frames` (Review Focus 5); `test_second_start_refused`; `test_end_from_bridge_stops_at_once`; `test_link_lost_mid_skill_reconnects_idle` (Review Focus 3); `test_home_saved_after_each_word` (tmp homes dir).
- [ ] **Step 2:** `uv run pytest tests/test_serve.py -q`. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** All four engine checks exit 0; `uv run cq-drive serve --help` prints its usage.
- [ ] **Step 5: Commit** `feat(ai): cq-drive serve - the skill server session (AI engine LLD §21)`.

### Task 11: Flash, the stand check, the floor checks (owner present, STOP in hand)

**Files:** none (results go into Task 12).

- [ ] **Step 1: Flash.** Flash the bridge from yantra (`pio run -e bridge`, upload over USB `/dev/ttyACM0`, or OTA as usual). Leave the Uno alone (protocol unchanged). Then check `/info` shows the new build.
- [ ] **Step 2: Stand check (tank on a stand, tracks off the ground; say what moves first).**
  1. `cq-drive serve` connects; `/logo` checks `FIND "car` fine and `FIND "dragon` as 7053; with the engine stopped, the check gives 7020.
  2. `IF SEE? "ball [ BEEP ]` with the ball in view → beep.
  3. `WANDER 10`: the tracks turn; STOP from the phone stops them at once and the program ends 7040.
  4. Kill the engine mid-`WANDER 20`: the tracks stop within 0.3 s, and the program ends 7052.
  5. The phone stick during a skill → 7045.
- [ ] **Step 3: Floor checks (owner present, STOP in hand, camera check first; say what moves).**
  1. `AREA 2  FIND "ball  SOUND FANFARE` from the home mark → done, ~30 cm from the ball.
  2. `WANDER 30` → no touch.
  3. `FORWARD 50  RIGHT 90  FORWARD 30  SETXY 0 0` → back within ~15 cm of the start.
  4. `FIND "car` with the toy car outside a 1 m area (`AREA 1`) → 7054.
  5. STOP during each.
- [ ] **Step 4:** Delete pictures and recordings after use.

### Task 12: As built

**Files:**
- Modify: `docs/design-notes.md` (a new § for the Z2a floor results), `docs/lld/ai-engine-lld.md` §21 (§21.8 as built), `docs/lld/tank-logo-lld.md` §24 and `docs/lld/esp32-bridge-lld.md` R26 (an as-built note each), `CLAUDE.md` (the plans list and the execution order)

- [ ] **Step 1:** Write the results with how each number was measured, every ruling from the ledger, and the differences from the LLDs.
- [ ] **Step 2: Commit and push** `docs: Plan Z2a floor checks and as built (design-notes §…, LLD §21.8)`.

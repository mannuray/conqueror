# Plan G1 — game mode on the bridge (Spec 10's safety under every program) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** a Tank Logo program the bridge's checker marks as a game runs under game mode: the child-play envelope armed before the first motion, every DRIVE checked against the engine's person estimate (30 cm keep-out, never towards, hold when unseen), caught = everything stops + a beep, the engine lost = held until STOP - with no engine game skills yet (they are Plan G2).

**Architecture:** pure `core::GameMode` (states) and `core::KeepOut` (the per-move rule) on the host-tested pattern of `SkillPushWatch`; `BodyLink::sendDrive` consults a `DriveGate` (the one exit every DRIVE takes - the stick and the motion service), `MotionController` ends a primitive "kept away" when the gate says so; `SkillHub` carries the engine's `people` flag and `person` stream and sends `game on/off`; the checker sets `CheckResult::game` (here from `CAUGHT?`, the one game word that needs no engine skill; Plan G3 adds FACE / ESCAPE / FOLLOW and person inputs).

**Tech Stack:** C++17 on the ESP32 bridge (PlatformIO, native Unity tests, `-Werror`, the pre-commit size check: files ≤ 400 lines, CCN ≤ 10); `logo/spec/words.yaml` + `logogen write`; Python smoke tools in `tools/smoke/`.

**Spec:** bridge LLD R34 (`docs/lld/esp32-bridge-lld.md`, approved 2026-11-09), Tank Logo LLD §27 (the game flag, errors), Spec 10 (`docs/superpowers/specs/2026-11-08-10-people-games-design.md`) S1-S12, design-notes §223-§225. Branch `plan-g1-game-mode` (worktree `scratchpad/plan-g1-game-mode`, from `main` after the P0 merge).

## Global Constraints
- **No Uno change** (654 B flash left): the Uno's `Envelope` (speed cap %, accel cap, CHILD_PLAY), stall event, obstacle brake are reused as they are.
- **Numbers (R34, owner 2026-11-09):** caught at `d_cm ≤ 30`; release when seen beyond **50 cm for 2 s**; a person estimate older than **300 ms** → output held at 0; older than **1 s** or the engine gone → **Held** (until STOP); forward motion with the person within **±60°** of ahead allowed only while `d_cm − stopping_cm(v) − 5 > 30`; turns only while `d_cm > 35`; the person **unseen for 1 s** → no forward motion and no turn (FACE's look-round is G2); arming waits **≤ 500 ms** for the ENVELOPE ack; a game run's time limit **15 minutes**.
- **The envelope:** `ENVELOPE {speed_cap: game_cap_pct, accel_cap: 0, lock_brake: CHILD_PLAY}` on arming (accel 0: the Uno's ramp brakes at twice its accel cap - a cap would slow every stop); `{100, 0, 0}` on Off; re-sent on an Uno HELLO during a game; cleared once at bridge boot when the Uno link is up.
- **`game_cap_pct`:** a bridge NVS setting, **0 = not measured** → a game run refuses to start, **7060**; measured on the floor in Task 6 (the track % that gives ~25 cm/s, from the encoders with `counts_per_m` 7514 - stated how).
- **Errors (`words.yaml`):** 7060 "The game can't start - the tank's speed limit isn't set.", 7061 "The game needs the seeing test first.", (7062 comes with Plan G3); caught is not an error; a keep-out cut ends a word **normally** ("kept away").
- **The game flag comes only from the bridge's own checker** (never from the page): `CheckResult::game` → `Program::game` → the run.
- **Generated files are never hand-edited:** change `logo/spec/words.yaml`, run `cd logo && uv run logogen write`.
- Commits: Conventional Commits ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus
1. A non-game program (no `CAUGHT?`) must behave exactly as today: no envelope, no gate, no `game` message - the whole Logo and motion test suites unchanged.
2. The stick during Held (a phone driving after the run ended 7052) gets zeros at `sendDrive`, not the stick's values; STOP still works (it is not a DRIVE).
3. An Uno reboot mid-game (HELLO): no DRIVE leaves before the envelope is acked again.
4. A person estimate jumping from 80 cm to unseen to 25 cm within one tick: the order of `onPerson` and `gate` must not let one forward DRIVE out at 25 cm.
5. STOP pressed while Arming (waiting for the ack): Off, the envelope cleared, nothing moves.

---

### Task 1: `GameMode` and `KeepOut` (pure, host-tested)
**Files:** create `firmware/esp32/src/core/game_mode.h/.cpp`, `firmware/esp32/src/core/keep_out.h/.cpp`; test `firmware/esp32/test/test_game_mode/test_game_mode.cpp`.

**Interfaces - Produces:**
- `struct core::PersonEst { bool seen; int16_t d_cm; int16_t bearing_deg; int8_t coming; uint8_t sure; uint32_t at_ms; };`
- `enum class core::GameState : uint8_t { kOff, kArming, kPlaying, kCaught, kHeld };`
- `enum class core::Gate : uint8_t { kAllow, kHoldZero, kEndKeptAway };`
- `struct core::Move { int8_t v_pct; float yaw_rate_dps; bool turn_only; };`
- `class core::KeepOut { static Gate check(const PersonEst& e, const Move& m, uint16_t stop_cm, uint32_t unseen_ms); };`
- `class core::GameMode` - `GameState state() const; enum class Start { kGo, kNoCap, kNoPeople }; Start startRun(bool game, bool people, uint8_t cap_pct, uint32_t now); void onEnvelopeAck(uint32_t now); void onPerson(const PersonEst& e, uint32_t now); void onCaughtEvent(uint32_t now); void onEngineGone(uint32_t now); void onStop(); void onRunEnd(); bool onUnoHello(); /* true: re-send the envelope */ Gate gate(const Move& m, uint16_t stop_cm, uint32_t now); bool caught() const; uint16_t caughtCount() const; bool wantsEnvelope(bool& on) /* one-shot: the envelope to send now */; bool justCaught() /* one-shot: beep + end every primitive */; Start lastRefusal() const /* why arming ended Off: kNoCap for no ack */; void step(uint32_t now) /* timeouts */;`

- [ ] **Step 1: failing tests** (Unity, `test_game_mode.cpp`), each named for its rule:
  - `test_a_non_game_run_never_arms`: `startRun(false, …)` → `kGo`, state `kOff`, `gate()` always `kAllow`, `wantsEnvelope` false.
  - `test_no_cap_or_no_people_refuses`: `startRun(true, true, 0, t)` → `kNoCap`; `startRun(true, false, 75, t)` → `kNoPeople`; state `kOff`.
  - `test_arming_holds_until_the_envelope_ack`: `startRun(true, true, 75, 0)` → `kGo`, `kArming`, `wantsEnvelope(on)` true with on; `gate(forward)` = `kHoldZero` until `onEnvelopeAck(100)` → `kPlaying`; no ack by 500 ms → `step(501)` gives `kOff` and a refusal the run reads as 7060 (`lastRefusal() == Start::kNoCap`).
  - `test_caught_at_30_and_released_after_50_for_2_s`: playing, `onPerson({seen,30,…})` → `kCaught`, `justCaught()` true once; persons at 45 cm keep caught; at 55 cm from t=1000 → still caught at 2999, `kPlaying` at 3000; at 55 then 48 at 2000 restarts the 2 s.
  - `test_stall_or_pushing_not_moving_is_caught`: `onCaughtEvent` → `kCaught`.
  - `test_stale_300_ms_holds_1_s_is_held`: last person at t=0; `gate` at 299 `kAllow` (if far), at 301 `kHoldZero`; `step(1001)` → `kHeld`; `onPerson` after Held does not leave it; `onStop()` → `kOff`, `wantsEnvelope(on)` with on false.
  - `test_engine_gone_is_held`: `onEngineGone` in Playing / Caught / Arming → `kHeld`.
  - `test_uno_hello_rearms`: playing; `onUnoHello()` true, state `kArming`, gate holds until the ack.
  - `test_review_focus_4_person_then_gate_same_tick`: playing at 80 cm; `onPerson(25 cm)` then `gate(forward)` in the same ms → not `kAllow`.
  - `test_stop_while_arming`: arming, `onStop()` → `kOff`, the clearing envelope wanted.
  - `KeepOut`: `test_forward_towards_ends_kept_away` (d 60, bearing 0, v 30, stop_cm 20 → 60−20−5 = 35 > 30 allow; d 50 → `kEndKeptAway`); `test_forward_away_or_aside_allowed` (bearing 90 → allow at d 40); `test_turns_from_35_cm` (turn_only at d 36 allow, d 35 hold); `test_unseen_1_s_holds_forward_and_turns` (seen false, unseen_ms 1001 → hold for forward and turn; backward allowed - backing is checked by `back_ok`, Task 5); `test_backward_is_never_cut_by_bearing`.
- [ ] **Step 2:** `pio test -e native -f test_game_mode` → FAIL (no files). **Step 3:** implement (states per R34.4; every timeout in `step(now)`; `gate()` returns `kHoldZero` in Arming / Caught / Held and when stale, else `KeepOut::check`). **Step 4:** PASS; `pio test -e native` all; the size check.
- [ ] **Step 5: Commit** `feat(bridge): game mode and the keep-out, pure (bridge LLD R34)`.

### Task 2: the one exit - `BodyLink` gate and the envelope
**Files:** `core/body_link.h/.cpp`, `core/acked_ops.h` (`kEnvelope`: `a` speed cap, `r` accel cap, `g` lock bits), `app/uno_channel.cpp`; tests `test/test_body_link`, `test/test_uno_channel`.

**Interfaces - Produces:** `class core::DriveGate { public: virtual ~DriveGate() = default; virtual bool allows(int8_t v, int8_t w, proto::Source src) = 0; };` (the bridge is not the Uno: `virtual` is allowed here as in the other ports); `BodyLink::setDriveGate(DriveGate*)`; `sendDrive` sends `0, 0` instead when the gate refuses (the stream stays alive, the Uno's deadman never fires from it); `bool BodyLink::sendEnvelope(uint8_t speed_cap, uint8_t accel_cap, uint8_t lock_bits, uint32_t now, uint8_t& seq)`; `UnoChannel::queueEnvelope(...)` as the other acked ops.
- [ ] **Step 1: failing tests:** `test_sendDrive_sends_zeros_when_the_gate_refuses` (a fake gate refusing → the frame on the wire has v 0, w 0, for both `kHuman` and `kAuto`); `test_no_gate_sends_as_today`; `test_envelope_is_an_acked_frame` (bytes of `EnvelopeMsg {75, 0, 2}`, the ack resolves the op); `test_stop_is_never_gated` (STOP goes out whatever the gate says).
- [ ] **Step 2:** FAIL. **Step 3:** implement. **Step 4:** PASS; all native tests; size check.
- [ ] **Step 5: Commit** `feat(bridge): every DRIVE through one gate; the ENVELOPE frame (bridge LLD R34.2)`.

### Task 3: the game flag, `CAUGHT?`, the errors, 15 minutes
**Files:** `logo/spec/words.yaml` (`CAUGHT?`: level 3, `kind: reporter`, `group: sense`, `needs: []`, "TRUE while the person has caught the tank"; errors 7060 / 7061; `game_run_time_s: 900`), generated outputs via `logogen write`; `firmware/esp32/src/logo/checker.h/.cpp` (`CheckResult::game`), `checker_launch.cpp` (LAUNCH parts too), `compiler.h/.cpp` + `program.h/.cpp` (`Program::game`), `check_job.cpp`, `run_parts.cpp` (the limit), `vm_*`/`effects` (`CAUGHT?` reads a `GameView` the run is given; false outside a game); tests `test_logo_checker`, `test_logo_run`, conformance / running cases in `logo/spec/tests`.

**Interfaces - Consumes:** `core::GameMode::caught()` (via a small `logo::GameView { virtual bool caught() const = 0; }` the controller passes - Task 5). **Produces:** `CheckResult::game`, `Program::game`, the generated `gen::kGameRunTimeS = 900`.
- [ ] **Step 1: failing tests:** checker `game` true for `CAUGHT?` in the main part, in a `TO` body never called, in a `LAUNCH` part; false without it; conformance case: `PRINT CAUGHT?` checks (level 3); running cases: a game run's limit 900 s (7041's text shows 15 minutes for a game - the builder words it from `words.yaml`), a non-game run keeps 300 s; `CAUGHT?` FALSE when the `GameView` says not caught, TRUE when it does.
- [ ] **Step 2:** FAIL. **Step 3:** `words.yaml`, `logogen write`, then the C++. **Step 4:** PASS; `logogen check`; all native tests; the conformance and running suites.
- [ ] **Step 5: Commit** `feat(logo): the game flag, CAUGHT?, 7060 / 7061, a game run's 15 minutes (Tank Logo LLD §27)`.

### Task 4: `SkillHub` - `people`, the person stream, `game on/off`, `back_ok`, `caught`
**Files:** `core/skill_hub.h/.cpp` (+ a split file if the size check asks); test `test/test_skill_hub`.

**Interfaces - Produces:** `bool SkillHub::people() const` (the engine's hello `"people":true`; false when absent or no engine); `bool SkillHub::takePerson(core::PersonEst& out)` (parsed from `{"t":"person","seen","d_cm","bearing","coming","sure","age_ms"}`, `at_ms = now − age_ms`); `void SkillHub::sendGame(bool on)`; `bool SkillHub::askBackOk(int16_t cm, int16_t arc_r_cm, int32_t& id)` / answers through the existing `skill_answer` path; `skill_end` with `"why":"caught"`.
- [ ] **Step 1: failing tests:** `test_hello_people_flag` (true / false / missing → false); `test_person_message_parsed_and_bounded` (d_cm −1..1000, bearing −180..180, a bad field → `nack BAD_FIELD`, nothing taken); `test_game_on_off_sent_to_the_engine`; `test_back_ok_question_and_answer` (a late answer after 300 ms dropped → treated as no); `test_skill_end_caught`.
- [ ] **Step 2:** FAIL. **Step 3:** implement. **Step 4:** PASS; all native; size check.
- [ ] **Step 5: Commit** `feat(bridge): the engine's people flag, its person stream, game on/off and back_ok (bridge LLD R34.3)`.

### Task 5: wiring - the run, the motion, caught, held, the pages, the cap setting
**Files:** `app/logo_controller.*`, `logo/run_controller.*` (start refusals 7060 / 7061, `GameView`), `app/motion_controller.*` (`gate()` per DRIVE: `kEndKeptAway` ends the primitive normally; backward primitives in a game ask `back_ok` first - no / timeout → end normally without moving), `app/drive_controller.*` (nothing but the `BodyLink` gate), `app/uno_event_router.*` (stall → `onCaughtEvent`), the slip watch (R21) → `onCaughtEvent`, `app/settings_controller.*` (`game_cap_pct` 0..100, NVS, `{"t":"set","game_cap_pct":n}` / read), `app/telemetry_publisher.*` (`{"t":"ev","kind":"game","state","d_cm","caught"}`), boot: the clearing envelope once the Uno link is up; a `GameDriveGate : core::DriveGate` adapter over `GameMode`; tests `test_logo_controller`, `test_motion_controller`, `test_settings_controller`, `test_uno_event_router`, `test_telemetry_publisher`.
- [ ] **Step 1: failing tests:** a game run with `game_cap_pct` 0 → 7060, without `people` → 7061, nothing sent to the Uno; a game run arms (ENVELOPE `{cap, 0, CHILD_PLAY}` queued, `game on` sent) and its first `FORWARD` waits for the ack; caught → every primitive ends, `SOUND` queued once, `ev game caught`, the program continues and `CAUGHT?` reads TRUE; Held → the run ends 7052, the stick gets zeros, STOP → Off, the clearing envelope and `game off`; a `FORWARD` towards the person at 50 cm ends normally (no 7024) and the next line runs; `BACK 20` in a game with `back_ok` no → ends without a DRIVE, the next line runs; yes → runs; the settings round-trip of `game_cap_pct`; a non-game run: no envelope, no `game` message, every existing controller test unchanged (Review Focus 1).
- [ ] **Step 2:** FAIL. **Step 3:** implement. **Step 4:** PASS; **all** native tests; `pio run -e bridge` (size); the size check.
- [ ] **Step 5: Commit** `feat(bridge): game mode wired - arming, the keep-out on every DRIVE, caught, held, the cap setting (bridge LLD R34)`.

### Task 6: tools, the stand checks with a stand-in engine, the cap on the floor
**Files:** create `tools/smoke/fake_person.py` (a stand-in skill server: hello with `people` true / false, streams `person` estimates from a script: `--at 80`, `--approach 80:25:3s`, `--stop-after 5s`, `--unseen`), `tools/smoke/game_cap.py` (floor: runs the tracks straight at 60..90 % in 5 % steps for 2 s each through `drive_rate`, reads the encoders, `v = Δcounts / Δt / counts_per_m`, prints the table, picks the largest % with v ≤ 25 cm/s, saves `game_cap_pct` - asks before it saves).
- [ ] **Stand (tracks off the ground; say what moves before each):** flash over WiFi; with `fake_person.py --at 80` and `game_cap_pct` set to a stand value (e.g. 60), run `REPEAT 20 [ IF CAUGHT? [ WAIT 1 ] FORWARD 10 ]`: the envelope armed (`ts` telemetry env_speed_cap), tracks turn; `--approach 80:25:3s`: the tracks stop at 30 cm, one beep, `ev game caught`; the fake person back at 60 for 2 s: tracks resume; `--stop-after`: Held within ~1 s, the phone's stick does nothing, STOP releases, envelope back to 100; `people` false → 7061; `game_cap_pct` 0 → 7060.
- [ ] **Floor (owner present; the tank drives straight ~2-3 m per step):** `game_cap.py` measures and saves `game_cap_pct` (report the table and how the % was picked).
- [ ] **Commit** `feat(tools): fake_person and game_cap for game mode's checks (bridge LLD R34)`.

### Task 7: docs as built, the review
- [ ] Bridge LLD R34.9 as built (names, the measured `game_cap_pct` and how, the stand results), Tank Logo LLD §27.7 (the flag and `CAUGHT?` as built), design-notes §226, CLAUDE.md plans row; commit, push.
- [ ] Opus final review of the branch (the Review Focus above), fixes with a failing test first, then the owner decides the merge.

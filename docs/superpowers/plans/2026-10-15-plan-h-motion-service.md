# Plan H — Bridge motion service (TURN / DRIVE_RATE) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (owner: native) task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** On the Uno Lite tank, `turn` (by / to, spin) and `drive_rate` work from any WebSocket client: the ESP32 bridge runs them on the gyro telemetry and sends ordinary DRIVE frames; the Uno sends TELEM_FAST at 50 Hz.

**Architecture:** New host-tested core classes on the bridge — `MotionRouter`, `TurnController`, `RateLoop`, `MotionService` — plus codec / JSON additions; `net/link_task.cpp` wires them in. Uno: one scheduler constant in both firmwares. Phone page: MOVES removed. No body-link protocol change.

**Tech Stack:** ESP32-S3 Arduino/ESP-IDF (PlatformIO `bridge`, host tests `native`, Unity), Uno AVR (PlatformIO `uno` / `mega`), vanilla JS page.

**Spec:** spec 02 Part E (E1–E5), spec 01 §4 / §6.1 / §8; LLD: `docs/lld/esp32-bridge-lld.md` R4 (R4.1–R4.9), `docs/lld/uno-lite-lld.md` §18.

**Branch:** `plan-h-motion` from `main` (5cbfb36 or later). Merge to `main` only after the owner's OK.

## Global Constraints
- Safety unchanged: the bridge only sends `DRIVE {src, v, w}` / `STOP`; every Uno rule (deadman 300 ms, obstacle brake, caps, lift, battery, stall) still applies. Never weaken a safety feature to pass a test.
- `turn`: `by` ±1…720 ° **or** `to` 0…359 °, `speed` 10…100 % (default 40), `radius_cm` > 0 → `nack UNAVAILABLE` on a body without `kMove` (needs encoders). `drive_rate`: `v` −100…100, `yaw_rate` −180…180 °/s (+ = CCW / left). `cancel` always acked.
- TURN done: |e| ≤ 1.5° and |rate| < 5 °/s for 200 ms; timeout |angle| / 20 °/s + 3 s; stalled: |w| ≥ w_min and |rate| < 2 °/s for 1.5 s; Kp 1.0 %/°, w_min 12 %, latency compensation 80 ms. DRIVE_RATE: ff 0.45 %/(°/s), Kp 0.3, Ki 0.6 /s, |w| ≤ 100 with anti-windup; stream gap 300 ms → STOP.
- Gains are constants in `src/core/motion_config.h` (floor tuning), not settings.
- Uno: TELEM_FAST 20 ms (50 Hz) in both firmwares; Uno Lite size gates (flash ≤ 30,720 B, RAM ≤ 1,536 B) still pass. Generated files untouched.
- Page: MOVES button and sheet removed; `tf` to clients stays 10 Hz (every 5th TELEM_FAST).
- Commits: Conventional Commits + `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Never print or commit `secrets.h`.

## Review Focus
1. A primitive must always end with `w = 0` then STOP — on done, cancel, STOP from anyone, owner change, owner socket close, telemetry gap, timeout, stall, OTA. → `test_turn_every_end_sends_zero_then_stop` (Task 5).
2. Yaw wrap: `to` across 0°/360° takes the short way; `by 360` / `by -720` keep direction and size. → `test_turn_target_wrap` (Task 3), simulation tests (Task 5).
3. A non-owner's `turn` / `drive_rate` never moves the tank; IR takeover mid-turn ends it with `not_owner`. → link_owner path + `test_service_owner_loss_ends` (Task 5) + hardware check (Task 7).
4. `drive_rate` stream stopping (phone sleeps) → STOP within 300 ms. → `test_rate_deadman` (Task 5).
5. Uno at 50 Hz: frames still parsed, `tf` stays 10 Hz to the page, nothing in the Uno's timing breaks (IMU 200 Hz, control 100 Hz). → bench check in Task 7 (frames_rx rate, page latency).

---

### Task 1: Uno TELEM_FAST at 50 Hz; bridge `tf` decimation 1 in 5
**Files:** `firmware/uno/src/lite/lite_app.cpp` (scheduler row `telemetryFastDue`), `firmware/uno/src/main.cpp` (full firmware row), `firmware/esp32/src/net/link_task.cpp` (`(s.fast_count & 1u) == 0` → `s.fast_count % 5u == 0`, comment "Uno 50 Hz -> page 10 Hz").
- [ ] Step 1: change both scheduler rows `50000UL` → `20000UL` for `telemetryFastDue` (check the phase column does not collide with `controlTask` / `imuTask`; keep the existing phase).
- [ ] Step 2: `~/.platformio/penv/bin/pio test -e native` (Uno) → all pass; `pio run -e uno` → size gate passes; `pio run -e mega` ok.
- [ ] Step 3: bridge decimation change; `pio test -e native` + `pio run -e bridge`.
- [ ] Step 4: commit `feat(uno): TELEM_FAST at 50 Hz for the bridge motion service; bridge forwards every 5th as tf`.

### Task 2: Codec — `turn`, `drive_rate`, `cancel`
**Files:** `src/core/ws_codec.h/.cpp`, `test/test_ws_codec/test_main.cpp`.
**Produces:** `WsCmdKind::kTurn, kDriveRate, kCancel`; `WsCommand` fields `int32_t turn_by_cdeg` (±72000 does not fit 16 bits), `bool turn_has_to`, `uint16_t turn_to_cdeg`, `uint8_t speed = 40`, `uint16_t radius_cm`, `int16_t rate_ddps` (0.1 °/s); `v` reused for drive_rate.
- [ ] Step 1: failing tests:
```cpp
void test_turn_by_and_to() {
  WsCommand c = P("{\"t\":\"turn\",\"by\":90,\"speed\":40,\"id\":5}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kTurn), K(c.kind));
  TEST_ASSERT_FALSE(c.turn_has_to);
  TEST_ASSERT_EQUAL_INT32(9000, c.turn_by_cdeg);
  TEST_ASSERT_EQUAL_UINT8(40, c.speed);
  c = P("{\"t\":\"turn\",\"to\":270}");
  TEST_ASSERT_TRUE(c.turn_has_to);
  TEST_ASSERT_EQUAL_UINT16(27000, c.turn_to_cdeg);
  TEST_ASSERT_EQUAL_UINT8(40, c.speed);  // default
  TEST_ASSERT_EQUAL_INT32(-72000, P("{\"t\":\"turn\",\"by\":-720}").turn_by_cdeg);
}
void test_turn_invalid() {
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\"}").kind));                    // neither
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"by\":90,\"to\":10}").kind));  // both
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"by\":721}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"to\":360}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"turn\",\"by\":90,\"speed\":5}").kind));
}
void test_drive_rate_and_cancel() {
  const WsCommand r = P("{\"t\":\"drive_rate\",\"v\":30,\"yaw_rate\":-45.5}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kDriveRate), K(r.kind));
  TEST_ASSERT_EQUAL_INT8(30, r.v);
  TEST_ASSERT_EQUAL_INT16(-455, r.rate_ddps);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"drive_rate\",\"v\":30,\"yaw_rate\":200}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCancel), K(P("{\"t\":\"cancel\",\"id\":3}").kind));
}
```
- [ ] Step 2: run `pio test -e native -f test_ws_codec` → FAIL. Step 3: implement (the existing number reader clamps to ±100 for drive fields — add an unclamped reader for `by` / `to` / `speed` / `radius_cm` / `yaw_rate` with explicit range checks). Step 4: PASS + whole suite. Step 5: commit `feat(esp32): parse turn, drive_rate and cancel`.

### Task 3: `MotionRouter`, `TurnController`, effective caps in `welcome`
**Files:** create `src/core/motion_config.h`, `src/core/motion_router.h/.cpp`, `src/core/turn_controller.h/.cpp`; modify `src/core/ws_json.cpp` (`jsonWelcome` caps: `turn` = body kTurn **or** local, `drive_rate` = body kDriveRate **or** local, `move` = body kMove); tests `test/test_motion/test_main.cpp` (new), `test/test_ws_json`.
**Produces:** `enum class Route {kForward, kLocal, kUnavailable}`; `Route routeTurn(uint8_t caps, uint16_t radius_cm)`, `Route routeDriveRate(uint8_t caps)`, `Route routeMove(uint8_t caps)`; `TurnController::begin(int32_t start_cdeg, int32_t target_cdeg, uint8_t speed_pct, uint32_t now)`, `TurnStep step(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now)` with `TurnStep {int8_t w; bool done; bool timeout; bool stalled;}`; `int32_t turnTarget(uint16_t yaw_cdeg, bool has_to, int32_t by_cdeg, uint16_t to_cdeg)` (unwrapped target, `to` = shortest way).
- [ ] Step 1: failing tests — router table (Lite caps = AUDIO_LED only → turn Local, drive_rate Local, move Unavailable, radius > 0 Unavailable; caps with kTurn → Forward); `test_turn_target_wrap` (yaw 350°, `to` 10° → +20°; yaw 10°, `to` 350° → −20°; `by` 360 → +360°); `TurnController` open-loop facts: far error → w = speed; near error → ≥ w_min; inside the window 200 ms → done; no motion for 1.5 s → stalled; timeout; welcome caps on Lite: `"turn":true,"drive_rate":true,"move":false`.
- [ ] Step 2: FAIL. Step 3: implement (constants from LLD R4.3 in `motion_config.h`). Step 4: PASS + suite. Step 5: commit `feat(esp32): motion router and turn controller`.

### Task 4: `RateLoop`
**Files:** `src/core/rate_loop.h/.cpp`; tests in `test/test_motion`.
**Produces:** `RateLoop::reset()`, `int8_t step(int16_t target_ddps, int16_t rate_ddps, uint16_t dt_ms)`.
- [ ] Step 1: failing tests — feed-forward only at zero error (target 90 °/s → w ≈ 40); integral grows on a steady shortfall and stops while saturated (anti-windup); sign change of the target resets the integral; output within ±100.
- [ ] Steps 2–5 as above; commit `feat(esp32): drive_rate loop`.

### Task 5: `MotionService` (state, ends, simulation)
**Files:** `src/core/motion_service.h/.cpp`; tests in `test/test_motion` with a small tank model in `test/support/tank_model.h` (first-order turn response: rate → w × 2.2 °/s per % with τ 150 ms, 80 ms telemetry delay, ±3 °/s noise, seeded).
**Produces:** `MotionService::startTurn(int fd, int32_t target_cdeg, uint8_t speed, uint32_t now, int32_t id)`, `driveRate(int fd, int8_t v, int16_t rate_ddps, uint32_t now)`, `cancel(Reason)`, `MotionOut onTelemetry(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now)`, `MotionOut tick(uint32_t now)` (watchdog / deadman / timeout without telemetry), `bool active()`, `int ownerFd()`, `uint8_t progressPct()`; `MotionOut {bool send; int8_t v; int8_t w; bool ended; PrimitiveEnd end;}`; `PrimitiveEnd {Kind kind; bool ok; Reason reason; int32_t id;}`.
- [ ] Step 1: failing tests — closed loop: `by 90`, `by -90`, `to` across 0°, `by 360` end `ok` within ±2° before timeout; `test_turn_every_end_sends_zero_then_stop` (each end path → last `MotionOut` has `w = 0`, `ended`, right reason); `test_service_owner_loss_ends` (`cancel(NotOwner)`); telemetry gap 200 ms → `Link`; `test_rate_deadman` (no `driveRate` update for 300 ms → ended `Deadman`, w 0); drive_rate in the model reaches ±10 % of 60 °/s within 1 s.
- [ ] Steps 2–5; commit `feat(esp32): motion service with closed-loop simulation tests`.

### Task 6: Wire into `link_task`; `primitive_done`, `tf.primitive`; page without MOVES
**Files:** `src/net/link_task.cpp`, `src/core/ws_json.h/.cpp` (`jsonPrimitiveDone(seq, tank_ms, kind, ok, reason, id)`, `jsonTf` gets `"primitive":{"kind":"turn","pct":n}` when active — pass via a small `PrimitiveView`), tests `test_ws_json`; `web/basic_drive.html` (remove `movesBtn`, `movesSheet`, the sheets map entry and the caps toggle).
- [ ] Step 1: failing JSON tests (`primitive_done` text; `tf` with and without `primitive`).
- [ ] Step 2: implement JSON; wiring per LLD R4.6: `kTurn` / `kDriveRate` → owner check (`g_owner.onDrive`), route; Local → `g_motion.start…`, ack id for `turn`; Forward → nack UNAVAILABLE for now with a ledger ruling (no full-firmware body to test; the frame path stays designed); Unavailable → nack UNAVAILABLE. On every new TELEM_FAST: `onTelemetry` → `sendDrive` through `g_owner.onDrive(owner_fd)`; non-Send → `cancel(NotOwner)`. Ends from STOP fast path, owner's drive / turn / drive_rate / cancel, `control_changed`, `body_offline`, WS close of the owner, OTA start. On `ended`: `sendDrive(0,0)`, `sendStop`, broadcast `primitive_done`.
- [ ] Step 3: page edit; `node --check`; pack size.
- [ ] Step 4: `pio test -e native` (all) + `pio run -e bridge`. Commit `feat(esp32): motion service wired into the link task; MOVES removed from the page`.

### Task 7: Flash, OTA, scripted checks on the tank (owner present)
- [ ] Step 1: Uno via USB (S1 to upload, then back; unplug USB); bridge via `scripts/ota.sh` (password never printed).
- [ ] Step 2 (stand): `/info` frames_rx rate ≈ 50/s more than before; page latency / tf 10 Hz unchanged; a non-owner phone's `turn` → NOT_OWNER; IR press mid-turn → `primitive_done reason not_owner`.
- [ ] Step 3 (floor, script + tape measure): 10 × `turn by 90` (5 left, 5 right) at speed 40 — record gyro error and the owner's measured angle; `turn to` the start heading; `drive_rate` 30 / 60 °/s on two surfaces for 3 s each (measured mean vs target).
- [ ] Step 4: record results in design-notes; tune `motion_config.h` only if acceptance fails (each change: test → floor → note).

### Task 8: Final review and handover
- [ ] Fresh reviewer on the branch range (most capable model); fix Critical / Important with tests; minors ledgered; owner OK → merge to `main`; delete the branch.

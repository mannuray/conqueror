# Plan R1 — Smooth turns (bridge LLD R18) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Spin turns and pivots slow to the slowest smooth spin in the last 25°, cut the power at the predicted stop (a learned coast), glide onto the target, and nudge at most once. No more 8–25 alternating pushes at the end of a turn (design-notes §112).

**Architecture:** `TurnController` (`core/`, pure, host-tested) gets phases Approach → Slow → Coast → (Nudge → NudgeCoast) → Done. The Slow and Nudge phases reuse the DRIVE_RATE `RateLoop` at `kSpinMinDdps` (65 °/s). The learned coast `T_coast` comes in through `begin()`, and a reading goes out once in `TurnStep`. `MotionService` passes it through to `PrimitiveEnd.coast_ms`. `MotionController` (`app/`) averages it like MOVE's roll-on and saves it in NVS (`cq_motion/coast_ms`) through the motion-store port. The roll-on reset also resets it. Arcs are unchanged; the Uno is unchanged.

**Tech Stack:** C++17 (ESP32-S3 Arduino 3.x, PlatformIO envs `bridge`, `bridge_ota`, `native`), Unity host tests, `scripts/size_check.py` (lizard 1.24.1).

**Spec:** `docs/lld/esp32-bridge-lld.md` §R18 (approved 2026-10-21; R18.5 holds the details settled while drafting this plan); the diagnosis is in `docs/design-notes.md` §112.

## Global Constraints
- Slow zone **25°** (`kTurnSlowZoneCdeg` 2500); slow rate **65 °/s** (`kSpinMinDdps` 650, unchanged).
- Cut when `error − rate × T_coast / 100 ≤ 0` along the way it drives (0.01°, 0.1 °/s, ms). Also cut when within **3°** (`kTurnDoneCdeg` 300) and turning slower than **30 °/s** (`kCoastMinRateDdps` 300).
- Stopped = the heading moved **< 2°** (`kSettleMoveCdeg` 200) over **300 ms** (`kWindowHoldMs`); otherwise the hold restarts.
- Stopped and within **3°** → done. Further off → **one** nudge (rate control at 65 °/s toward the target, the same cut), then done whatever the error. **Never a second nudge.**
- `T_coast`: default **60 ms** (`kCoastDefaultMs`). A reading counts only when the cut rate was ≥ 30 °/s and it is within **20–400 ms**, and only from the first coast. Average `(3 × old + reading + 2) / 4`, clamped to 20..400. NVS `cq_motion` key `coast_ms`. Reset together with the roll-on (Settings → Tune).
- Learned only from a turn or pivot that ended **done**.
- Stall (1.5 s at |rate| < 2 °/s while driven), timeout, cancel, STOP, IR takeover, link loss: unchanged. **No safety rule is relaxed.**
- No `%llu`, no heap, no new dependencies. Generated files are untouched. The protocol is unchanged (no protogen run, so the Uno needs no reflash).
- Conventional Commits, ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Branch `plan-r1` from `main`.

## Review Focus
1. A `to` turn already facing its heading, or `by` 0–3°, must end done at once with no push. A push would glide past by more than the error. Pinned by `test_turn_within_three_degrees_at_rest_does_not_push`.
2. A nudge **back** after an overshoot must cut on its own crossing. The cut is judged along the way it drives (`m_dir`), not the turn's sign. A pivot's nudge back must keep the same inside track. Pinned by `test_turn_nudge_back_after_an_overshoot` and `test_service_pivot_correction_keeps_the_same_track`.
3. A cancelled / stalled / timed-out turn must never change the learned coast, even when its first coast had already produced a reading. Pinned by `test_a_cancelled_turn_learns_nothing`; the controller learns only on `kDone`.
4. NVS out of range on load (e.g. an old or corrupt `coast_ms`) must fall back to 60 ms. `motionStoreLoadCoast` checks 20..400. It is not host-tested (`net/` is device-only): the reviewer checks it by reading.
5. Logo turns (RIGHT / LEFT / PIVOT, HOME) must get the learned coast too, not the default. `startLogo` passes `m_coast_ms`. The reviewer checks both call sites (lines near `kLogoPrimitive`).

## File structure
| File | Change |
|---|---|
| `firmware/esp32/src/core/motion_config.h` | R18 constants; the old window constants removed |
| `firmware/esp32/src/core/turn_controller.{h,cpp}` | rewritten: phases, rate-controlled Slow / Nudge, predicted-stop cut, coast reading, `coastAverage` |
| `firmware/esp32/src/core/motion_service.{h,cpp}` | `coast_ms` into `startTurn` / `startPivot`; `PrimitiveEnd.coast_ms` |
| `firmware/esp32/test/support/tank_model.h` | `setRealistic()` floor model (deadband lift, glide), `pushes()`, `directionChanges()` |
| `firmware/esp32/test/test_motion/test_main.cpp` | window tests → R18 tests; realistic closed-loop tests |
| `firmware/esp32/src/ports/motion_store.h`, `firmware/esp32/src/net/motion_store.{h,cpp}`, `firmware/esp32/src/net/ports_esp32.cpp`, `firmware/esp32/src/fakes/fake_ports.{h,cpp}` | `loadCoast` / `saveCoast` (NVS `coast_ms`) |
| `firmware/esp32/src/app/motion_controller.{h,cpp}` | load the coast, pass it to turns / pivots, learn from done ones (`learnFrom`) |
| `firmware/esp32/src/app/settings_controller.{h,cpp}`, `firmware/esp32/src/net/link_task.cpp` | the roll-on reset also resets the coast (`Hooks::coast_ms`) |
| `firmware/esp32/test/test_motion_controller`, `test_settings_controller`, `test_command_router` | coast learning / reset tests; hooks wiring |
| `docs/lld/esp32-bridge-lld.md`, `docs/design-notes.md`, `CLAUDE.md` | as built, floor results |

---

### Task 1: The R18 turn controller (core)

**Files:**
- Modify: `firmware/esp32/src/core/motion_config.h`
- Rewrite: `firmware/esp32/src/core/turn_controller.h`, `firmware/esp32/src/core/turn_controller.cpp`
- Modify: `firmware/esp32/src/core/motion_service.h`, `firmware/esp32/src/core/motion_service.cpp`
- Modify: `firmware/esp32/test/support/tank_model.h`, `firmware/esp32/test/test_motion/test_main.cpp`

**Interfaces:**
- Consumes: `RateLoop` (`core/rate_loop.h`: `reset()`, `step(target_ddps, rate_ddps, dt_ms)`, feed-forward default `kRateFfPctPerDps` 0.45), `motion::kSpinMinDdps`.
- Produces:
  - `enum class TurnPhase : uint8_t { kApproach, kSlow, kCoast, kNudge, kNudgeCoast }`
  - `TurnStep::coast_ms` (uint16_t, 0 = none)
  - `TurnController::begin(uint16_t start_yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, uint32_t now_ms, uint16_t coast_ms = motion::kCoastDefaultMs)`
  - `TurnController::phase()`
  - `uint16_t core::coastAverage(uint16_t old_ms, uint16_t reading_ms)`
  - `MotionService::startTurn(..., uint32_t now_ms, uint16_t coast_ms = motion::kCoastDefaultMs)`, and the same for `startPivot`
  - `PrimitiveEnd::coast_ms` (uint16_t, 0 = none)
  - `cqtest::TankModel::setRealistic(double decel_dps2 = 1000.0)`, `pushes()`, `directionChanges()`

- [ ] **Step 1: Write the failing tests — the realistic tank model**

Apply to `firmware/esp32/test/support/tank_model.h`:

```diff
diff --git a/firmware/esp32/test/support/tank_model.h b/firmware/esp32/test/support/tank_model.h
index 8b80054..12b6e06 100644
--- a/firmware/esp32/test/support/tank_model.h
+++ b/firmware/esp32/test/support/tank_model.h
@@ -2,6 +2,8 @@
 // Turn rate follows w x 2.2 °/s per % (floor data 2026-10-15) with a 150 ms lag; below 8 % the tracks do not
 // turn (static friction); commands arrive 40 ms late and telemetry reports 40 ms late (80 ms round trip);
 // the reported rate carries +-3 °/s seeded noise. Yaw + = CCW, wrapped 0..360 like the Uno's.
+// R18 (design-notes §112): setRealistic() models the floor instead - the motor deadband lifts the slowest spin
+// to ~50 °/s (no 2 °/s crawl), and with w = 0 the tank glides to a stop at a fixed deceleration (momentum).
 #pragma once
 
 #include <math.h>
@@ -16,13 +18,32 @@ class TankModel {
   }
   // A body that flips w when reversing (car-style, ignoring src AUTO) - to test the bridge's divergence guard.
   void setCarFlip(bool on) { m_car_flip = on; }
+  // R18: |w| < 8 % no turn; from 8 % the spin jumps to 50 °/s, then +1.4 °/s per % (60 % -> 123 °/s); cut to
+  // w = 0 it glides at decel_dps2 (°/s²) - 1000 gives ~2° from 65 °/s, plus the 40 ms the command takes to arrive.
+  void setRealistic(double decel_dps2 = 1000.0) {
+    m_realistic = true;
+    m_decel = decel_dps2;
+  }
+  int directionChanges() const { return m_flips; }  // the tank's real rate changed sign (a wobble)
+  int pushes() const { return m_pushes; }            // times the command went from 0 to turning
   // Advance 20 ms with the command (v, w) (%) just sent by the bridge.
   void step(int8_t w, int8_t v = 0) {
     if (m_car_flip && v < 0) w = static_cast<int8_t>(-w);
     m_cmd[m_head] = w;
     const int8_t applied = m_cmd[(m_head + 1) % kDelay];  // sent 2 ticks (40 ms) ago
-    const double target = (applied > -8 && applied < 8) ? 0.0 : applied * 2.2;
-    m_rate += (target - m_rate) * (0.02 / 0.15);
+    if (m_realistic) {
+      stepRealistic(applied);
+    } else {
+      const double target = (applied > -8 && applied < 8) ? 0.0 : applied * 2.2;
+      m_rate += (target - m_rate) * (0.02 / 0.15);
+    }
+    if (w != 0 && m_last_w == 0) ++m_pushes;
+    m_last_w = w;
+    if (m_rate != 0.0) {
+      const int sign = m_rate > 0 ? 1 : -1;
+      if (m_last_sign != 0 && sign != m_last_sign) ++m_flips;
+      m_last_sign = sign;
+    }
     m_yaw += m_rate * 0.02;
     m_turned += m_rate * 0.02;
     m_tel_yaw[m_head] = m_yaw;
@@ -40,6 +61,16 @@ class TankModel {
 
  private:
   static constexpr int kDelay = 2;
+  void stepRealistic(int8_t applied) {
+    const int a = applied < 0 ? -applied : applied;
+    if (a < 8) {  // no drive: momentum, a fixed deceleration down to 0 (never past it)
+      const double dv = m_decel * 0.02;
+      m_rate = fabs(m_rate) <= dv ? 0.0 : m_rate - (m_rate > 0 ? dv : -dv);
+      return;
+    }
+    const double target = (applied < 0 ? -1.0 : 1.0) * (50.0 + 1.4 * (a - 8));
+    m_rate += (target - m_rate) * (0.02 / 0.15);
+  }
   double noise() {
     m_seed = m_seed * 1103515245u + 12345u;
     return ((m_seed >> 16) % 601) / 100.0 - 3.0;  // -3.00 .. +3.00
@@ -53,6 +84,12 @@ class TankModel {
   int m_head = 0;
   uint32_t m_seed;
   bool m_car_flip = false;
+  bool m_realistic = false;
+  double m_decel = 1000.0;
+  int8_t m_last_w = 0;
+  int m_pushes = 0;
+  int m_last_sign = 0;
+  int m_flips = 0;
 };
 
 }  // namespace cqtest
```

- [ ] **Step 2: Write the failing tests — R18 unit and closed-loop tests**

Apply to `firmware/esp32/test/test_motion/test_main.cpp`. The window tests (`near_error_keeps_minimum`, `latency_compensation_brakes_early`, `done_after_hold_in_window`, `window_hysteresis`) are replaced. The shaking / drift / multi-turn tests are restated for R18. The smooth-model closed loop now passes that model's own coast (170 ms) and allows 3°.

```diff
diff --git a/firmware/esp32/test/test_motion/test_main.cpp b/firmware/esp32/test/test_motion/test_main.cpp
index 26c1412..4af270c 100644
--- a/firmware/esp32/test/test_motion/test_main.cpp
+++ b/firmware/esp32/test/test_motion/test_main.cpp
@@ -1,4 +1,4 @@
-// Plan H (bridge LLD R4): motion router, turn target, turn controller, rate loop, motion service.
+// Plan H (bridge LLD R4, R18): motion router, turn target, turn controller, rate loop, motion service.
 #include <math.h>
 #include <unity.h>
 
@@ -70,92 +70,172 @@ void test_turn_far_error_runs_at_speed_toward_target() {
   TEST_ASSERT_FALSE(s.done);
 }
 
-void test_turn_near_error_keeps_minimum() {
+// ---------- R18: slow zone, cut at the predicted stop, one nudge ----------
+void test_turn_slow_zone_holds_the_slowest_smooth_spin() {
   TurnController tc;
   tc.begin(0, 9000, 40, 0);
-  const TurnStep s = tc.step(8500, 0, 20);  // 5° left to go, still: 5 % < w_min -> w_min
-  TEST_ASSERT_EQUAL_INT8(motion::kTurnWMin, s.w);
+  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kApproach), static_cast<int>(tc.phase()));
+  const TurnStep s = tc.step(7000, motion::kSpinMinDdps, 20);  // 20° to go at 65 °/s: rate control, no cut yet
+  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kSlow), static_cast<int>(tc.phase()));
+  TEST_ASSERT_EQUAL_INT8(29, s.w);  // feed-forward 0.45 x 65 °/s, no error
 }
 
-void test_turn_latency_compensation_brakes_early() {
+void test_turn_short_turn_starts_slow() {
   TurnController tc;
-  tc.begin(0, 9000, 40, 0);
-  // 8° to go but turning at 120 °/s: 80 ms ahead it would be at -1.6° -> command reverses at w_min
-  const TurnStep s = tc.step(8200, 1200, 20);
-  TEST_ASSERT_EQUAL_INT8(-motion::kTurnWMin, s.w);
+  tc.begin(0, -1500, 40, 0);  // 15° right
+  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kSlow), static_cast<int>(tc.phase()));
+  const TurnStep s = tc.step(0, 0, 20);
+  TEST_ASSERT_TRUE(s.w < 0);  // toward the target under rate control
+}
+
+void test_turn_cuts_when_the_predicted_stop_reaches_the_target() {
+  TurnController tc;
+  tc.begin(0, 9000, 40, 0, 60);
+  TurnStep s = tc.step(8500, 650, 20);  // 5° to go, 65 °/s x 60 ms = 3.9°: not yet
+  TEST_ASSERT_TRUE(s.w > 0);
+  s = tc.step(8700, 650, 40);  // 3° to go < 3.9°: cut
+  TEST_ASSERT_EQUAL_INT8(0, s.w);
+  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kCoast), static_cast<int>(tc.phase()));
+  s = tc.step(8900, 300, 60);  // still gliding: w stays 0, not done
+  TEST_ASSERT_EQUAL_INT8(0, s.w);
+  TEST_ASSERT_FALSE(s.done);
 }
 
 namespace {
-// Arrive at `yaw` and stand still until the heading average sees it (the window needs < 15 °/s over 200 ms):
-// returns the time the window was entered (first w == 0).
-uint32_t settleAt(TurnController& tc, uint16_t yaw, uint32_t& t) {
-  for (int i = 0; i < 40; ++i) {
-    if (tc.step(yaw, 0, t += 20).w == 0) return t;
-  }
-  return 0;
+// After a cut at 8700 (65 °/s), glide to `stop` and hold there: returns the step that ended the hold.
+TurnStep glideTo(TurnController& tc, uint16_t stop, uint32_t& t) {
+  TurnStep s{};
+  for (int i = 0; i < 40 && !s.done && tc.phase() != TurnPhase::kNudge; ++i) s = tc.step(stop, 0, t += 20);
+  return s;
 }
 }  // namespace
 
-void test_turn_done_after_hold_in_window() {
+void test_turn_done_once_stopped_with_the_coast_reading() {
   TurnController tc;
-  tc.begin(0, 9000, 40, 0);
+  tc.begin(0, 9000, 40, 0, 60);
   uint32_t t = 0;
-  const uint32_t entered = settleAt(tc, 8960, t);  // 0.4° off, still: enters the window
-  TEST_ASSERT_TRUE(entered > 0);
-  TurnStep s{};
-  while (t + 20 < entered + motion::kWindowHoldMs) {
-    s = tc.step(8960, 0, t += 20);
-    TEST_ASSERT_EQUAL_INT8(0, s.w);
-    TEST_ASSERT_FALSE(s.done);
+  tc.step(8700, 650, t += 20);  // cut at 87° turning 65 °/s
+  const uint32_t cut_at = t;
+  const TurnStep s = glideTo(tc, 8990, t);  // glides 2.9°, stops 0.1° short
+  TEST_ASSERT_TRUE(s.done);
+  TEST_ASSERT_TRUE(t >= cut_at + motion::kWindowHoldMs);  // done only after the stillness hold
+  TEST_ASSERT_EQUAL_UINT16(44, s.coast_ms);  // 290 cdeg x 100 / 650 ddps
+}
+
+void test_turn_one_nudge_when_it_stops_short() {
+  TurnController tc;
+  tc.begin(0, 9000, 40, 0, 60);
+  uint32_t t = 0;
+  tc.step(8000, 650, t += 20);
+  tc.step(8700, 650, t += 20);  // cut
+  TurnStep s = glideTo(tc, 8500, t);  // went back?? no: stopped 5° short (the heading did not reach the cut point)
+  TEST_ASSERT_FALSE(s.done);
+  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kNudge), static_cast<int>(tc.phase()));
+  s = tc.step(8500, 0, t += 20);
+  TEST_ASSERT_TRUE(s.w > 0);  // the nudge: toward the target
+  s = tc.step(8700, 650, t += 20);  // the nudge's own cut
+  TEST_ASSERT_EQUAL_INT8(0, s.w);
+  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kNudgeCoast), static_cast<int>(tc.phase()));
+  s = glideTo(tc, 8650, t);  // still 3.5° short: done anyway - never a second nudge
+  TEST_ASSERT_TRUE(s.done);
+  TEST_ASSERT_EQUAL_UINT16(0, s.coast_ms);  // learned from the first coast only
+}
+
+void test_turn_nudge_back_after_an_overshoot() {
+  TurnController tc;
+  tc.begin(0, 9000, 40, 0, 60);
+  uint32_t t = 0;
+  tc.step(8700, 650, t += 20);  // cut
+  glideTo(tc, 9500, t);         // glided 5° past
+  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kNudge), static_cast<int>(tc.phase()));
+  TurnStep s = tc.step(9500, 0, t += 20);
+  TEST_ASSERT_TRUE(s.w < 0);  // back toward the target
+  s = tc.step(9400, -650, t += 20);  // 4° to go back at 65 °/s: no cut yet (3.9°)
+  TEST_ASSERT_TRUE(s.w < 0);
+  s = tc.step(9300, -650, t += 20);  // 3° to go: cut
+  TEST_ASSERT_EQUAL_INT8(0, s.w);
+  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kNudgeCoast), static_cast<int>(tc.phase()));
+}
+
+// A turn within 3° of its target and not turning is already there: the slowest spin would glide past further than
+// that, so it ends without a push (also a "to" heading it already faces, and a turn of 0-3°).
+void test_turn_within_three_degrees_at_rest_does_not_push() {
+  const int32_t deltas[] = {0, 250, -300, 9000};
+  for (int32_t d : deltas) {
+    TurnController tc;
+    tc.begin(0, d, 40, 0);
+    uint32_t t = 0;
+    const uint16_t at = static_cast<uint16_t>(d == 9000 ? 8990 : 0);  // 9000: arrives 0.1° short, still
+    TurnStep s{};
+    for (int i = 0; i < 30 && !s.done; ++i) {
+      s = tc.step(at, 0, t += 20);
+      TEST_ASSERT_EQUAL_INT8(0, s.w);
+    }
+    TEST_ASSERT_TRUE(s.done);
+    TEST_ASSERT_FALSE(s.stalled);
   }
-  s = tc.step(8960, 0, t += 20);
-  TEST_ASSERT_TRUE(s.done);   // hold reached
+  TurnController tc;  // 4° at rest: it does push (then glides)
+  tc.begin(0, 400, 40, 0);
+  TEST_ASSERT_TRUE(tc.step(0, 0, 20).w > 0);
 }
 
-// Floor test 2026-10-16 (marble): on target the tank shook left-right (gyro +-55 °/s, heading +-1°); the momentary
-// rate kept it out of the window and the latency term turned the jerks into +-12 % corrections until the timeout.
-// Inside the window the command is 0 whatever the rate, and the turn ends after the hold.
+void test_turn_coast_reading_ignores_bad_cuts() {
+  {  // cut while turning slower than 30 °/s: no reading
+    TurnController tc;
+    tc.begin(0, 9000, 40, 0, 60);
+    uint32_t t = 0;
+    tc.step(8950, 200, t += 20);
+    TEST_ASSERT_EQUAL_UINT16(0, glideTo(tc, 9000, t).coast_ms);
+  }
+  {  // coasted 9° from 65 °/s = 138 ms: kept; 30° = 461 ms: above 400 ms, dropped
+    TurnController tc;
+    tc.begin(0, 9000, 40, 0, 60);
+    uint32_t t = 0;
+    tc.step(8700, 650, t += 20);
+    TEST_ASSERT_EQUAL_UINT16(0, glideTo(tc, 11700, t).coast_ms);  // also 27° past: nudge, reading dropped
+  }
+}
+
+void test_coast_average_moves_a_quarter_and_stays_in_range() {
+  TEST_ASSERT_EQUAL_UINT16(70, coastAverage(60, 100));  // (180 + 100 + 2) / 4
+  TEST_ASSERT_EQUAL_UINT16(50, coastAverage(60, 20));   // (180 + 20 + 2) / 4
+  TEST_ASSERT_EQUAL_UINT16(motion::kCoastMinMs, coastAverage(20, 20));
+  TEST_ASSERT_EQUAL_UINT16(motion::kCoastMaxMs, coastAverage(400, 400));
+}
+
+// Floor test 2026-10-16 (marble): on target the tank shook left-right (gyro +-55 °/s, heading +-1°). Coasting, the
+// command is 0 whatever the rate, and "stopped" looks at the heading, not the gyro: the turn ends after the hold.
 void test_turn_shaking_at_target_finishes_without_corrections() {
   TurnController tc;
   tc.begin(0, 9000, 40, 0);
-  TurnStep s{};
   uint32_t t = 0;
+  tc.step(8700, 650, t += 20);  // cut
+  TurnStep s{};
   bool done = false;
-  for (int i = 0; i < 60 && !done; ++i) {
+  for (int i = 0; i < 40 && !done; ++i) {
     const bool up = i % 2 == 0;
     s = tc.step(up ? 8900 : 9050, up ? 550 : -550, t += 20);  // -1° / +0.5°, jerking +-55 °/s
-    if (t > motion::kAvgRateMs + 20) TEST_ASSERT_EQUAL_INT8(0, s.w);  // once the arrival left the average
+    TEST_ASSERT_EQUAL_INT8(0, s.w);
     done = s.done;
   }
   TEST_ASSERT_TRUE(done);
-  TEST_ASSERT_TRUE(t <= motion::kAvgRateMs + motion::kWindowHoldMs + 60);  // average settles, then the hold
+  // the first hold restarts (the glide from the cut moved it 2° or more), the second one ends the turn
+  TEST_ASSERT_TRUE(t <= 20 + 2 * motion::kWindowHoldMs + 40);
 }
 
-// A real slow drift through the window (8 °/s) is not "done": it is still moving, and corrects once it leaves.
+// A real slow drift (8 °/s = 2.4° per 300 ms) is not "stopped": no end, no nudge while it moves.
 void test_turn_slow_drift_is_not_done() {
   TurnController tc;
   tc.begin(0, 9000, 40, 0);
-  TurnStep s{};
   uint32_t t = 0;
-  int32_t yaw = 8860;  // 1.4° short, drifting on at 8 °/s (16 cdeg per 20 ms)
-  while (yaw < 9260) {
-    s = tc.step(static_cast<uint16_t>(yaw), 80, t += 20);
+  tc.step(8700, 650, t += 20);  // cut
+  int32_t yaw = 8860;
+  for (int i = 0; i < 40; ++i) {
+    const TurnStep s = tc.step(static_cast<uint16_t>(yaw), 80, t += 20);
     TEST_ASSERT_FALSE(s.done);
+    TEST_ASSERT_EQUAL_INT8(0, s.w);
     yaw += 16;
   }
-  s = tc.step(static_cast<uint16_t>(yaw), 80, t += 20);  // 2.7° past: out of the window
-  TEST_ASSERT_TRUE(s.w < 0);  // corrects back
-}
-
-void test_turn_window_hysteresis() {
-  TurnController tc;
-  tc.begin(0, 9000, 40, 0);
-  uint32_t t = 0;
-  TEST_ASSERT_TRUE(settleAt(tc, 8900, t) > 0);  // 1° off: in
-  TurnStep s = tc.step(8800, 0, t += 20);  // 2° off: still settling (exit band 2.5°)
-  TEST_ASSERT_EQUAL_INT8(0, s.w);
-  s = tc.step(8700, 0, t += 20);  // 3° off: leaves the window, drives again
-  TEST_ASSERT_EQUAL_INT8(motion::kTurnWMin, s.w);
 }
 
 void test_turn_stalled_and_timeout() {
@@ -173,14 +253,23 @@ void test_turn_stalled_and_timeout() {
   TEST_ASSERT_TRUE(s.timeout);
 }
 
+void test_turn_stalled_in_the_slow_zone() {
+  TurnController tc;
+  tc.begin(0, 1500, 40, 0);  // Slow from the start; the tank never moves
+  TurnStep s{};
+  for (uint32_t t = 20; t <= 1700 && !s.stalled; t += 20) s = tc.step(0, 0, t);
+  TEST_ASSERT_TRUE(s.stalled);
+}
+
 void test_turn_tracks_multi_turn() {
   TurnController tc;
   tc.begin(35000, 36000, 40, 0);  // by 360 from 350°
   // pass through 0° twice-worth of wrap: 350 -> 170 (crossing 0) -> 349
   tc.step(100, 900, 20);    // +11° (wrapped)
   tc.step(17000, 900, 40);  // +169°
-  const TurnStep s = tc.step(34900, 900, 60);  // +359° total, 1° to go
-  TEST_ASSERT_NOT_EQUAL(0, s.w);  // still turning (latency term), not done yet
+  const TurnStep s = tc.step(34900, 900, 60);  // +359° total, 1° to go at 90 °/s: cut
+  TEST_ASSERT_EQUAL_INT8(0, s.w);
+  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kCoast), static_cast<int>(tc.phase()));
   TEST_ASSERT_TRUE(tc.progressPct() >= 99);
 }
 
@@ -228,22 +317,38 @@ struct TurnRun {
   double error_deg = 0;    // turned - requested, after 1 s of settling
   int8_t last_w = 99;      // the final command the service sent
   uint32_t ms = 0;
+  int pushes = 0;          // R18: times the command went from 0 to turning (1 = no nudge, 2 = one nudge)
+  int flips = 0;           // R18: times the tank's real turn changed direction (a wobble)
+  double slow_rate = 0;    // R18: |rate| (°/s) 10° before the target
+};
+
+struct TurnOpts {
+  bool realistic = false;  // R18: the floor model (deadband lift + glide)
+  uint16_t coast_ms = motion::kCoastDefaultMs;
+  double decel = 1000.0;
+  bool pivot = false;
 };
 
 TurnRun runTurn(double start_yaw, bool has_to, int32_t by_cdeg, uint16_t to_cdeg, uint8_t speed = 40,
-               uint32_t seed = 12345) {
+               uint32_t seed = 12345, const TurnOpts& opt = TurnOpts{}) {
   cqtest::TankModel tank(start_yaw, seed);
+  if (opt.realistic) tank.setRealistic(opt.decel);
   MotionService svc;
   const uint16_t yaw0 = tank.reportedYawCdeg();
   const int32_t delta = turnDelta(yaw0, has_to, by_cdeg, to_cdeg);
-  svc.startTurn(7, yaw0, delta, speed, 42, 0);
+  if (opt.pivot) {
+    svc.startPivot(7, yaw0, delta, speed, 42, 0, opt.coast_ms);
+  } else {
+    svc.startTurn(7, yaw0, delta, speed, 42, 0, opt.coast_ms);
+  }
   TurnRun r;
   int8_t w = 0;
   for (uint32_t t = 20; t <= 30000 && !r.ended; t += 20) {
     tank.step(w);
+    if (r.slow_rate == 0 && fabs(delta / 100.0 - tank.turnedDeg()) <= 10.0) r.slow_rate = fabs(tank.rateDps());
     const MotionOut o = svc.onTelemetry(tank.reportedYawCdeg(), tank.reportedRateDdps(), t);
     if (o.send) {
-      w = o.w;
+      w = o.w;  // a pivot's w is the spin part; the model turns by it
       r.last_w = o.w;
     }
     if (o.ended) {
@@ -254,6 +359,8 @@ TurnRun runTurn(double start_yaw, bool has_to, int32_t by_cdeg, uint16_t to_cdeg
   }
   for (int i = 0; i < 50; ++i) tank.step(0);  // 1 s coasting with w = 0 (the bridge then sends STOP)
   r.error_deg = tank.turnedDeg() - delta / 100.0;
+  r.pushes = tank.pushes();
+  r.flips = tank.directionChanges();
   return r;
 }
 }  // namespace
@@ -264,17 +371,85 @@ void test_service_turn_closed_loop() {
       {123, false, 36000, 0, 40}, {0, false, 1000, 0, 40}, {0, false, 18000, 0, 40}, {0, false, -72000, 0, 40},
       {0, false, 9000, 0, 12},  {0, false, 9000, 0, 100}};
   const uint32_t seeds[] = {1u, 7u, 99u};
+  TurnOpts o;
+  o.coast_ms = 170;  // R18: this smooth model glides on its 150 ms lag (~170 ms coast, as the learning would find)
   for (const auto& c : cases) for (uint32_t seed : seeds) {
-    const TurnRun r = runTurn(c.start, c.to, c.by, c.to_c, c.speed, seed);
+    const TurnRun r = runTurn(c.start, c.to, c.by, c.to_c, c.speed, seed, o);
     TEST_ASSERT_TRUE(r.ended);
     TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDone), static_cast<int>(r.end.reason));
     TEST_ASSERT_TRUE(r.end.ok);
     TEST_ASSERT_EQUAL_INT32(42, r.end.id);
     TEST_ASSERT_EQUAL_INT8(0, r.last_w);
-    TEST_ASSERT_TRUE_MESSAGE(fabs(r.error_deg) <= 2.0, "turn error above 2 degrees");
+    TEST_ASSERT_TRUE_MESSAGE(fabs(r.error_deg) <= 3.0, "turn error above 3 degrees");
+    TEST_ASSERT_TRUE_MESSAGE(r.pushes <= 2, "more than one nudge");
   }
 }
 
+// R18 (design-notes §112): on the floor model a turn slows to ~65 °/s, glides onto the target with one cut, needs at
+// most one nudge and never wobbles - the old controller made 8-25 alternating pushes here.
+void test_service_turn_realistic_one_smooth_stop() {
+  const struct { int32_t by; uint8_t speed; } cases[] = {{9000, 40}, {-9000, 40}, {36000, 40}, {1000, 40},
+                                                         {18000, 60}, {-4500, 100}, {9000, 12}};
+  const uint32_t seeds[] = {1u, 7u, 99u};
+  for (const auto& c : cases) for (uint32_t seed : seeds) {
+    TurnOpts o;
+    o.realistic = true;
+    o.coast_ms = 70;  // the model's own coast, as learned (see the learning test)
+    const TurnRun r = runTurn(0, false, c.by, 0, c.speed, seed, o);
+    TEST_ASSERT_TRUE(r.ended);
+    TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDone), static_cast<int>(r.end.reason));
+    TEST_ASSERT_EQUAL_INT8(0, r.last_w);
+    TEST_ASSERT_TRUE_MESSAGE(fabs(r.error_deg) <= 3.0, "turn error above 3 degrees");
+    TEST_ASSERT_TRUE_MESSAGE(r.pushes <= 2, "more than one nudge");
+    TEST_ASSERT_TRUE_MESSAGE(r.flips <= 1, "the turn wobbled");
+    if (c.by >= 3000 || c.by <= -3000) TEST_ASSERT_TRUE_MESSAGE(r.slow_rate <= 80.0, "not slowed near the target");
+  }
+}
+
+// A wrong coast (the default 60 ms against a slipperier floor) still ends within one nudge; the readings pull the
+// learned value toward the floor's real coast within about ten turns, and then the turns need no nudge.
+void test_service_turn_realistic_learns_the_coast() {
+  const double decels[] = {1000.0, 500.0};
+  for (double decel : decels) {
+    uint16_t coast = motion::kCoastDefaultMs;
+    TurnRun r;
+    for (int i = 0; i < 12; ++i) {
+      TurnOpts o;
+      o.realistic = true;
+      o.coast_ms = coast;
+      o.decel = decel;
+      r = runTurn(0, false, i % 2 ? -9000 : 9000, 0, 40, 11u + i, o);
+      TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDone), static_cast<int>(r.end.reason));
+      TEST_ASSERT_TRUE(r.pushes <= 2);
+      if (r.end.coast_ms != 0) coast = coastAverage(coast, r.end.coast_ms);
+    }
+    TEST_ASSERT_TRUE_MESSAGE(fabs(r.error_deg) <= 1.5, "learned turn not within 1.5 degrees");
+    TEST_ASSERT_EQUAL_INT_MESSAGE(1, r.pushes, "learned turn still needs a nudge");
+  }
+}
+
+void test_service_pivot_realistic_one_smooth_stop() {
+  for (int dir = -1; dir <= 1; dir += 2) {
+    TurnOpts o;
+    o.realistic = true;
+    o.coast_ms = 70;
+    o.pivot = true;
+    const TurnRun r = runTurn(0, false, dir * 9000, 0, 40, 5u, o);
+    TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDone), static_cast<int>(r.end.reason));
+    TEST_ASSERT_TRUE(fabs(r.error_deg) <= 3.0);
+    TEST_ASSERT_TRUE(r.pushes <= 2);
+    TEST_ASSERT_TRUE(r.flips <= 1);
+  }
+}
+
+void test_service_turn_end_carries_the_coast_reading() {
+  TurnOpts o;
+  o.realistic = true;
+  const TurnRun r = runTurn(0, false, 9000, 0, 40, 3u, o);
+  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDone), static_cast<int>(r.end.reason));
+  TEST_ASSERT_TRUE(r.end.coast_ms >= motion::kCoastMinMs && r.end.coast_ms <= motion::kCoastMaxMs);
+}
+
 void test_turn_every_end_sends_zero_then_stop() {
   const EndReason reasons[] = {EndReason::kCancelled, EndReason::kStop, EndReason::kNotOwner};
   for (EndReason why : reasons) {
@@ -784,9 +959,10 @@ void test_service_pivot_correction_keeps_the_same_track() {
   for (int dir = -1; dir <= 1; dir += 2) {
     MotionService svc;
     svc.startPivot(3, 0, dir * 9000, 40, 1, 0);
-    // 5° past the target, still: the controller turns back
+    // 5° past the target, still: R18 coasts (w = 0) until stopped, then nudges back
     const uint16_t past = static_cast<uint16_t>(dir > 0 ? 9500 : 36000 - 9500);
-    MotionOut o = svc.onTelemetry(past, 0, 20);
+    MotionOut o;
+    for (uint32_t t = 20; t <= 1000 && !(o.send && o.w != 0); t += 20) o = svc.onTelemetry(past, 0, t);
     TEST_ASSERT_TRUE(o.send);
     TEST_ASSERT_TRUE(dir > 0 ? o.w < 0 : o.w > 0);  // turning back
     const int l = o.v - o.w, r = o.v + o.w;
@@ -802,13 +978,19 @@ int main() {
   RUN_TEST(test_router_move_needs_encoders);
   RUN_TEST(test_turn_target_wrap);
   RUN_TEST(test_turn_far_error_runs_at_speed_toward_target);
-  RUN_TEST(test_turn_near_error_keeps_minimum);
-  RUN_TEST(test_turn_latency_compensation_brakes_early);
-  RUN_TEST(test_turn_done_after_hold_in_window);
+  RUN_TEST(test_turn_slow_zone_holds_the_slowest_smooth_spin);
+  RUN_TEST(test_turn_short_turn_starts_slow);
+  RUN_TEST(test_turn_cuts_when_the_predicted_stop_reaches_the_target);
+  RUN_TEST(test_turn_done_once_stopped_with_the_coast_reading);
+  RUN_TEST(test_turn_one_nudge_when_it_stops_short);
+  RUN_TEST(test_turn_nudge_back_after_an_overshoot);
+  RUN_TEST(test_turn_within_three_degrees_at_rest_does_not_push);
+  RUN_TEST(test_turn_coast_reading_ignores_bad_cuts);
+  RUN_TEST(test_coast_average_moves_a_quarter_and_stays_in_range);
   RUN_TEST(test_turn_shaking_at_target_finishes_without_corrections);
   RUN_TEST(test_turn_slow_drift_is_not_done);
-  RUN_TEST(test_turn_window_hysteresis);
   RUN_TEST(test_turn_stalled_and_timeout);
+  RUN_TEST(test_turn_stalled_in_the_slow_zone);
   RUN_TEST(test_turn_tracks_multi_turn);
   RUN_TEST(test_rate_feed_forward_at_zero_error);
   RUN_TEST(test_rate_integral_grows_on_shortfall);
@@ -816,6 +998,10 @@ int main() {
   RUN_TEST(test_rate_sign_change_resets_integral);
   RUN_TEST(test_rate_zero_target_clears_integral);
   RUN_TEST(test_service_turn_closed_loop);
+  RUN_TEST(test_service_turn_realistic_one_smooth_stop);
+  RUN_TEST(test_service_turn_realistic_learns_the_coast);
+  RUN_TEST(test_service_pivot_realistic_one_smooth_stop);
+  RUN_TEST(test_service_turn_end_carries_the_coast_reading);
   RUN_TEST(test_turn_every_end_sends_zero_then_stop);
   RUN_TEST(test_service_owner_loss_ends);
   RUN_TEST(test_service_telemetry_gap_ends_link);
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_motion`
Expected: FAIL to compile — `TurnPhase` / `phase()` / `coastAverage` / `setRealistic` not declared, `startTurn` has too many arguments.

- [ ] **Step 4: Implement — constants**

Apply to `firmware/esp32/src/core/motion_config.h`:

```diff
diff --git a/firmware/esp32/src/core/motion_config.h b/firmware/esp32/src/core/motion_config.h
index 670073b..7d97440 100644
--- a/firmware/esp32/src/core/motion_config.h
+++ b/firmware/esp32/src/core/motion_config.h
@@ -11,17 +11,20 @@ namespace motion {
 constexpr int32_t kTurnKpPctPerDeg = 1;      // w = 1 % per degree still to go
 constexpr int8_t kTurnWMin = 12;             // % - smallest command that still turns
 constexpr int32_t kLatencyDdpsToCdeg = 8;    // e_p = e - rate * 80 ms  ->  rate_ddps * 8 / 10 (0.01°)
-// Target window by heading only (floor test 2026-10-16, LLD R4.16): inside it w = 0 whatever the gyro rate (a
-// shaking tank reads +-55 °/s on target), done once held with little net movement (rate averaged over the hold).
-constexpr int32_t kWindowInCdeg = 150;       // window: |e| <= 1.5° and the heading's own average rate over
-constexpr int16_t kWindowEnterRateDdps = 150;  // the last 200 ms < 15 °/s (not still sweeping through) ...
-constexpr uint32_t kAvgRateMs = 200;
-constexpr int32_t kWindowOutCdeg = 250;      // ... left again only beyond 2.5° (no hunting around the target)
-constexpr uint32_t kWindowHoldMs = 300;      // done: in the window for 300 ms ...
-constexpr int32_t kSettleMoveCdeg = 200;     // ... moving < 2° over it (avg < 6.7 °/s), else the hold restarts
+// Stopped (R18; the floor test 2026-10-16, LLD R4.16, taught to judge it by the heading - a shaking tank reads
+// +-55 °/s on target): the heading moved < 2° over 300 ms (avg < 6.7 °/s), else the hold restarts.
+constexpr uint32_t kWindowHoldMs = 300;
+constexpr int32_t kSettleMoveCdeg = 200;
 constexpr int16_t kStallRateDdps = 20;       // stalled: |rate| < 2 °/s while commanding >= w_min ...
 constexpr uint32_t kStallMs = 1500;          // ... for 1.5 s
 constexpr uint32_t kTimeoutBaseMs = 3000;    // timeout = |angle| / 20 °/s + 3 s
+// R18 (design-notes §112): approach, slow to the slowest smooth spin, cut at the predicted stop, at most one nudge.
+constexpr int32_t kTurnSlowZoneCdeg = 2500;  // the last 25° under rate control at kSpinMinDdps
+constexpr int32_t kTurnDoneCdeg = 300;       // stopped within 3°: done; further off: one nudge, then done anyway
+constexpr uint16_t kCoastDefaultMs = 60;     // T_coast: predicted stop = pos + rate x T_coast (learned, NVS)
+constexpr uint16_t kCoastMinMs = 20;         // a reading counts only within 20..400 ms ...
+constexpr uint16_t kCoastMaxMs = 400;
+constexpr int16_t kCoastMinRateDdps = 300;   // ... and with a cut rate of at least 30 °/s
 
 // DRIVE_RATE: w = ff * r + Kp * err + Ki * integral(err), |w| <= 100, integration stops while saturated.
 constexpr float kRateFfPctPerDps = 0.45f;  // floor data 2026-10-15: w 60 % spin ~ 110-137 °/s
```

- [ ] **Step 5: Implement — the turn controller**

Replace `firmware/esp32/src/core/turn_controller.h` with:

```cpp
// TURN (spin) and PIVOT on the bridge (bridge LLD R4.3, R18): approach proportional to the angle still to go
// (latency-compensated, minimum command), the last 25° under rate control at the slowest smooth spin, a cut at the
// predicted stop (learned coast), at most one nudge, done once stopped; stall and timeout. Whole numbers, except
// the rate loop.
#pragma once

#include <stdint.h>

#include "core/motion_config.h"
#include "core/rate_loop.h"

namespace cq {
namespace core {

// Relative angle to turn (0.01°, + = CCW / left): `by` as given, or `to` the shortest way from the current yaw.
int32_t turnDelta(uint16_t yaw_cdeg, bool has_to, int32_t by_cdeg, uint16_t to_cdeg);
int32_t wrapDeltaCdeg(int32_t d);  // to -18000..17999
// R18.2: the learned coast, as the roll-on: (3 x old + reading + 2) / 4, kept within kCoastMinMs..kCoastMaxMs.
uint16_t coastAverage(uint16_t old_ms, uint16_t reading_ms);

struct TurnStep {
  int8_t w = 0;  // % for DRIVE (v = 0)
  bool done = false;
  bool timeout = false;
  bool stalled = false;
  uint16_t coast_ms = 0;  // R18: a new coast reading (ms) once the first coast has stopped; 0 = none
};

enum class TurnPhase : uint8_t { kApproach, kSlow, kCoast, kNudge, kNudgeCoast };

class TurnController {
 public:
  // coast_ms: the learned T_coast (predicted stop = pos + rate x T_coast).
  void begin(uint16_t start_yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, uint32_t now_ms,
             uint16_t coast_ms = motion::kCoastDefaultMs);
  TurnStep step(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms);
  uint8_t progressPct() const;
  TurnPhase phase() const { return m_phase; }

 private:
  int8_t command(int32_t error_cdeg, int16_t rate_ddps) const;  // the approach (proportional)
  void driven(TurnStep& s, int32_t error, int16_t rate_ddps, uint32_t now_ms);
  void coasting(TurnStep& s, int32_t error, uint32_t now_ms);
  bool cutNow(int32_t error_cdeg, int16_t rate_ddps) const;  // the predicted stop reaches the target
  void cut(int16_t rate_ddps, uint32_t now_ms);
  uint16_t coastReading() const;
  bool stopped(uint32_t now_ms);
  bool stalledFor(int8_t w, int16_t rate_ddps, uint32_t now_ms);

  int32_t m_delta = 0;      // target, relative to the start (0.01°)
  int32_t m_pos = 0;        // turned so far, unwrapped (0.01°)
  uint16_t m_last_yaw = 0;
  uint8_t m_speed = 40;
  uint32_t m_start_ms = 0;
  uint32_t m_last_ms = 0;
  uint32_t m_timeout_ms = 0;
  uint16_t m_coast_ms = 60;
  TurnPhase m_phase = TurnPhase::kApproach;
  int8_t m_dir = 1;          // the way it drives now: the turn's sign, or the nudge's (back after an overshoot)
  RateLoop m_rate;
  int32_t m_cut_pos = 0;     // where the first cut happened ...
  int16_t m_cut_rate = 0;    // ... and how fast it was turning then
  uint32_t m_still_since = 0;
  int32_t m_still_pos = 0;   // m_pos when the current stillness hold began
  bool m_stalling = false;
  uint32_t m_stall_since = 0;
};

}  // namespace core
}  // namespace cq
```

Replace `firmware/esp32/src/core/turn_controller.cpp` with:

```cpp
#include "core/turn_controller.h"

#include "core/motion_config.h"

namespace cq {
namespace core {
namespace {
int32_t absI(int32_t v) { return v < 0 ? -v : v; }
}  // namespace

uint16_t coastAverage(uint16_t old_ms, uint16_t reading_ms) {
  int32_t v = (3 * static_cast<int32_t>(old_ms) + reading_ms + 2) / 4;
  if (v < motion::kCoastMinMs) v = motion::kCoastMinMs;
  if (v > motion::kCoastMaxMs) v = motion::kCoastMaxMs;
  return static_cast<uint16_t>(v);
}

int32_t wrapDeltaCdeg(int32_t d) {
  d %= 36000;
  if (d >= 18000) d -= 36000;
  if (d < -18000) d += 36000;
  return d;
}

int32_t turnDelta(uint16_t yaw_cdeg, bool has_to, int32_t by_cdeg, uint16_t to_cdeg) {
  if (!has_to) return by_cdeg;
  const int32_t d = wrapDeltaCdeg(static_cast<int32_t>(to_cdeg) - yaw_cdeg);
  return d == -18000 ? 18000 : d;  // exactly opposite: turn left (+180°)
}

void TurnController::begin(uint16_t start_yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, uint32_t now_ms,
                           uint16_t coast_ms) {
  m_delta = delta_cdeg;
  m_pos = 0;
  m_last_yaw = start_yaw_cdeg;
  m_speed = speed_pct;
  m_start_ms = m_last_ms = now_ms;
  m_timeout_ms = static_cast<uint32_t>(absI(delta_cdeg)) / 2u + motion::kTimeoutBaseMs;  // |angle| / 20 °/s + 3 s
  m_coast_ms = coast_ms;
  m_phase = absI(delta_cdeg) <= motion::kTurnSlowZoneCdeg ? TurnPhase::kSlow : TurnPhase::kApproach;
  m_dir = delta_cdeg < 0 ? -1 : 1;
  m_rate.reset();
  m_cut_pos = 0;
  m_cut_rate = 0;
  m_stalling = false;
}

int8_t TurnController::command(int32_t error_cdeg, int16_t rate_ddps) const {
  const int32_t ep = error_cdeg - static_cast<int32_t>(rate_ddps) * motion::kLatencyDdpsToCdeg / 10;
  const int32_t e = ep != 0 ? ep : error_cdeg;
  int32_t mag = absI(e) * motion::kTurnKpPctPerDeg / 100;
  if (mag < motion::kTurnWMin) mag = motion::kTurnWMin;
  if (mag > m_speed) mag = m_speed;
  return static_cast<int8_t>(e < 0 ? -mag : mag);
}

// R18: w = 0 once the tank would stop on (or past) the target if the power were cut now - along the way it
// drives (m_dir), so a nudge back after an overshoot cuts on its own crossing. Within 3° and not turning it is
// already there: the slowest spin would glide past by more than that.
bool TurnController::cutNow(int32_t error_cdeg, int16_t rate_ddps) const {
  const int16_t r = rate_ddps < 0 ? static_cast<int16_t>(-rate_ddps) : rate_ddps;
  if (absI(error_cdeg) <= motion::kTurnDoneCdeg && r < motion::kCoastMinRateDdps) return true;
  const int32_t left = error_cdeg - static_cast<int32_t>(rate_ddps) * m_coast_ms / 100;  // 0.1 °/s x ms -> 0.01°
  return m_dir > 0 ? left <= 0 : left >= 0;
}

void TurnController::cut(int16_t rate_ddps, uint32_t now_ms) {
  if (m_phase != TurnPhase::kNudge) {  // the first cut is the one the coast is learned from
    m_cut_pos = m_pos;
    m_cut_rate = rate_ddps;
  }
  m_phase = m_phase == TurnPhase::kNudge ? TurnPhase::kNudgeCoast : TurnPhase::kCoast;
  m_still_since = now_ms;
  m_still_pos = m_pos;
}

// Stopped: held for kWindowHoldMs with < kSettleMoveCdeg net movement (the heading, not the shaking gyro rate).
bool TurnController::stopped(uint32_t now_ms) {
  if (now_ms - m_still_since < motion::kWindowHoldMs) return false;
  if (absI(m_pos - m_still_pos) < motion::kSettleMoveCdeg) return true;
  m_still_since = now_ms;
  m_still_pos = m_pos;
  return false;
}

// T_coast (ms) seen on this turn: how far it went on after the first cut, over the rate at the cut.
uint16_t TurnController::coastReading() const {
  const int32_t rate = m_cut_rate < 0 ? -m_cut_rate : m_cut_rate;
  if (rate < motion::kCoastMinRateDdps) return 0;
  const int32_t coast = (m_delta >= 0 ? m_pos - m_cut_pos : m_cut_pos - m_pos);  // along the turn
  const int32_t ms = coast * 100 / rate;
  return ms >= motion::kCoastMinMs && ms <= motion::kCoastMaxMs ? static_cast<uint16_t>(ms) : 0;
}

bool TurnController::stalledFor(int8_t w, int16_t rate_ddps, uint32_t now_ms) {
  const int16_t r = rate_ddps < 0 ? static_cast<int16_t>(-rate_ddps) : rate_ddps;
  const int8_t aw = w < 0 ? static_cast<int8_t>(-w) : w;
  if (aw < motion::kTurnWMin || r >= motion::kStallRateDdps) {
    m_stalling = false;
    return false;
  }
  if (!m_stalling) {
    m_stalling = true;
    m_stall_since = now_ms;
  }
  return now_ms - m_stall_since >= motion::kStallMs;
}

void TurnController::driven(TurnStep& s, int32_t error, int16_t rate_ddps, uint32_t now_ms) {
  if (m_phase == TurnPhase::kApproach && absI(error) <= motion::kTurnSlowZoneCdeg) {
    m_phase = TurnPhase::kSlow;  // the last 25°: rate control at the slowest smooth spin
    m_rate.reset();
  }
  if (m_phase != TurnPhase::kApproach && cutNow(error, rate_ddps)) {
    cut(rate_ddps, now_ms);
    return;  // w = 0: glide onto the target
  }
  if (m_phase == TurnPhase::kApproach) {
    s.w = command(error, rate_ddps);
  } else {
    const int16_t target = static_cast<int16_t>(m_dir * motion::kSpinMinDdps);
    const uint32_t dt = now_ms - m_last_ms;
    s.w = m_rate.step(target, rate_ddps, static_cast<uint16_t>(dt > 100 ? 100 : dt));
  }
  s.stalled = stalledFor(s.w, rate_ddps, now_ms);
}

void TurnController::coasting(TurnStep& s, int32_t error, uint32_t now_ms) {
  m_stalling = false;
  if (!stopped(now_ms)) return;  // w = 0 while it glides
  if (m_phase == TurnPhase::kCoast) {
    s.coast_ms = coastReading();
    if (absI(error) > motion::kTurnDoneCdeg) {
      m_phase = TurnPhase::kNudge;  // stopped too far off: one nudge, never a second
      m_dir = error < 0 ? -1 : 1;
      m_rate.reset();
      return;
    }
  }
  s.done = true;
}

TurnStep TurnController::step(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms) {
  m_pos += wrapDeltaCdeg(static_cast<int32_t>(yaw_cdeg) - m_last_yaw);
  m_last_yaw = yaw_cdeg;
  const int32_t error = m_delta - m_pos;
  TurnStep s;
  s.timeout = now_ms - m_start_ms >= m_timeout_ms;
  if (m_phase == TurnPhase::kCoast || m_phase == TurnPhase::kNudgeCoast) {
    coasting(s, error, now_ms);
  } else {
    driven(s, error, rate_ddps, now_ms);
  }
  m_last_ms = now_ms;
  return s;
}

uint8_t TurnController::progressPct() const {
  if (m_delta == 0) return 100;
  int32_t p = m_pos * 100 / m_delta;
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return static_cast<uint8_t>(p);
}

}  // namespace core
}  // namespace cq
```

- [ ] **Step 6: Implement — the motion service passes the coast through**

Apply to `firmware/esp32/src/core/motion_service.h` and `.cpp`:

```diff
diff --git a/firmware/esp32/src/core/motion_service.cpp b/firmware/esp32/src/core/motion_service.cpp
index a9f137c..0681691 100644
--- a/firmware/esp32/src/core/motion_service.cpp
+++ b/firmware/esp32/src/core/motion_service.cpp
@@ -6,11 +6,12 @@ namespace cq {
 namespace core {
 
 void MotionService::startTurn(int fd, uint16_t yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, int32_t id,
-                              uint32_t now_ms) {
+                              uint32_t now_ms, uint16_t coast_ms) {
   m_kind = PrimKind::kTurn;
   m_fd = fd;
   m_id = id;
-  m_turn.begin(yaw_cdeg, delta_cdeg, speed_pct, now_ms);
+  m_coast_reading = 0;
+  m_turn.begin(yaw_cdeg, delta_cdeg, speed_pct, now_ms, coast_ms);
   m_last_telem_ms = now_ms;
 }
 
@@ -52,7 +53,7 @@ void MotionService::startArc(int fd, const MotionTelem& t, int32_t delta_cdeg, u
 }
 
 void MotionService::startPivot(int fd, uint16_t yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, int32_t id,
-                               uint32_t now_ms) {
+                               uint32_t now_ms, uint16_t coast_ms) {
   m_kind = PrimKind::kPivot;
   m_fd = fd;
   m_id = id;
@@ -60,7 +61,8 @@ void MotionService::startPivot(int fd, uint16_t yaw_cdeg, int32_t delta_cdeg, ui
   // outside track = 2|w| <= speed; the spin's minimum w still applies (12 % -> outside 24 %)
   uint8_t half = static_cast<uint8_t>(speed_pct / 2);
   if (half < motion::kTurnWMin) half = static_cast<uint8_t>(motion::kTurnWMin);
-  m_turn.begin(yaw_cdeg, delta_cdeg, half, now_ms);
+  m_coast_reading = 0;
+  m_turn.begin(yaw_cdeg, delta_cdeg, half, now_ms, coast_ms);
   m_last_telem_ms = now_ms;
 }
 
@@ -80,6 +82,7 @@ MotionOut MotionService::stepArc(const MotionTelem& t, uint32_t now_ms) {
 
 MotionOut MotionService::stepPivot(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms) {
   const TurnStep s = m_turn.step(yaw_cdeg, rate_ddps, now_ms);
+  if (s.coast_ms != 0) m_coast_reading = s.coast_ms;
   if (s.done) return finish(EndReason::kDone);
   if (s.stalled) return finish(EndReason::kStalled);
   if (s.timeout) return finish(EndReason::kTimeout);
@@ -104,6 +107,7 @@ MotionOut MotionService::finish(EndReason why) {
     o.end.has_moved = true;
     o.end.moved_mm = m_move.movedMm();
   }
+  if (m_kind == PrimKind::kTurn || m_kind == PrimKind::kPivot) o.end.coast_ms = m_coast_reading;
   m_kind = PrimKind::kNone;
   m_fd = -1;
   m_id = -1;
@@ -114,6 +118,7 @@ MotionOut MotionService::cancel(EndReason why) { return active() ? finish(why) :
 
 MotionOut MotionService::stepTurn(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms) {
   const TurnStep s = m_turn.step(yaw_cdeg, rate_ddps, now_ms);
+  if (s.coast_ms != 0) m_coast_reading = s.coast_ms;
   if (s.done) return finish(EndReason::kDone);
   if (s.stalled) return finish(EndReason::kStalled);
   if (s.timeout) return finish(EndReason::kTimeout);
diff --git a/firmware/esp32/src/core/motion_service.h b/firmware/esp32/src/core/motion_service.h
index d128f81..4af24f0 100644
--- a/firmware/esp32/src/core/motion_service.h
+++ b/firmware/esp32/src/core/motion_service.h
@@ -6,6 +6,7 @@
 #include <stdint.h>
 
 #include "core/arc_controller.h"
+#include "core/motion_config.h"
 #include "core/move_controller.h"
 #include "core/rate_loop.h"
 #include "core/turn_controller.h"
@@ -35,6 +36,7 @@ struct PrimitiveEnd {
   int32_t id = -1;
   bool has_moved = false;  // Plan K: a move reports its counted distance
   int32_t moved_mm = 0;
+  uint16_t coast_ms = 0;  // R18: a turn / pivot's coast reading (ms), 0 = none
 };
 
 struct MotionOut {
@@ -47,14 +49,17 @@ struct MotionOut {
 
 class MotionService {
  public:
-  void startTurn(int fd, uint16_t yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, int32_t id, uint32_t now_ms);
+  // coast_ms: the learned T_coast (R18).
+  void startTurn(int fd, uint16_t yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, int32_t id, uint32_t now_ms,
+                 uint16_t coast_ms = motion::kCoastDefaultMs);
   void driveRate(int fd, int8_t v, int16_t rate_ddps, uint32_t now_ms);  // starts or updates the stream
   MotionOut cancel(EndReason why);
   void startMove(int fd, const MotionTelem& t, int16_t dist_cm, uint8_t speed_pct, uint16_t cpm, int32_t id,
                  uint32_t now_ms, uint16_t roll_mm = 0);
   void startArc(int fd, const MotionTelem& t, int32_t delta_cdeg, uint16_t radius_cm, uint8_t speed_pct, uint16_t cpm,
                 int32_t id, uint32_t now_ms);
-  void startPivot(int fd, uint16_t yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, int32_t id, uint32_t now_ms);
+  void startPivot(int fd, uint16_t yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, int32_t id, uint32_t now_ms,
+                  uint16_t coast_ms = motion::kCoastDefaultMs);
   MotionOut onTelemetry(const MotionTelem& t, uint32_t now_ms);
   MotionOut onTelemetry(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms) {  // turn / rate callers
     MotionTelem t;
@@ -88,6 +93,7 @@ class MotionService {
   MoveController m_move;
   ArcController m_arc;
   int8_t m_pivot_dir = 1;  // +1 left pivot (left track still), -1 right pivot
+  uint16_t m_coast_reading = 0;  // R18: kept until the turn / pivot ends
   int8_t m_rate_v = 0;
   int16_t m_rate_target = 0;
   bool m_rate_limited = false;
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_motion`
Expected: PASS, 61 test cases. The callers in `app/` still compile, because `coast_ms` defaults to 60 ms.

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native`
Expected: PASS (all suites).

- [ ] **Step 8: Commit**

```bash
git add firmware/esp32/src/core/motion_config.h firmware/esp32/src/core/turn_controller.h firmware/esp32/src/core/turn_controller.cpp \
        firmware/esp32/src/core/motion_service.h firmware/esp32/src/core/motion_service.cpp firmware/esp32/test/support/tank_model.h \
        firmware/esp32/test/test_motion/test_main.cpp
git commit -m "feat(bridge): R18 turns - slow to the slowest smooth spin, cut at the predicted stop, one nudge

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Learn and keep the coast (ports, NVS, app)

**Files:**
- Modify: `firmware/esp32/src/ports/motion_store.h`, `firmware/esp32/src/net/motion_store.h`, `firmware/esp32/src/net/motion_store.cpp`, `firmware/esp32/src/net/ports_esp32.cpp`
- Modify: `firmware/esp32/src/fakes/fake_ports.h`, `firmware/esp32/src/fakes/fake_ports.cpp`
- Modify: `firmware/esp32/src/app/motion_controller.h`, `firmware/esp32/src/app/motion_controller.cpp`
- Modify: `firmware/esp32/src/app/settings_controller.h`, `firmware/esp32/src/app/settings_controller.cpp`, `firmware/esp32/src/net/link_task.cpp`
- Test: `firmware/esp32/test/test_motion_controller/test_main.cpp`, `firmware/esp32/test/test_settings_controller/test_main.cpp`, `firmware/esp32/test/test_command_router/test_main.cpp`

**Interfaces:**
- Consumes (Task 1): `core::coastAverage`, `PrimitiveEnd::coast_ms`, `MotionService::startTurn/startPivot(..., coast_ms)`, `motion::kCoastDefaultMs / kCoastMinMs / kCoastMaxMs`.
- Produces:
  - `ports::loadCoast()` / `ports::saveCoast(uint16_t)`
  - `net::motionStoreLoadCoast()` / `motionStoreSaveCoast(uint16_t)`
  - `fakes::World::coast` (default 60) and `saved_coast`
  - `MotionController::coastMs()` (`uint16_t&`)
  - `SettingsController::Hooks::coast_ms` (`uint16_t*`, last member, default `nullptr`)

- [ ] **Step 1: Write the failing tests**

Apply to the three test files:

```diff
diff --git a/firmware/esp32/test/test_command_router/test_main.cpp b/firmware/esp32/test/test_command_router/test_main.cpp
index e8a0da1..76d9dde 100644
--- a/firmware/esp32/test/test_command_router/test_main.cpp
+++ b/firmware/esp32/test/test_command_router/test_main.cpp
@@ -69,7 +69,7 @@ void setUp() {
   tel->setViews({[] { return drive->ownerView(); }, [] { return motion->view(); }, [] { return !link->synced(); }});
   settings->setHooks({[] { return motion->active(); }, [] { return drive->otaActive(); }, [] { return owner->driverFd(); },
                       [](int fd) { motion->sendCalib(fd); }, [](const core::AckedOp& op) { return uno->request(op); },
-                      &motion->cpm(), &motion->rollMm(), [] { return uno->freeSlots(); }});
+                      &motion->cpm(), &motion->rollMm(), [] { return uno->freeSlots(); }, &motion->coastMs()});
   g_post = 0;
   drive->begin(0);
   drive->onClientOpen(3, 0);
diff --git a/firmware/esp32/test/test_motion_controller/test_main.cpp b/firmware/esp32/test/test_motion_controller/test_main.cpp
index f0b3b7a..5e14fd6 100644
--- a/firmware/esp32/test/test_motion_controller/test_main.cpp
+++ b/firmware/esp32/test/test_motion_controller/test_main.cpp
@@ -1,4 +1,4 @@
-// MotionController: turn / move / drive_rate / cancel, primitive ends, counts per metre (bridge LLD R4, R7, R8, R15.4).
+// MotionController: turn / move / drive_rate / cancel, primitive ends, counts per metre, the learned turn coast (bridge LLD R4, R7, R8, R15.4, R18).
 #include <string.h>
 #include <unity.h>
 
@@ -157,6 +157,43 @@ void test_cancel_by_another_client_is_not_owner() {
   TEST_ASSERT_FALSE(mc->active());
 }
 
+// R18.2: a done turn reports its coast reading; the controller averages it in and saves it.
+namespace {
+void yawFrame(uint16_t yaw, int16_t rate, uint32_t now) {
+  proto::TelemFastMsg f{};
+  f.yaw = yaw;
+  f.yaw_rate = rate;
+  uint8_t b[64];
+  const size_t n = cqtest::frame(f, 3, b);
+  unoSends(b, n, now);
+  mc->onTelemetry(now);
+}
+}  // namespace
+
+void test_a_done_turn_saves_the_learned_coast() {
+  TEST_ASSERT_EQUAL(60, mc->coastMs());  // loaded at begin
+  syncedWithEncoders();
+  mc->onTurn(3, 1, P("{\"t\":\"turn\",\"by\":90,\"speed\":40,\"id\":1}"), 100);
+  TEST_ASSERT_TRUE(mc->active());
+  uint32_t t = 100;
+  yawFrame(8700, 650, t += 20);  // cut at 87° turning 65 °/s
+  for (int i = 0; i < 30 && mc->active(); ++i) yawFrame(8990, 0, t += 20);  // glided 2.9°, stopped
+  TEST_ASSERT_FALSE(mc->active());
+  TEST_ASSERT_TRUE(has(3, "primitive_done"));
+  TEST_ASSERT_EQUAL(1, static_cast<int>(fakes::world().saved_coast.size()));
+  TEST_ASSERT_EQUAL(56, fakes::world().saved_coast[0]);  // (3 x 60 + 44 + 2) / 4
+  TEST_ASSERT_EQUAL(56, mc->coastMs());
+}
+
+void test_a_cancelled_turn_learns_nothing() {
+  syncedWithEncoders();
+  mc->onTurn(3, 1, P("{\"t\":\"turn\",\"by\":90,\"speed\":40,\"id\":1}"), 100);
+  yawFrame(8700, 650, 120);
+  mc->end(core::MotionEvent::kCancel, 140);
+  TEST_ASSERT_EQUAL(0, static_cast<int>(fakes::world().saved_coast.size()));
+  TEST_ASSERT_EQUAL(60, mc->coastMs());
+}
+
 void test_calib_reply_has_counts_and_roll_diagnostics() {
   mc->sendCalib(3);
   const std::string j = fakes::lastTo(3);
@@ -261,6 +298,8 @@ int main() {
   RUN_TEST(test_move_ended_by_stop_is_broadcast_and_logged_without_a_second_stop);
   RUN_TEST(test_cancelled_move_sends_stop);
   RUN_TEST(test_cancel_by_another_client_is_not_owner);
+  RUN_TEST(test_a_done_turn_saves_the_learned_coast);
+  RUN_TEST(test_a_cancelled_turn_learns_nothing);
   RUN_TEST(test_calib_reply_has_counts_and_roll_diagnostics);
   RUN_TEST(test_logo_move_starts_without_a_reply_and_its_end_reaches_the_program);
   RUN_TEST(test_a_page_s_own_primitive_end_does_not_reach_the_program);
diff --git a/firmware/esp32/test/test_settings_controller/test_main.cpp b/firmware/esp32/test/test_settings_controller/test_main.cpp
index 8d4f6eb..1ce1107 100644
--- a/firmware/esp32/test/test_settings_controller/test_main.cpp
+++ b/firmware/esp32/test/test_settings_controller/test_main.cpp
@@ -25,7 +25,7 @@ int g_driver = -1;
 int g_calib_sent = 0;
 std::vector<core::AckedOp> g_ops;
 size_t g_free = 8;  // free request slots the fake queue reports
-uint16_t g_cpm = 7514, g_roll = 27;
+uint16_t g_cpm = 7514, g_roll = 27, g_coast = 80;
 
 core::WsCommand P(const char* s) { return core::parseWsCommand(s, strlen(s)); }
 bool has(int fd, const char* needle) { return fakes::countTo(fd, needle) > 0; }
@@ -46,6 +46,7 @@ void setUp() {
   g_free = 8;
   g_cpm = 7514;
   g_roll = 27;
+  g_coast = 80;
   sc->setHooks({[] { return g_active; }, [] { return g_ota; }, [] { return g_driver; },
                      [](int) { ++g_calib_sent; },
                      [](const core::AckedOp& op) {
@@ -54,7 +55,7 @@ void setUp() {
                        g_ops.push_back(op);
                        return true;
                      },
-                     &g_cpm, &g_roll, [] { return g_free; }});
+                     &g_cpm, &g_roll, [] { return g_free; }, &g_coast});
 }
 void tearDown() {}
 
@@ -140,6 +141,17 @@ void test_calib_save_failure_is_busy() {
   TEST_ASSERT_TRUE(has(3, "BUSY"));
 }
 
+// R18.2: resetting the roll-on (confirmed on the tank) also resets the learned turn coast.
+void test_calib_reset_roll_also_resets_the_coast() {
+  sc->onCalib(3, 5, P("{\"t\":\"calib\",\"reset\":\"roll\",\"id\":5}"), 1000);
+  TEST_ASSERT_TRUE(has(3, "\"t\":\"confirm\""));
+  sc->onButtonPress(2000);
+  TEST_ASSERT_EQUAL(core::motion::kRollDefaultMm, g_roll);
+  TEST_ASSERT_EQUAL(core::motion::kCoastDefaultMs, g_coast);
+  TEST_ASSERT_EQUAL(1, static_cast<int>(fakes::world().saved_coast.size()));
+  TEST_ASSERT_EQUAL(core::motion::kCoastDefaultMs, fakes::world().saved_coast[0]);
+}
+
 void test_calib_read_sends_calib() {
   sc->onCalib(3, -1, P("{\"t\":\"calib\"}"), 1000);
   TEST_ASSERT_EQUAL(1, g_calib_sent);
@@ -181,6 +193,7 @@ int main() {
   RUN_TEST(test_calib_set_only_for_the_driver_and_not_while_moving);
   RUN_TEST(test_calib_save_failure_is_busy);
   RUN_TEST(test_calib_read_sends_calib);
+  RUN_TEST(test_calib_reset_roll_also_resets_the_coast);
   RUN_TEST(test_wifi_mode_result_is_logged);
   RUN_TEST(test_restart_is_acked_logged_and_due_after_500_ms);
   RUN_TEST(test_restart_refused_while_moving);
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_motion_controller -f test_settings_controller -f test_command_router`
Expected: FAIL to compile — `coastMs()` not a member of `MotionController`, `saved_coast` not a member of `World`, too many initializers for `Hooks`.

- [ ] **Step 3: Implement**

Apply:

```diff
diff --git a/firmware/esp32/src/app/motion_controller.cpp b/firmware/esp32/src/app/motion_controller.cpp
index 966cb36..716924e 100644
--- a/firmware/esp32/src/app/motion_controller.cpp
+++ b/firmware/esp32/src/app/motion_controller.cpp
@@ -30,6 +30,7 @@ core::MotionTelem telemOf(const core::TankSnapshot& s) {
 void MotionController::begin() {
   m_cpm = ports::loadCpm();  // Plan K
   m_roll_mm = ports::loadRoll();  // spec 02 §G6
+  m_coast_ms = ports::loadCoast();  // bridge LLD R18.2
 }
 
 bool MotionController::encoderFault() const {
@@ -61,11 +62,24 @@ void MotionController::run(const core::LinkOut& o, uint32_t now) {
   if (o.end.reason != core::EndReason::kDone) {  // Plan O: why a move / turn ended early
     m_log.add(core::LogKind::kPrimitive, static_cast<int32_t>(o.end.reason), static_cast<int32_t>(o.end.kind), "",
               m_tel.tankMs(), now);
-  } else if (o.end.kind == core::PrimKind::kMove) {
-    const core::TankSnapshot& s = m_link.snapshot();  // §G6: watch the roll-on from here
+  } else {
+    learnFrom(o.end, now);
+  }
+}
+
+// A done move: watch its roll-on (§G6). A done turn / pivot with a coast reading: average it in and save it (R18.2).
+void MotionController::learnFrom(const core::PrimitiveEnd& e, uint32_t now) {
+  if (e.kind == core::PrimKind::kMove) {
+    const core::TankSnapshot& s = m_link.snapshot();
     m_roll.begin((s.fast.enc_l + s.fast.enc_r) / 2, now);
     ++m_roll_begun;
+    return;
   }
+  if (e.coast_ms == 0) return;
+  const uint16_t next = core::coastAverage(m_coast_ms, e.coast_ms);
+  ports::logLine("motion: turn coast %u ms -> average %u ms", static_cast<unsigned>(e.coast_ms),
+                 static_cast<unsigned>(next));
+  if (next != m_coast_ms && ports::saveCoast(next)) m_coast_ms = next;
 }
 
 void MotionController::end(core::MotionEvent e, uint32_t now) {
@@ -101,8 +115,8 @@ bool MotionController::startLogo(int fd, const logo::Action& a, uint32_t now, co
     case logo::ActKind::kArc:
       svc.startArc(fd, telemOf(snap), a.delta_cdeg, a.radius_cm, a.speed_pct, m_cpm, kLogoPrimitive, now);
       return true;
-    case logo::ActKind::kPivot: svc.startPivot(fd, snap.fast.yaw, a.delta_cdeg, spin, kLogoPrimitive, now); return true;
-    default: svc.startTurn(fd, snap.fast.yaw, a.delta_cdeg, spin, kLogoPrimitive, now); return true;  // turn, HOME
+    case logo::ActKind::kPivot: svc.startPivot(fd, snap.fast.yaw, a.delta_cdeg, spin, kLogoPrimitive, now, m_coast_ms); return true;
+    default: svc.startTurn(fd, snap.fast.yaw, a.delta_cdeg, spin, kLogoPrimitive, now, m_coast_ms); return true;  // turn, HOME
   }
 }
 
@@ -154,9 +168,9 @@ void MotionController::onTurn(int fd, int32_t id, const core::WsCommand& c, uint
   if (c.radius_cm > 0) {
     svc.startArc(fd, telemOf(snap), delta, c.radius_cm, c.speed, m_cpm, id, now);
   } else if (c.pivot) {
-    svc.startPivot(fd, snap.fast.yaw, delta, c.speed, id, now);
+    svc.startPivot(fd, snap.fast.yaw, delta, c.speed, id, now, m_coast_ms);
   } else {
-    svc.startTurn(fd, snap.fast.yaw, delta, c.speed, id, now);
+    svc.startTurn(fd, snap.fast.yaw, delta, c.speed, id, now, m_coast_ms);
   }
   m_out.ack(fd, id);
 }
diff --git a/firmware/esp32/src/app/motion_controller.h b/firmware/esp32/src/app/motion_controller.h
index 89323f6..1fc692e 100644
--- a/firmware/esp32/src/app/motion_controller.h
+++ b/firmware/esp32/src/app/motion_controller.h
@@ -50,6 +50,7 @@ class MotionController {
   void cancelRollOn() { m_roll.cancel(); }  // driving again / a new primitive
   uint16_t& cpm() { return m_cpm; }
   uint16_t& rollMm() { return m_roll_mm; }
+  uint16_t& coastMs() { return m_coast_ms; }  // R18.2
   void sendCalib(int fd);
   core::PrimitiveView view() const;
 
@@ -62,6 +63,7 @@ class MotionController {
   bool rateRefused(int fd, uint32_t now, core::BridgeReason& why);
   bool claimed(int fd, int32_t id, uint32_t now);
   void observeRoll(uint32_t now);
+  void learnFrom(const core::PrimitiveEnd& e, uint32_t now);
   bool encoderFault() const;
   bool logoRefused(const logo::Action& a, core::BridgeReason& why) const;
 
@@ -74,6 +76,7 @@ class MotionController {
   Hooks m_h{nullptr, nullptr, nullptr, nullptr, nullptr};
   uint16_t m_cpm = 0;
   uint16_t m_roll_mm = 0;
+  uint16_t m_coast_ms = core::motion::kCoastDefaultMs;
   int32_t m_roll_last = -1;  // diagnostics in the calib reply: last reading (mm, -1 none) ...
   uint16_t m_roll_begun = 0;  // ... moves watched
   uint16_t m_roll_read = 0;   // ... readings taken
diff --git a/firmware/esp32/src/app/settings_controller.cpp b/firmware/esp32/src/app/settings_controller.cpp
index db72bcb..68e84d6 100644
--- a/firmware/esp32/src/app/settings_controller.cpp
+++ b/firmware/esp32/src/app/settings_controller.cpp
@@ -207,6 +207,10 @@ void SettingsController::applyCalibReset(const core::PendingChange& ch) {
   } else {
     *m_h.roll_mm = core::motion::kRollDefaultMm;
     if (!ports::saveRoll(*m_h.roll_mm)) ports::logLine("calib: roll-on reset not saved (NVS)");
+    if (m_h.coast_ms != nullptr) {  // R18.2: the turn coast is learned the same way
+      *m_h.coast_ms = core::motion::kCoastDefaultMs;
+      if (!ports::saveCoast(*m_h.coast_ms)) ports::logLine("calib: turn coast reset not saved (NVS)");
+    }
   }
   m_out.to(ch.fd, core::jsonConfirmed(m_out.buf(), m_out.cap(), core::changeName(ch.kind)));
   m_h.send_calib(ch.fd);
diff --git a/firmware/esp32/src/app/settings_controller.h b/firmware/esp32/src/app/settings_controller.h
index 1a815eb..70ec970 100644
--- a/firmware/esp32/src/app/settings_controller.h
+++ b/firmware/esp32/src/app/settings_controller.h
@@ -26,6 +26,7 @@ class SettingsController {
     uint16_t* cpm;      // counts per metre in use
     uint16_t* roll_mm;  // learned roll-on in use
     size_t (*free_slots)();  // free Uno request slots (shared with the phones' requests, R15.5)
+    uint16_t* coast_ms = nullptr;  // R18.2: learned turn coast in use (reset with the roll-on)
   };
 
   SettingsController(Replies& out, core::EventLog& log, const core::BodyLink& link)
diff --git a/firmware/esp32/src/fakes/fake_ports.cpp b/firmware/esp32/src/fakes/fake_ports.cpp
index cd57244..dcc2a58 100644
--- a/firmware/esp32/src/fakes/fake_ports.cpp
+++ b/firmware/esp32/src/fakes/fake_ports.cpp
@@ -67,6 +67,13 @@ bool saveRoll(uint16_t v) {
   world().roll = v;
   return true;
 }
+uint16_t loadCoast() { return world().coast; }
+bool saveCoast(uint16_t v) {
+  if (!world().save_ok) return false;
+  world().saved_coast.push_back(v);
+  world().coast = v;
+  return true;
+}
 bool loadAiUrl(char* out, size_t cap) {
   snprintf(out, cap, "%s", world().ai_url.c_str());
   return true;
diff --git a/firmware/esp32/src/fakes/fake_ports.h b/firmware/esp32/src/fakes/fake_ports.h
index 6052132..53c4525 100644
--- a/firmware/esp32/src/fakes/fake_ports.h
+++ b/firmware/esp32/src/fakes/fake_ports.h
@@ -24,9 +24,11 @@ struct World {
   bool close_ok = true;
   uint16_t cpm = 7514;
   uint16_t roll = 27;
+  uint16_t coast = 60;
   bool save_ok = true;
   std::vector<uint16_t> saved_cpm;
   std::vector<uint16_t> saved_roll;
+  std::vector<uint16_t> saved_coast;
   std::string ai_url;
   std::vector<core::WifiRequest> wifi_posted;
   bool wifi_post_ok = true;
diff --git a/firmware/esp32/src/net/link_task.cpp b/firmware/esp32/src/net/link_task.cpp
index 5be4f32..3c5690c 100644
--- a/firmware/esp32/src/net/link_task.cpp
+++ b/firmware/esp32/src/net/link_task.cpp
@@ -200,7 +200,7 @@ void wireSettingsAndTank() {
   g_settings.setHooks({[] { return g_motion.active(); }, [] { return g_drive.otaActive(); },
                        [] { return g_owner.driverFd(); }, [](int fd) { g_motion.sendCalib(fd); },
                        [](const core::AckedOp& op) { return g_uno.request(op); }, &g_motion.cpm(), &g_motion.rollMm(),
-                       [] { return g_uno.freeSlots(); }});
+                       [] { return g_uno.freeSlots(); }, &g_motion.coastMs()});
   g_tank.setSelfTestHooks({[] { return !g_link.synced(); }, [] { return !standingStill(); }, queueSelfTest});
 }
 
diff --git a/firmware/esp32/src/net/motion_store.cpp b/firmware/esp32/src/net/motion_store.cpp
index 414d6ba..a6ba225 100644
--- a/firmware/esp32/src/net/motion_store.cpp
+++ b/firmware/esp32/src/net/motion_store.cpp
@@ -51,5 +51,26 @@ bool motionStoreSaveRoll(uint16_t mm) {
   return ok;
 }
 
+uint16_t motionStoreLoadCoast() {
+  uint16_t v = core::motion::kCoastDefaultMs;
+  Preferences p;
+  if (p.begin("cq_motion", true)) {
+    const uint16_t saved = p.getUShort("coast_ms", 0xFFFF);
+    if (saved >= core::motion::kCoastMinMs && saved <= core::motion::kCoastMaxMs) v = saved;
+    p.end();
+  }
+  logf("motion: turn coast %u ms", static_cast<unsigned>(v));
+  return v;
+}
+
+bool motionStoreSaveCoast(uint16_t ms) {
+  if (ms < core::motion::kCoastMinMs || ms > core::motion::kCoastMaxMs) return false;
+  Preferences p;
+  if (!p.begin("cq_motion", false)) return false;
+  const bool ok = p.putUShort("coast_ms", ms) == sizeof(uint16_t);
+  p.end();
+  return ok;
+}
+
 }  // namespace net
 }  // namespace cq
diff --git a/firmware/esp32/src/net/motion_store.h b/firmware/esp32/src/net/motion_store.h
index 0e6d3e1..2da3716 100644
--- a/firmware/esp32/src/net/motion_store.h
+++ b/firmware/esp32/src/net/motion_store.h
@@ -10,6 +10,8 @@ uint16_t motionStoreLoad();             // the saved value, or motion::kCpmDefau
 bool motionStoreSave(uint16_t cpm);     // false when out of range or the flash write failed
 uint16_t motionStoreLoadRoll();         // spec 02 §G6: learned roll-on (mm), default 25, 0..80
 bool motionStoreSaveRoll(uint16_t mm);
+uint16_t motionStoreLoadCoast();        // bridge LLD R18.2: learned turn coast (ms), default 60, 20..400
+bool motionStoreSaveCoast(uint16_t ms);
 
 }  // namespace net
 }  // namespace cq
diff --git a/firmware/esp32/src/net/ports_esp32.cpp b/firmware/esp32/src/net/ports_esp32.cpp
index 56af97b..c692844 100644
--- a/firmware/esp32/src/net/ports_esp32.cpp
+++ b/firmware/esp32/src/net/ports_esp32.cpp
@@ -35,6 +35,8 @@ uint16_t loadCpm() { return net::motionStoreLoad(); }
 bool saveCpm(uint16_t v) { return net::motionStoreSave(v); }
 uint16_t loadRoll() { return net::motionStoreLoadRoll(); }
 bool saveRoll(uint16_t v) { return net::motionStoreSaveRoll(v); }
+uint16_t loadCoast() { return net::motionStoreLoadCoast(); }
+bool saveCoast(uint16_t v) { return net::motionStoreSaveCoast(v); }
 
 bool loadAiUrl(char* out, size_t cap) { return net::appAiUrlLoad(out, cap); }
 bool saveAiUrl(const char* url) { return net::appAiUrlSave(url); }
diff --git a/firmware/esp32/src/ports/motion_store.h b/firmware/esp32/src/ports/motion_store.h
index a6ded94..dcb8de3 100644
--- a/firmware/esp32/src/ports/motion_store.h
+++ b/firmware/esp32/src/ports/motion_store.h
@@ -1,4 +1,4 @@
-// Port: counts per metre and the learned roll-on in flash (bridge LLD R15.3, spec 02 §G6).
+// Port: counts per metre, the learned roll-on and the learned turn coast in flash (bridge LLD R15.3, spec 02 §G6).
 #pragma once
 
 #include <stdint.h>
@@ -10,6 +10,8 @@ uint16_t loadCpm();         // the saved value, or the default when missing / ou
 bool saveCpm(uint16_t v);   // false: out of range or the flash write failed
 uint16_t loadRoll();        // mm
 bool saveRoll(uint16_t v);
+uint16_t loadCoast();       // R18.2: learned turn coast (ms)
+bool saveCoast(uint16_t v);
 
 }  // namespace ports
 }  // namespace cq
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_motion_controller -f test_settings_controller -f test_command_router`
Expected: PASS.

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native`
Expected: PASS, 598 test cases.

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio run -e bridge`
Expected: SUCCESS (flash ~44 %).

Run: `uv run --with lizard==1.24.1 python scripts/size_check.py`
Expected: no output, exit 0. (`MotionController::run` stays at complexity ≤ 10 because the learning lives in `learnFrom`.)

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/ports/motion_store.h firmware/esp32/src/net/motion_store.h firmware/esp32/src/net/motion_store.cpp firmware/esp32/src/net/ports_esp32.cpp \
        firmware/esp32/src/fakes/fake_ports.h firmware/esp32/src/fakes/fake_ports.cpp firmware/esp32/src/app/motion_controller.h \
        firmware/esp32/src/app/motion_controller.cpp firmware/esp32/src/app/settings_controller.h firmware/esp32/src/app/settings_controller.cpp \
        firmware/esp32/src/net/link_task.cpp firmware/esp32/test/test_motion_controller/test_main.cpp \
        firmware/esp32/test/test_settings_controller/test_main.cpp firmware/esp32/test/test_command_router/test_main.cpp
git commit -m "feat(bridge): learn the turn coast like the roll-on, keep it in NVS, reset it with the roll-on

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: On the tank — flash and floor check (owner present)

**Files:** none (the results go into Task 4).

Safety: this is a **floor** check, so the tracks move. Ask the owner "stand or floor?" before every drive command and say what moves. Keep STOP within reach (the phone's STOP or the IR remote). The camera stays on. Only the bridge is flashed; the protocol is unchanged, so the Uno is not.

- [ ] **Step 1: Flash the bridge over the air**

```bash
cd firmware/esp32
export CQ_OTA_PASSWORD="$(sed -n 's/.*CQ_OTA_PASSWORD[^"]*"\([^"]*\)".*/\1/p' include/secrets.h)"
~/.platformio/penv/bin/pio run -e bridge_ota -t upload --upload-port 192.168.31.167 2>&1 | grep -vF -- "$CQ_OTA_PASSWORD" | tail -5
unset CQ_OTA_PASSWORD
```
Expected: `SUCCESS`. The bridge reboots and the phone page reconnects. The serial / Log shows `motion: turn coast 60 ms` at boot.

- [ ] **Step 2: Record the turns (floor; the tank spins in place)**

The owner places the tank on the floor with ~40 cm clear around it. Run the §112 recording, `uv run cq-record --no-view` (50 Hz `td`), while sending with `tools/smoke/turn.py`:
RIGHT 90 ×3, LEFT 90 ×3, a 360°, then `/logo` `REPEAT 4 [FORWARD 30 RIGHT 90]`.
Expected per turn: the rate falls to ~65 °/s in the last 25°; one cut; at most one nudge; no alternating pushes; it ends within ±3° (within ±1–2° once a few turns have been learned). No 7044 in the square. The Log shows `motion: turn coast N ms -> average M ms` after each done turn.

- [ ] **Step 3: Note the numbers**

For each turn, note: the final error (°), the number of pushes, the coast reading and the average, and the time from the cut to done. Include how each number was read (the recording's `td` columns, the Log lines).

---

### Task 4: Docs as built

**Files:**
- Modify: `docs/lld/esp32-bridge-lld.md` (R18.6 as built + floor results)
- Modify: `docs/design-notes.md` (new § after §112: the floor results, the numbers with how they were read)
- Modify: `CLAUDE.md` (the plan list and the roadmap line: Plan R1 done / floor-checked)

- [ ] **Step 1: Write R18.6 "As built"**

Cover: the files changed; 598 host tests; the realistic tank model (deadband lift to 50 °/s from 8 %, +1.4 °/s per %, a 1000 °/s² glide); the floor numbers from Task 3 Step 3 with how each was read; any change made on the tank.

- [ ] **Step 2: Add the design-notes section and update CLAUDE.md**

- [ ] **Step 3: Commit**

```bash
git add docs/lld/esp32-bridge-lld.md docs/design-notes.md CLAUDE.md
git commit -m "docs: Plan R1 smooth turns - as built and floor results

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```


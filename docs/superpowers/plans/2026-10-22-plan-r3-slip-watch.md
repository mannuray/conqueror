# Plan R3 — Pushing but not moving (bridge LLD R21) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A straight drive (`FORWARD`, `BACK`, `GO`) that is held by something the distance sensor cannot see, with one track slipping against it, ends **blocked** after ~1.5 s instead of pushing until its timeout or the 5-minute limit (design-notes §119).

**Architecture:**
- A pure `SlipWatch` (`core/`) compares, every 0.5 s, the turn the tracks' speed difference should cause, `(v_right − v_left) / track width`, with the gyro's measured rate.
- `|expected| ≥ 30 °/s` while the gyro sees < 1/3 of it, for 1.5 s, means held.
- `MoveController` and the GO primitive in `MotionService` each own one, and end `kBlocked` when it says held.
- Turns, pivots, arcs and DRIVE_RATE are untouched; the Uno is unchanged.

**Tech Stack:** C++17 (ESP32-S3 Arduino 3.x, PlatformIO `bridge`, `bridge_ota`, `native`), Unity host tests.

**Spec:** `docs/lld/esp32-bridge-lld.md` Revision 21 (approved 2026-10-22); the finding is design-notes §119.

## Global Constraints
- `kSlipWindowMs` 500, `kSlipExpectedDdps` 300 (30 °/s), `kSlipHoldMs` 1500, `B` = `kTrackWidthCm` 19.
- Track speeds = counts moved in the window ÷ `counts_per_m` × 100 cm/m × 1000 / window ms. They are signed by the drive's direction (the Uno's counts rise both ways). Expected rate = (v_r − v_l) / B × 57.3 °/rad, in 0.1 °/s; measured = the mean gyro rate over the window.
- Held = slipping windows without a break for ≥ 1.5 s, counted from the start of the first one. Its ending is `kBlocked` (`BLOCKED?` TRUE in a program that uses it, else 7024).
- A GO gets the `counts_per_m` in use: `startGo(..., uint16_t cpm = motion::kCpmDefault)`; Logo passes `m_cpm`.
- No change to turns, pivots, arcs, DRIVE_RATE, the Uno, the protocol or any safety rule; no heap.
- Conventional Commits ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; branch `plan-r3` from `main`.

## Review Focus
1. **A slow crawl at the end of a MOVE** (`kMoveCrawlPct`) and the Uno's straight-driving corrections must never look like a slip. At the crawl both tracks are slow, so the expected rate stays far below 30 °/s. Pinned by `test_equal_tracks_are_never_held` and all the existing move tests still passing.
2. **A real gentle curve,** for example the heading hold bringing the tank back on line, must never be held: the gyro agrees with the tracks. Pinned by `test_a_real_curve_is_never_held`.
3. **Reversing** must use the drive's direction for the sign, or a reverse curve looks like a slip. Pinned by `test_backing_up_with_a_slipping_track_is_held` (both parts).
4. **An Uno restart mid-drive** (the counts drop) gives one window with a huge difference. It can only make that one window "slipping"; the drive already stalls on the falling counts (MOVE) and the link events end it. The reviewer checks that one window can never reach the 1.5 s hold on its own.
5. **The gyro scale (R19) and a scaled rate:** `rate_ddps` is the corrected one, and `expected` uses the counts. A 3 % scale changes the measured value by 3 %, far inside the "< 1/3" margin.

---

### Task 1: SlipWatch

**Files:**
- Create: `firmware/esp32/src/core/slip_watch.h`, `firmware/esp32/src/core/slip_watch.cpp`
- Modify: `firmware/esp32/src/core/motion_config.h`
- Test: `firmware/esp32/test/test_slip_watch/test_main.cpp`

**Interfaces:**
- Produces:
  - `cq::core::SlipWatch` with `void reset()` and `bool step(int32_t enc_l, int32_t enc_r, int16_t rate_ddps, uint16_t cpm, int8_t dir, uint32_t now_ms)` (true once held)
  - `motion::kSlipWindowMs`, `kSlipExpectedDdps`, `kSlipHoldMs`

- [ ] **Step 1: Write the failing test** — create `firmware/esp32/test/test_slip_watch/test_main.cpp`:

```cpp
// SlipWatch (bridge LLD R21): a track difference the gyro does not see means a slipping track - the tank is held.
#include <unity.h>

#include "core/slip_watch.h"

using cq::core::SlipWatch;

void setUp() {}
void tearDown() {}

namespace {
constexpr uint16_t kCpm = 7517;
// Drives `ms` of 20 ms frames: each frame adds the counts per second / 50 to each track; returns when first held
// (ms since `from`), or -1.
int32_t drive(SlipWatch& w, uint32_t from, uint32_t ms, int32_t l_per_s, int32_t r_per_s, int16_t gyro_ddps,
              int8_t dir = 1) {
  int32_t l = 0, r = 0;
  for (uint32_t t = 0; t <= ms; t += 20) {
    if (w.step(l, r, gyro_ddps, kCpm, dir, from + t)) return static_cast<int32_t>(t);
    l += l_per_s / 50;
    r += r_per_s / 50;
  }
  return -1;
}
}  // namespace

// Design-notes §119: a track against a table leg - left 1750 counts/s, right 330, the gyro 0 -> held after 1.5 s.
void test_the_table_leg_is_held_after_one_and_a_half_seconds() {
  SlipWatch w;
  w.reset();
  const int32_t at = drive(w, 1000, 4000, 1750, 330, 0);
  TEST_ASSERT_TRUE_MESSAGE(at >= 1500, "held too early");
  TEST_ASSERT_TRUE_MESSAGE(at <= 2100, "held too late");
}

void test_a_real_curve_is_never_held() {  // the same tracks, and the gyro turns as they say (-57 deg/s)
  SlipWatch w;
  w.reset();
  TEST_ASSERT_EQUAL_INT32(-1, drive(w, 1000, 4000, 1750, 330, -570));
}

void test_equal_tracks_are_never_held() {
  SlipWatch w;
  w.reset();
  TEST_ASSERT_EQUAL_INT32(-1, drive(w, 1000, 4000, 1750, 1720, 0));  // 2 % apart: ~1.6 deg/s expected
}

void test_a_short_slip_is_not_held() {  // 1 s of slip, then free again - the counts run on
  SlipWatch w;
  w.reset();
  int32_t l = 0, r = 0;
  for (uint32_t t = 0; t <= 3000; t += 20) {
    TEST_ASSERT_FALSE(w.step(l, r, 0, kCpm, 1, 1000 + t));
    l += 35;
    r += t < 1000 ? 6 : 35;
  }
}

void test_backing_up_with_a_slipping_track_is_held() {
  SlipWatch w;
  w.reset();
  const int32_t at = drive(w, 1000, 4000, 330, 1750, 0, -1);  // counts rise in reverse too
  TEST_ASSERT_TRUE(at >= 1500 && at <= 2100);
  SlipWatch v;  // reversing with the gyro turning as the tracks say (reverse flips the turn's sign)
  v.reset();
  TEST_ASSERT_EQUAL_INT32(-1, drive(v, 1000, 4000, 330, 1750, -570, -1));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_table_leg_is_held_after_one_and_a_half_seconds);
  RUN_TEST(test_a_real_curve_is_never_held);
  RUN_TEST(test_equal_tracks_are_never_held);
  RUN_TEST(test_a_short_slip_is_not_held);
  RUN_TEST(test_backing_up_with_a_slipping_track_is_held);
  return UNITY_END();
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_slip_watch`
Expected: FAIL — `'core/slip_watch.h' file not found`.

- [ ] **Step 3: Implement** — the constants:

```diff
diff --git a/firmware/esp32/src/core/motion_config.h b/firmware/esp32/src/core/motion_config.h
index 8012958..755461d 100644
--- a/firmware/esp32/src/core/motion_config.h
+++ b/firmware/esp32/src/core/motion_config.h
@@ -47,6 +47,11 @@ constexpr int32_t kMoveStallCm = 1;
 constexpr uint32_t kMoveBlockedMs = 2000;      // blocked flag (obstacle brake) for 2 s -> end blocked
 constexpr uint16_t kMoveObstacleMm = 250;      // no progress with an obstacle this close ahead -> blocked (the Uno's
                                                // slow-down stops the tank without its blocked flag, floor 2026-10-17)
+// R21 (design-notes §119): pushing but not moving - the tracks' speed difference should turn the tank; if the gyro
+// does not see it for kSlipHoldMs, a track is slipping and the straight drive ends blocked.
+constexpr uint32_t kSlipWindowMs = 500;     // one check per 0.5 s of counts and gyro
+constexpr int16_t kSlipExpectedDdps = 300;  // the tracks say >= 30 deg/s ...
+constexpr uint32_t kSlipHoldMs = 1500;      // ... the gyro sees < 1/3 of it, for 1.5 s
 constexpr int32_t kMoveTimeoutCmPerS = 5;      // timeout = |cm| / 5 cm/s + 3 s
 constexpr uint32_t kMoveTimeoutBaseMs = 3000;
 // counts_per_m (Plan K, spec 02 §G4): NVS, default from the 160 cm tape run (design-notes §63).
```

`firmware/esp32/src/core/slip_watch.h`:

```cpp
// Pushing but not moving (bridge LLD R21): during a straight drive, tracks that really move at different speeds turn
// the tank - (v_right - v_left) / track width. When the gyro does not see that turn for kSlipHoldMs, a track is
// slipping (e.g. against a table leg the distance sensor cannot see) and the tank is held. Pure, host-tested.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

class SlipWatch {
 public:
  void reset() {
    m_started = false;
    m_slipping = false;
  }
  // Each TELEM_FAST: the raw counts (they rise in both directions), the gyro, counts per metre, the drive's
  // direction (+1 forward, -1 reverse). True once held.
  bool step(int32_t enc_l, int32_t enc_r, int16_t rate_ddps, uint16_t cpm, int8_t dir, uint32_t now_ms);

 private:
  void startWindow(int32_t enc_l, int32_t enc_r, uint32_t now_ms);
  bool m_started = false;
  uint32_t m_win_ms = 0;
  int32_t m_l0 = 0, m_r0 = 0;
  int32_t m_rate_sum = 0;
  int32_t m_rate_n = 0;
  bool m_slipping = false;
  uint32_t m_slip_since = 0;
};

}  // namespace core
}  // namespace cq
```

`firmware/esp32/src/core/slip_watch.cpp`:

```cpp
#include "core/slip_watch.h"

#include "core/motion_config.h"

namespace cq {
namespace core {
namespace {
constexpr float kDegPerRad = 57.29578f;
float absF(float v) { return v < 0.0f ? -v : v; }
}  // namespace

void SlipWatch::startWindow(int32_t enc_l, int32_t enc_r, uint32_t now_ms) {
  m_started = true;
  m_win_ms = now_ms;
  m_l0 = enc_l;
  m_r0 = enc_r;
  m_rate_sum = 0;
  m_rate_n = 0;
}

bool SlipWatch::step(int32_t enc_l, int32_t enc_r, int16_t rate_ddps, uint16_t cpm, int8_t dir, uint32_t now_ms) {
  if (!m_started || cpm == 0) {
    startWindow(enc_l, enc_r, now_ms);
    return false;
  }
  m_rate_sum += rate_ddps;
  ++m_rate_n;
  const uint32_t dt = now_ms - m_win_ms;
  if (dt < motion::kSlipWindowMs) return false;
  // The turn the tracks say (0.1 deg/s, + = left): signed track speeds in cm/s over the track width.
  const float s = static_cast<float>(dir) * 100.0f * 1000.0f / static_cast<float>(cpm) / static_cast<float>(dt);
  const float diff_cm_s = static_cast<float>((enc_r - m_r0) - (enc_l - m_l0)) * s;
  const float expected = diff_cm_s / static_cast<float>(motion::kTrackWidthCm) * kDegPerRad * 10.0f;
  const float measured = static_cast<float>(m_rate_sum) / static_cast<float>(m_rate_n);
  const bool slip = absF(expected) >= motion::kSlipExpectedDdps && absF(measured) * 3.0f < absF(expected);
  if (!slip) {
    m_slipping = false;
  } else if (!m_slipping) {
    m_slipping = true;
    m_slip_since = m_win_ms;  // the window it began in
  }
  startWindow(enc_l, enc_r, now_ms);
  return m_slipping && now_ms - m_slip_since >= motion::kSlipHoldMs;
}

}  // namespace core
}  // namespace cq
```

- [ ] **Step 4: Run it to verify it passes**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_slip_watch`
Expected: PASS, 5 test cases.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/core/slip_watch.h firmware/esp32/src/core/slip_watch.cpp firmware/esp32/src/core/motion_config.h firmware/esp32/test/test_slip_watch/test_main.cpp
git commit -m "feat(bridge): R21 SlipWatch - a track difference the gyro does not see means the tank is held

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: MOVE and GO end blocked when held

**Files:**
- Modify: `firmware/esp32/src/core/move_controller.{h,cpp}`, `firmware/esp32/src/core/motion_service.{h,cpp}`, `firmware/esp32/src/app/motion_controller.cpp`
- Test: `firmware/esp32/test/test_motion/test_main.cpp`

**Interfaces:**
- Consumes (Task 1): `SlipWatch`.
- Produces: `MotionService::startGo(int fd, uint8_t speed_pct, int32_t id, uint32_t now_ms, uint16_t cpm = motion::kCpmDefault)`.

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/firmware/esp32/test/test_motion/test_main.cpp b/firmware/esp32/test/test_motion/test_main.cpp
index 51299d3..87adea8 100644
--- a/firmware/esp32/test/test_motion/test_main.cpp
+++ b/firmware/esp32/test/test_motion/test_main.cpp
@@ -1223,6 +1223,42 @@ void test_link_uno_stall_ends_a_go() {
   TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kStalled), static_cast<int>(o.end.reason));
 }
 
+// Bridge LLD R21: a straight drive with a slipping track (the gyro sees no turn) ends blocked after ~1.5 s - for a
+// MOVE and a GO. Design-notes §119: left 1750 counts/s, right 330, the gyro 0.
+namespace {
+MotionTelem slipAt(int32_t l, int32_t r) {
+  MotionTelem t;
+  t.enc_l = l;
+  t.enc_r = r;
+  return t;  // rate 0, nothing seen ahead
+}
+}  // namespace
+
+void test_a_move_with_a_slipping_track_ends_blocked() {
+  MotionService svc;
+  svc.startMove(3, slipAt(0, 0), 300, 40, 7517, 7, 0);
+  MotionOut o;
+  uint32_t t = 0;
+  for (int32_t l = 0, r = 0; t <= 3000 && !o.ended; l += 35, r += 7) o = svc.onTelemetry(slipAt(l, r), t += 20);
+  TEST_ASSERT_TRUE(o.ended);
+  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kBlocked), static_cast<int>(o.end.reason));
+  TEST_ASSERT_TRUE(t >= 1500 && t <= 2200);
+}
+
+void test_a_go_with_a_slipping_track_ends_blocked() {
+  MotionService svc;
+  svc.startGo(3, 40, 7, 0, 7517);
+  MotionOut o;
+  uint32_t t = 0;
+  for (int32_t l = 0, r = 0; t <= 3000 && !o.ended; l += 35, r += 7) {
+    svc.keepGo(t);
+    o = svc.onTelemetry(slipAt(l, r), t += 20);
+  }
+  TEST_ASSERT_TRUE(o.ended);
+  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kBlocked), static_cast<int>(o.end.reason));
+  TEST_ASSERT_TRUE(t >= 1500 && t <= 2200);
+}
+
 int main() {
   UNITY_BEGIN();
   RUN_TEST(test_router_lite_runs_turn_and_rate_locally);
@@ -1301,5 +1337,7 @@ int main() {
   RUN_TEST(test_a_move_takes_over_from_go_without_an_end);
   RUN_TEST(test_link_hold_sends_stop_and_ends_done);
   RUN_TEST(test_link_uno_stall_ends_a_go);
+  RUN_TEST(test_a_move_with_a_slipping_track_ends_blocked);
+  RUN_TEST(test_a_go_with_a_slipping_track_ends_blocked);
   return UNITY_END();
 }
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_motion`
Expected: FAIL to compile — `too many arguments to function call, expected 4, have 5` (`startGo` with a cpm).

- [ ] **Step 3: Implement**

```diff
diff --git a/firmware/esp32/src/app/motion_controller.cpp b/firmware/esp32/src/app/motion_controller.cpp
index 696763d..f4e1dac 100644
--- a/firmware/esp32/src/app/motion_controller.cpp
+++ b/firmware/esp32/src/app/motion_controller.cpp
@@ -115,7 +115,7 @@ bool MotionController::startLogo(int fd, const logo::Action& a, uint32_t now, co
   core::MotionService& svc = m_motion.service();
   const uint8_t spin = a.speed_pct < core::motion::kTurnWMin ? core::motion::kTurnWMin : a.speed_pct;
   switch (a.kind) {  // VARIANT POINT: Logo motions (Tank Logo LLD §10, §22)
-    case logo::ActKind::kGo: svc.startGo(fd, a.speed_pct, kLogoPrimitive, now); return true;
+    case logo::ActKind::kGo: svc.startGo(fd, a.speed_pct, kLogoPrimitive, now, m_cpm); return true;
     case logo::ActKind::kMove:
       svc.startMove(fd, telemOf(snap), a.dist_cm, a.speed_pct, m_cpm, kLogoPrimitive, now, m_roll_mm);
       return true;
diff --git a/firmware/esp32/src/core/motion_service.cpp b/firmware/esp32/src/core/motion_service.cpp
index 46d07d7..a2afec8 100644
--- a/firmware/esp32/src/core/motion_service.cpp
+++ b/firmware/esp32/src/core/motion_service.cpp
@@ -121,14 +121,16 @@ MotionOut MotionService::finish(EndReason why) {
 
 MotionOut MotionService::cancel(EndReason why) { return active() ? finish(why) : MotionOut{}; }
 
-void MotionService::startGo(int fd, uint8_t speed_pct, int32_t id, uint32_t now_ms) {
+void MotionService::startGo(int fd, uint8_t speed_pct, int32_t id, uint32_t now_ms, uint16_t cpm) {
   if (m_kind != PrimKind::kGo || m_fd != fd) {
     m_kind = PrimKind::kGo;
     m_fd = fd;
     m_id = id;
     m_last_telem_ms = now_ms;
     m_go_ref_set = false;
+    m_go_slip.reset();
   }
+  m_go_cpm = cpm;
   m_go_v = static_cast<int8_t>(speed_pct > 100 ? 100 : speed_pct);
   m_go_update_ms = now_ms;
 }
@@ -162,6 +164,7 @@ MotionOut MotionService::stepGo(const MotionTelem& t, uint32_t now_ms) {
     return finish(EndReason::kBlocked);
   }
   if (goStopped(t, now_ms) && t.dist_mm < motion::kMoveObstacleMm) return finish(EndReason::kBlocked);
+  if (m_go_slip.step(t.enc_l, t.enc_r, t.rate_ddps, m_go_cpm, 1, now_ms)) return finish(EndReason::kBlocked);  // R21
   MotionOut o;
   o.send = true;
   o.v = m_go_v;  // w = 0: the Uno's heading hold keeps it straight
diff --git a/firmware/esp32/src/core/motion_service.h b/firmware/esp32/src/core/motion_service.h
index 1bc49af..3614d93 100644
--- a/firmware/esp32/src/core/motion_service.h
+++ b/firmware/esp32/src/core/motion_service.h
@@ -9,6 +9,7 @@
 #include "core/motion_config.h"
 #include "core/move_controller.h"
 #include "core/rate_loop.h"
+#include "core/slip_watch.h"
 #include "core/turn_controller.h"
 
 namespace cq {
@@ -56,7 +57,7 @@ class MotionService {
   MotionOut cancel(EndReason why);
   // R20: GO - drive straight on at speed_pct (w = 0, the Uno's heading hold) until held, replaced or blocked; the
   // owner refreshes it (keepGo) at least every kRateDeadmanMs. startGo while going: a new speed, refreshed.
-  void startGo(int fd, uint8_t speed_pct, int32_t id, uint32_t now_ms);
+  void startGo(int fd, uint8_t speed_pct, int32_t id, uint32_t now_ms, uint16_t cpm = motion::kCpmDefault);
   void keepGo(uint32_t now_ms) {
     if (m_kind == PrimKind::kGo) m_go_update_ms = now_ms;
   }
@@ -109,6 +110,8 @@ class MotionService {
   int32_t m_go_ref_counts = 0;
   uint16_t m_go_ref_dist = 0;
   uint32_t m_go_ref_ms = 0;
+  uint16_t m_go_cpm = motion::kCpmDefault;  // R21
+  SlipWatch m_go_slip;                      // R21: pushing but not moving
   bool goStopped(const MotionTelem& t, uint32_t now_ms);  // no progress for kMoveStallMs
   int16_t m_rate_target = 0;
   bool m_rate_limited = false;
diff --git a/firmware/esp32/src/core/move_controller.cpp b/firmware/esp32/src/core/move_controller.cpp
index 1ff8e16..aea0181 100644
--- a/firmware/esp32/src/core/move_controller.cpp
+++ b/firmware/esp32/src/core/move_controller.cpp
@@ -26,6 +26,7 @@ void MoveController::begin(int32_t enc_l, int32_t enc_r, int16_t dist_cm, uint8_
   m_ref_ms = now_ms;
   m_deadline = now_ms + static_cast<uint32_t>(cm) * 1000u / motion::kMoveTimeoutCmPerS + motion::kMoveTimeoutBaseMs;
   m_blocked = false;
+  m_slip.reset();
 }
 
 // The obstacle brake has held the move for kMoveBlockedMs.
@@ -71,8 +72,8 @@ MoveStep MoveController::step(const MotionTelem& t, uint32_t now_ms) {
     s.done = true;
     return s;
   }
-  if (blockedTooLong(t, now_ms)) {
-    s.blocked = true;
+  if (blockedTooLong(t, now_ms) || m_slip.step(t.enc_l, t.enc_r, t.rate_ddps, m_cpm, m_dir, now_ms)) {
+    s.blocked = true;  // the obstacle brake, or a slipping track the gyro does not see (R21)
     return s;
   }
   if (stuck(t, now_ms, s)) return s;
diff --git a/firmware/esp32/src/core/move_controller.h b/firmware/esp32/src/core/move_controller.h
index 14eb1c4..b6f72e7 100644
--- a/firmware/esp32/src/core/move_controller.h
+++ b/firmware/esp32/src/core/move_controller.h
@@ -5,6 +5,8 @@
 
 #include <stdint.h>
 
+#include "core/slip_watch.h"
+
 namespace cq {
 namespace core {
 
@@ -50,6 +52,7 @@ class MoveController {
   int8_t m_dir = 1;
   uint8_t m_speed = 40;
   uint16_t m_cpm = 7560;
+  SlipWatch m_slip;  // R21: pushing but not moving
   uint32_t m_ref_ms = 0;
   uint32_t m_deadline = 0;
   bool m_blocked = false;
```

- [ ] **Step 4: Run to verify they pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native`
Expected: PASS, 652 test cases (all the existing move tests too: no false slip).
Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio run -e bridge`
Expected: SUCCESS (flash ~44.4 %).
Run: `uv run --with lizard==1.24.1 python scripts/size_check.py`
Expected: no output, exit 0.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/core/move_controller.h firmware/esp32/src/core/move_controller.cpp firmware/esp32/src/core/motion_service.h firmware/esp32/src/core/motion_service.cpp firmware/esp32/src/app/motion_controller.cpp firmware/esp32/test/test_motion/test_main.cpp
git commit -m "feat(bridge): R21 a MOVE or a GO with a slipping track ends blocked

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: On the tank — flash and floor check (owner present)

**Safety:** a **floor** check, so the tank drives. Ask "stand or floor?" before each drive and say what moves. Keep STOP within reach.

- [ ] **Step 1: Flash the bridge over the air**

```bash
cd firmware/esp32
export CQ_OTA_PASSWORD="$(sed -n 's/.*CQ_OTA_PASSWORD[^"]*"\([^"]*\)".*/\1/p' include/secrets.h)"
~/.platformio/penv/bin/pio run -e bridge_ota -t upload --upload-port 192.168.31.167 2>&1 | grep -vF -- "$CQ_OTA_PASSWORD" | tail -3
unset CQ_OTA_PASSWORD
```
Expected: `SUCCESS`.

- [ ] **Step 2: A track against a table leg, out of the sensor's view**

The owner places the tank so that one track will meet a table leg beside its path. Run `FOREVER [ GO  WAIT UNTIL BLOCKED?  BACK 10  RIGHT 90 ]` with `td_record.py` at 50 Hz; stop it with STOP after the event.
Expected: within ~2 s of the track meeting the leg, the GO ends blocked and the tank backs off and turns (in §119 it pushed for 4.7 s).

- [ ] **Step 3: No false stops**

`move.py 200` on open floor.
Expected: it ends as before (±1 cm, design-notes §116); no early "blocked".

---

### Task 4: Docs as built

**Files:** `docs/lld/esp32-bridge-lld.md` (R21.4 as built + floor results), `docs/design-notes.md` (the floor results with how they were read), `CLAUDE.md` (plan list, Next).

- [ ] **Step 1: Write them.**
- [ ] **Step 2: Commit**

```bash
git add docs CLAUDE.md
git commit -m "docs: Plan R3 pushing but not moving - as built and floor results

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```


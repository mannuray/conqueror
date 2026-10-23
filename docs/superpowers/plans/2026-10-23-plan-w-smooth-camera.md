# Plan W — Smooth camera motion (Uno Lite LLD §23) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The camera moves smoothly, like an RC servo: no stop-and-go on slow stick moves, and ramped starts on fast ones, for every source (stick, IR, HOME, the return, `CAM_SET`).

**Architecture:** `CamLite::step()` replaces "at most 24 dd toward the target" with `follow()` per axis:
- a quarter of the gap per tick (rounded up), capped at 24 dd;
- at most 3 dd per tick faster than the speed already going that way (a turn back starts from 0);
- lands within 2 dd.

There's a new speed per axis (`m_v_pan`, `m_v_tilt`). Nothing else changes: the targets, telemetry, trims, the 2 s release, the protocol.

**Tech Stack:** C++ (AVR `uno` env, `-Os`), PlatformIO `native` Unity host tests.

**Spec:** `docs/lld/uno-lite-lld.md` §23 (approved 2026-10-23); design-notes §136.

**Built from a tested draft:** branch `w-draft`. Uno native 301/301; flash **31 488 B** (+112 B over 31 376; budget 120 B); RAM 1544 B.

## Global Constraints
- **Uno:**
  - no heap, no `virtual`, no `delay()`; domain code never includes `ports/` or `Arduino.h`;
  - integer maths only (the Lite does not link float maths);
  - every function's complexity ≤ 10 (pre-commit `size_check`).
- **Flash:** ≤ 31 496 B (31 376 + 120).
- **Constants:** `kStepDd` 24, `kFollowDiv` 4, `kAccelDd` 3, `kSnapDd` 2.
- **Unchanged:** `pan()` / `tilt()`, `outPan()` / `outTilt()` (telemetry), `servoPan()` / `servoTilt()` (trims), the 2 s release. No protocol, bridge or generated-file change.
- **Hardware:** a stand check; only the camera moves. The Uno is flashed with avrdude on the Linux machine (`/dev/ttyUSB0`, S1 in the upload position, then back).

## Review Focus
1. **The IR jog** (12 dd added to the target per tick): the output must keep up at 60°/s without stalls. Covered by `test_cam_held_jog_rate` (the target) and the staircase test (the same shape); felt on the stand.
2. **A target move while released** (after 2 s): the speed starts from 0, so there's no jump. Covered by `test_cam_idle_detach_after_2s` and `test_cam_small_jog_wakes_servos`.
3. **The joystick's return home after release:** a big move with ease-in, landing exactly. Covered by `test_cam_big_move_eases_in_and_lands`.
4. **int16 range:** gaps up to 1800 dd and speeds ≤ 24, so no overflow. `(mag + 3) / 4` with mag ≤ 1800 is safe.
5. **The trims still reach the servo's 0 / 180 stops** with the slower approach. Covered by `test_cam_trim_moves_only_the_servo` (60 and 100 ticks still enough).

---

### Task 1: The smooth step

**Files:**
- Modify: `firmware/uno/src/domain/lite/cam_lite.h`, `firmware/uno/src/domain/lite/cam_lite.cpp`
- Test: `firmware/uno/test/test_lite_ui/test_main.cpp` (`test_cam_smooth_steps` replaced by four tests)

**Interfaces:**
- Produces: `CamLite::kFollowDiv`, `kAccelDd`, `kSnapDd`; `step()` with the new motion. Nothing else changes.

- [ ] **Step 1: Write the failing tests** (apply this diff):

```diff
diff --git a/firmware/uno/test/test_lite_ui/test_main.cpp b/firmware/uno/test/test_lite_ui/test_main.cpp
index a5fc985..a940de2 100644
--- a/firmware/uno/test/test_lite_ui/test_main.cpp
+++ b/firmware/uno/test/test_lite_ui/test_main.cpp
@@ -33,19 +33,67 @@ void test_cam_joy_release_returns_home_unless_locked() {
   TEST_ASSERT_EQUAL_INT16(0, c.pan());  // locked: stays
 }
 
-// LLD §8 extra 1 (space allowed it): the servo output follows the target at most 24 deci-degrees per 20 ms tick
-// (120 °/s, the full firmware's servo_speed default); pan()/tilt() stay the targets.
-void test_cam_smooth_steps() {
+// Uno Lite LLD §23: the output follows the target smoothly - a quarter of the gap per tick, at most 24 dd per
+// 20 ms tick (120 °/s), speeding up by at most 3 dd per tick, never past the target, landing exactly.
+void test_cam_big_move_eases_in_and_lands() {
   CamLite c;
   c.set(1800, 900);
-  c.step();
-  TEST_ASSERT_EQUAL_INT16(924, c.outPan());
-  TEST_ASSERT_EQUAL_INT16(900, c.outTilt());
-  for (int i = 0; i < 100; ++i) c.step();
+  int16_t last = c.outPan(), step_prev = 0;
+  for (int i = 0; i < 120; ++i) {
+    c.step();
+    const int16_t d = static_cast<int16_t>(c.outPan() - last);
+    TEST_ASSERT_TRUE(d >= 0 && d <= CamLite::kStepDd);  // one way, capped
+    if (d > step_prev) TEST_ASSERT_TRUE(d - step_prev <= CamLite::kAccelDd);  // eases in
+    TEST_ASSERT_TRUE(c.outPan() <= 1800);  // never past the target
+    if (i < 3) TEST_ASSERT_EQUAL_INT16(3 * (i + 1), d);  // 3, 6, 9 ...
+    step_prev = d;
+    last = c.outPan();
+  }
   TEST_ASSERT_EQUAL_INT16(1800, c.outPan());
-  c.set(1790, 900);
+  TEST_ASSERT_EQUAL_INT16(900, c.outTilt());
+}
+
+// The phone's stick: +9 dd (1 %) every 3 ticks. The output must move every tick - no stop-and-go.
+void test_cam_staircase_target_moves_every_tick() {
+  CamLite c;
+  int16_t target = 900, last = 900;
+  for (int i = 0; i < 90; ++i) {
+    if (i % 3 == 0) target = static_cast<int16_t>(target + 9);
+    c.set(target, 900);
+    c.step();
+    if (i >= 2) TEST_ASSERT_TRUE(c.outPan() > last);
+    last = c.outPan();
+  }
+}
+
+// A new target behind the camera mid-move: it turns back without going past the new target.
+void test_cam_reversal_does_not_overshoot() {
+  CamLite c;
+  c.set(1800, 900);
+  for (int i = 0; i < 20; ++i) c.step();  // at full speed
+  const int16_t back = static_cast<int16_t>(c.outPan() - 100);
+  c.set(back, 900);
+  int16_t peak = c.outPan();
+  for (int i = 0; i < 80; ++i) {
+    c.step();
+    TEST_ASSERT_TRUE(c.outPan() >= back);
+    if (c.outPan() > peak) peak = c.outPan();
+  }
+  TEST_ASSERT_EQUAL_INT16(back, c.outPan());
+  TEST_ASSERT_TRUE(peak - back <= 100 + CamLite::kStepDd);  // at most one more tick forward before turning
+}
+
+void test_cam_small_moves_land_exactly() {
+  CamLite c;
+  c.set(901, 900);
   c.step();
-  TEST_ASSERT_EQUAL_INT16(1790, c.outPan());  // a small move lands exactly, no overshoot
+  TEST_ASSERT_EQUAL_INT16(901, c.outPan());
+  c.set(891, 900);
+  for (int i = 0; i < 20; ++i) {
+    c.step();
+    TEST_ASSERT_TRUE(c.outPan() >= 891);
+  }
+  TEST_ASSERT_EQUAL_INT16(891, c.outPan());
 }
 
 // Final review M1: a large CAM_NUDGE must stop at the limit, not wrap to the opposite one (16-bit int on AVR).
@@ -178,7 +226,10 @@ int main() {
   UNITY_BEGIN();
   RUN_TEST(test_cam_limits_and_us);
   RUN_TEST(test_cam_joy_release_returns_home_unless_locked);
-  RUN_TEST(test_cam_smooth_steps);
+  RUN_TEST(test_cam_big_move_eases_in_and_lands);
+  RUN_TEST(test_cam_staircase_target_moves_every_tick);
+  RUN_TEST(test_cam_reversal_does_not_overshoot);
+  RUN_TEST(test_cam_small_moves_land_exactly);
   RUN_TEST(test_cam_nudge_large_saturates);
   RUN_TEST(test_cam_held_jog_rate);
   RUN_TEST(test_cam_joy_up_is_smaller_tilt);
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd firmware/uno && ~/.platformio/penv/bin/pio test -e native -f test_lite_ui`
Expected: `error: no member named 'kAccelDd' in 'cq::CamLite'`.

- [ ] **Step 3: Write the code** (apply this diff):

```diff
diff --git a/firmware/uno/src/domain/lite/cam_lite.cpp b/firmware/uno/src/domain/lite/cam_lite.cpp
index f906c97..7c5c50d 100644
--- a/firmware/uno/src/domain/lite/cam_lite.cpp
+++ b/firmware/uno/src/domain/lite/cam_lite.cpp
@@ -4,8 +4,24 @@ namespace cq {
 
 namespace {
 int16_t clampI(int16_t v, int16_t lo, int16_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
-int16_t toward(int16_t out, int16_t target, int16_t step) {
-  return clampI(target, static_cast<int16_t>(out - step), static_cast<int16_t>(out + step));
+// One axis, one tick (Uno Lite LLD §23), worked on the size of the gap: a quarter of it (rounded up, so it never
+// stalls), at most kStepDd, at most kAccelDd faster than the speed already going that way (a turn back starts
+// from 0); never past the target, because the step is at most a quarter of the gap; within kSnapDd it lands.
+void follow(int16_t& out, int16_t& v, int16_t target) {
+  const int16_t gap = static_cast<int16_t>(target - out);
+  const bool back = gap < 0;
+  const int16_t mag = back ? static_cast<int16_t>(-gap) : gap;
+  if (mag <= CamLite::kSnapDd) {
+    out = target;
+    v = 0;
+    return;
+  }
+  int16_t want = static_cast<int16_t>((mag + CamLite::kFollowDiv - 1) / CamLite::kFollowDiv);
+  if (want > CamLite::kStepDd) want = CamLite::kStepDd;
+  const int16_t speed = back == (v < 0) ? (v < 0 ? static_cast<int16_t>(-v) : v) : 0;
+  if (want > speed + CamLite::kAccelDd) want = static_cast<int16_t>(speed + CamLite::kAccelDd);
+  v = back ? static_cast<int16_t>(-want) : want;
+  out = static_cast<int16_t>(out + v);
 }
 }  // namespace
 
@@ -38,8 +54,8 @@ void CamLite::joy(int8_t x, int8_t y, bool active, uint32_t now_ms) {
 }
 
 void CamLite::step() {
-  m_out_pan = toward(m_out_pan, m_pan, kStepDd);
-  m_out_tilt = toward(m_out_tilt, m_tilt, kStepDd);
+  follow(m_out_pan, m_v_pan, m_pan);
+  follow(m_out_tilt, m_v_tilt, m_tilt);
 }
 
 void CamLite::tick(uint32_t now_ms) {
diff --git a/firmware/uno/src/domain/lite/cam_lite.h b/firmware/uno/src/domain/lite/cam_lite.h
index 113fec4..784d5eb 100644
--- a/firmware/uno/src/domain/lite/cam_lite.h
+++ b/firmware/uno/src/domain/lite/cam_lite.h
@@ -3,8 +3,9 @@
 
 namespace cq {
 
-// Camera pan/tilt in deci-degrees. pan()/tilt() are the targets; step() moves the servo output toward them at
-// most kStepDd per 20 ms tick (120 °/s, smooth motion = LLD §8 extra 1).
+// Camera pan/tilt in deci-degrees. pan()/tilt() are the targets; step() moves the servo output toward them each
+// 20 ms tick: a quarter of the gap, at most kStepDd (120 °/s), speeding up by at most kAccelDd per tick, landing
+// exactly (Uno Lite LLD §23: smooth like an RC servo, whatever feeds the target).
 // Pan 0..1800 (x > 0 = right = lower angle), tilt 300..1100 (y > 0 = up = lower angle: this tank looks down at
 // larger angles, design-notes §45), home 900/900. Joystick release → home after 500 ms
 // unless locked.
@@ -14,6 +15,9 @@ class CamLite {
   static constexpr int16_t kPanMin = 0, kPanMax = 1800, kTiltMin = 300, kTiltMax = 1100;
   static constexpr uint16_t kReturnMs = 500;
   static constexpr int16_t kStepDd = 24;
+  static constexpr int16_t kFollowDiv = 4;  // each tick covers this fraction of the gap (1/4)
+  static constexpr int16_t kAccelDd = 3;    // the speed grows by at most this per tick (0 -> 120 °/s in 0.16 s)
+  static constexpr int16_t kSnapDd = 2;     // this close: land on the target
   static constexpr uint16_t kJoyTimeoutMs = 500;   // stick held without refresh = phone lost (full CameraHead)
   static constexpr uint16_t kDetachAfterMs = 2000;  // servos released after 2 s still (spec 02 §D5.4)
   static constexpr int16_t kJogDd = 12;  // per 20 ms tick = 60 °/s (full firmware: 1.2° per tick)
@@ -57,6 +61,8 @@ class CamLite {
   int16_t m_tilt = kHome;
   int16_t m_out_pan = kHome;
   int16_t m_out_tilt = kHome;
+  int16_t m_v_pan = 0;  // dd per tick
+  int16_t m_v_tilt = 0;
   int16_t m_pan_trim = 0;
   int16_t m_tilt_trim = 0;
   int8_t m_jog_pan = 0;
```

- [ ] **Step 4: Run the tests, the whole suite and the build**

Run: `cd firmware/uno && ~/.platformio/penv/bin/pio test -e native -f test_lite_ui && ~/.platformio/penv/bin/pio test -e native 2>&1 | tail -1 && ~/.platformio/penv/bin/pio run -e uno | grep -E "Flash:|RAM:"`
Expected: 16 camera tests pass; `301 test cases: 301 succeeded`; Flash 31 488 B (≤ 31 496), RAM 1544 B.

- [ ] **Step 5: Commit**

```bash
git add firmware/uno
git commit -m "feat(uno): smooth camera motion - follow, speed cap, gentle start (Uno Lite LLD §23)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 2: On the stand — flash and feel (owner present)

Only the camera moves; the tracks don't.

- [ ] **Step 1: Flash the Uno.**
  - The owner sets S1 to upload.
  - Copy `firmware/uno/.pio/build/uno/firmware.hex` to the Linux machine (`scp` to `/tmp/`), then run there: `~/.platformio/packages/tool-avrdude/avrdude -C ~/.platformio/packages/tool-avrdude/avrdude.conf -p atmega328p -c arduino -P /dev/ttyUSB0 -b 115200 -D -U flash:w:/tmp/<hex>:i`.
  - Expected: `31488 bytes of flash verified`. S1 back afterwards, and the hex deleted from `/tmp`.
- [ ] **Step 2: Record.**
  - Run the session script `servo_record.py 30 run2.csv` (as in design-notes §136) while the owner moves the phone's camera stick slowly for ~10 s, then quickly, then lets go.
  - Expected: in slow moves, no "M..M" pattern (the output moves every tick while the stick moves); fast moves ramp up over ~8 ticks; landings with no overshoot.
- [ ] **Step 3: Feel.** The owner compares it with the RC transmitter. If it's too soft (lags) or too quick, tune `kFollowDiv` (3 = quicker, 5 = softer) or `kAccelDd`, re-test (Task 1 Step 4), and flash again.
- [ ] **Step 4:** Write the numbers in the ledger. Delete the recording CSVs.

### Task 3: Docs as built

**Files:**
- `docs/lld/uno-lite-lld.md` (§23.1 as built: flash, the constants as tuned, the recording);
- `docs/design-notes.md` (the before / after numbers, the owner's feel);
- `CLAUDE.md` (plan list: `2026-10-23-plan-w-smooth-camera.md`; the Uno flash figure).

- [ ] **Step 1: Write them.**
- [ ] **Step 2: Commit**

```bash
git add docs CLAUDE.md
git commit -m "docs: Plan W smooth camera motion - as built and stand results

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

#include <math.h>
#include <stdio.h>
#include <unity.h>

#include "domain/lite/gyro_zero_lite.h"
#include "domain/lite/yaw_lite.h"
#include "domain/lite/heading_hold_lite.h"
#include "domain/lite/drive_mix_lite.h"

using namespace cq;

void setUp() {}
void tearDown() {}

namespace {
// n samples of z with ±3 alternating noise every 5 ms; returns the next sample time.
uint32_t feedZ(GyroZeroLite& g, int16_t z, int n, uint32_t t, bool quiet = true) {
  for (int i = 0; i < n; ++i, t += 5) g.update(static_cast<int16_t>(z + ((i & 1) ? 3 : -3)), quiet, t);
  return t;
}
}  // namespace

void test_gzl_starts_from_stored() {
  GyroZeroLite g;
  g.seed(-17);
  TEST_ASSERT_EQUAL_INT16(-17, g.zero());
  TEST_ASSERT_EQUAL_UINT16(0, g.accepted());
}

void test_gzl_learns_after_two_agreeing_windows() {
  GyroZeroLite g;
  g.seed(0);
  uint32_t t = feedZ(g, -13, 200 + 400, 0);
  TEST_ASSERT_EQUAL_UINT16(0, g.accepted());
  feedZ(g, -13, 400, t);
  TEST_ASSERT_EQUAL_UINT16(1, g.accepted());
  TEST_ASSERT_EQUAL_INT16(-13, g.zero());
}

void test_gzl_ignores_a_bump() {
  GyroZeroLite g;
  g.seed(0);
  uint32_t t = feedZ(g, -13, 200 + 200, 0);
  t = feedZ(g, 200, 1, t);
  t = feedZ(g, -13, 199, t);
  feedZ(g, -13, 400, t);
  TEST_ASSERT_EQUAL_UINT16(0, g.accepted());
}

void test_gzl_motion_restarts_settle() {
  GyroZeroLite g;
  g.seed(0);
  uint32_t t = feedZ(g, -13, 200 + 200, 0);
  t = feedZ(g, -13, 1, t, false);
  feedZ(g, -13, 800, t);
  TEST_ASSERT_EQUAL_UINT16(0, g.accepted());
}

void test_gzl_disagreeing_windows_not_learned() {
  GyroZeroLite g;
  g.seed(0);
  uint32_t t = feedZ(g, 100, 200 + 400, 0);
  t = feedZ(g, 140, 400, t);
  feedZ(g, 180, 400, t);
  TEST_ASSERT_EQUAL_UINT16(0, g.accepted());
}

void test_gzl_rejects_out_of_range() {
  GyroZeroLite g;
  g.seed(0);
  feedZ(g, 1400, 200 + 800, 0);
  TEST_ASSERT_EQUAL_UINT16(0, g.accepted());
}

void test_gzl_reseed_only_on_change() {
  GyroZeroLite g;
  g.seed(0);
  feedZ(g, -13, 200 + 800, 0);
  g.seed(0);
  TEST_ASSERT_EQUAL_INT16(-13, g.zero());
  g.seed(-15);
  TEST_ASSERT_EQUAL_INT16(-15, g.zero());
}

// 65.5 raw = 1 °/s; 200 samples = 1 s → 655 raw for 200 samples = 10° = 1000 cdeg.
void test_yaw_integrates_ccw() {
  YawLite y;
  for (int i = 0; i < 200; ++i) y.update(655, false);
  TEST_ASSERT_INT32_WITHIN(2, 1000, y.yawCdeg());
  TEST_ASSERT_EQUAL_INT16(100, y.rateDdps());  // 10.0 °/s
}

void test_yaw_wraps_below_zero() {
  YawLite y;
  for (int i = 0; i < 200; ++i) y.update(-655, false);  // 10° clockwise from 0
  TEST_ASSERT_INT32_WITHIN(2, 35000, y.yawCdeg());
}

void test_yaw_still_clamp_only_when_stationary() {
  YawLite y;
  for (int i = 0; i < 200; ++i) y.update(20, true);  // 0.3 °/s noise at rest: ignored
  TEST_ASSERT_EQUAL_INT32(0, y.yawCdeg());
  for (int i = 0; i < 2000; ++i) y.update(20, false);  // same rate while moving: counted (≈ 3.05°)
  TEST_ASSERT_INT32_WITHIN(5, 305, y.yawCdeg());
}

namespace {
HeadingHoldLite hh() {
  HeadingHoldLite h;
  h.configure(headingGainsFromMilli(3500, 1200));
  return h;
}
}  // namespace

void test_heading_gains_from_settings() {
  const HeadingGains g = headingGainsFromMilli(3500, 1200);
  TEST_ASSERT_EQUAL_INT16(1575, g.kp_q8);
  TEST_ASSERT_EQUAL_INT16(307, g.kd_q8);
}

void test_heading_steering_is_open_loop() {
  HeadingHoldLite h = hh();
  int8_t s = 0, c = 0;
  h.teleop(0, 40, true, 1000, 2000, s, c);
  TEST_ASSERT_EQUAL_INT8(40, s);
  TEST_ASSERT_EQUAL_INT8(0, c);
  TEST_ASSERT_FALSE(h.holding());
}

void test_heading_captures_then_corrects_drift() {
  HeadingHoldLite h = hh();
  int8_t s = 0, c = 0;
  h.teleop(80, 0, true, 1000, 0, s, c);  // straight, not rotating → capture 10.00°
  TEST_ASSERT_TRUE(h.holding());
  h.teleop(80, 0, true, 900, 0, s, c);   // drifted 1° clockwise → turn CCW (positive), ≈ 6 %
  TEST_ASSERT_EQUAL_INT8(0, s);
  TEST_ASSERT_INT8_WITHIN(1, 6, c);
}

void test_heading_rate_damping() {
  HeadingHoldLite h = hh();
  int8_t s = 0, c = 0;
  h.teleop(80, 0, true, 1000, 0, s, c);
  h.teleop(80, 0, true, 1000, 100, s, c);  // on heading but turning CCW at 10 °/s → push CW (≈ −12 %)
  TEST_ASSERT_INT8_WITHIN(1, -12, c);
}

void test_heading_waits_for_turn_to_settle_max_300ms() {
  HeadingHoldLite h = hh();
  int8_t s = 0, c = 0;
  h.teleop(80, 50, true, 0, 900, s, c);  // steering
  for (int i = 0; i < 29; ++i) h.teleop(80, 0, true, 0, 200, s, c);  // still turning 20 °/s
  TEST_ASSERT_FALSE(h.holding());
  TEST_ASSERT_EQUAL_INT8(0, c);  // no braking while settling
  h.teleop(80, 0, true, 0, 200, s, c);
  h.teleop(80, 0, true, 0, 200, s, c);
  TEST_ASSERT_TRUE(h.holding());  // 0.3 s passed: captured despite the pull
}

void test_heading_wraps_short_way() {
  HeadingHoldLite h = hh();
  int8_t s = 0, c = 0;
  h.teleop(80, 0, true, 50, 0, s, c);      // hold 0.50°
  h.teleop(80, 0, true, 35950, 0, s, c);   // now 359.50° = 1° clockwise of the hold → small CCW push
  TEST_ASSERT_INT8_WITHIN(1, 6, c);
}

void test_heading_gyro_lost_is_open_loop() {
  HeadingHoldLite h = hh();
  int8_t s = 0, c = 0;
  h.teleop(80, 0, true, 1000, 0, s, c);
  h.teleop(80, 0, false, 500, 0, s, c);  // gyro gone: no stale correction
  TEST_ASSERT_EQUAL_INT8(0, c);
  TEST_ASSERT_EQUAL_INT8(0, s);
  TEST_ASSERT_FALSE(h.holding());
  h.teleop(80, 30, false, 500, 0, s, c);
  TEST_ASSERT_EQUAL_INT8(30, s);
}

void test_heading_stop_resets() {
  HeadingHoldLite h = hh();
  int8_t s = 0, c = 0;
  h.teleop(80, 0, true, 1000, 0, s, c);
  h.teleop(0, 0, true, 1000, 0, s, c);
  TEST_ASSERT_FALSE(h.holding());
}

// Plan H review C1 (option A): car-style reverse steering flips only a driver's stick, never the bridge
// controller's output (src AUTO) - a flipped controller output becomes positive feedback (design-notes §39).
void test_steer_lite_car_flip_except_auto() {
  TEST_ASSERT_EQUAL_INT8(-30, steerLite(-40, 30, true, false));   // car-style, reversing driver: flipped
  TEST_ASSERT_EQUAL_INT8(30, steerLite(40, 30, true, false));     // forward: as given
  TEST_ASSERT_EQUAL_INT8(30, steerLite(-40, 30, false, false));   // tank-style: never flipped
  TEST_ASSERT_EQUAL_INT8(30, steerLite(-40, 30, true, true));     // bridge controller (AUTO): never flipped
}

void test_mix_steering_scaled_gyro_not() {
  Tracks t = mixLite(80, 0, 10, 40);  // gyro 10 % at 80 %: full 10
  TEST_ASSERT_EQUAL_INT16(70, t.l);
  TEST_ASSERT_EQUAL_INT16(90, t.r);
  t = mixLite(80, 10, 0, 40);  // driver 10 % at 80 %: angle mix (spec 02 §A4.1), turn_full no longer scales it
  TEST_ASSERT_EQUAL_INT16(68, t.l);
  TEST_ASSERT_EQUAL_INT16(80, t.r);  // outer = stick distance |(80, 10)| = 80
}

void test_mix_turning_priority() {  // §A4.1: a steering push keeps both tracks forward, outer at the stick magnitude
  const Tracks t = mixLite(100, 50, 0, 40);
  TEST_ASSERT_EQUAL_INT16(57, t.l);
  TEST_ASSERT_EQUAL_INT16(100, t.r);
}

// Spec 02 §A4.1 (owner 2026-10-15): stick steering by angle.
void test_angle_mix_45_is_forward_arc() {
  Tracks t = mixLite(71, 71, 0, 100);  // up-left 45°: forward arc, inner (left) slower, never backwards
  TEST_ASSERT_EQUAL_INT16(36, t.l);
  TEST_ASSERT_EQUAL_INT16(100, t.r);
  t = mixLite(71, -71, 0, 100);        // up-right
  TEST_ASSERT_EQUAL_INT16(100, t.l);
  TEST_ASSERT_EQUAL_INT16(36, t.r);
  t = mixLite(35, 35, 0, 100);         // half push: same shape, slower
  TEST_ASSERT_EQUAL_INT16(17, t.l);
  TEST_ASSERT_EQUAL_INT16(49, t.r);
}

void test_angle_mix_spin_only_near_sideways() {
  Tracks t = mixLite(26, 97, 0, 100);  // 75°: inner track stopped (pivot)
  TEST_ASSERT_EQUAL_INT16(0, t.l);
  TEST_ASSERT_EQUAL_INT16(100, t.r);
  t = mixLite(17, 98, 0, 100);         // 80°: spin starting
  TEST_ASSERT_EQUAL_INT16(-29, t.l);
  TEST_ASSERT_EQUAL_INT16(99, t.r);
  t = mixLite(0, 100, 0, 100);         // 90°: spin on the spot
  TEST_ASSERT_EQUAL_INT16(-100, t.l);
  TEST_ASSERT_EQUAL_INT16(100, t.r);
}

// Deferred minor (bridge LLD R3.11): the outer track follows the stick distance, never above MAX (old estimate
// "big + 3/8 small" was ~7 % over near 20°). Full stick at MAX 60 %, every angle 1..90°.
void test_angle_mix_never_over_max() {
  for (int deg = 1; deg <= 90; ++deg) {
    const double th = deg * 3.14159265358979 / 180.0;
    const int8_t v = static_cast<int8_t>(lround(60.0 * cos(th)));
    const int8_t w = static_cast<int8_t>(lround(60.0 * sin(th)));
    if (w == 0) continue;
    const Tracks t = mixLite(v, w, 0, 100);
    const int16_t outer = t.l > t.r ? t.l : t.r;
    char msg[16];
    snprintf(msg, sizeof(msg), "deg %d", deg);
    TEST_ASSERT_TRUE_MESSAGE(outer <= 61 && outer >= 59, msg);
  }
}

void test_angle_mix_reverse_both_tracks_back() {
  const Tracks t = mixLite(-71, 71, 0, 100);  // reversing 45°: both tracks backwards (old mix: one forward)
  TEST_ASSERT_EQUAL_INT16(-100, t.l);
  TEST_ASSERT_EQUAL_INT16(-36, t.r);
}

void test_ir_axes_unchanged() {  // IR sends pure axes: identical tracks to the old mix
  Tracks t = mixLite(80, 0, 0, 40);
  TEST_ASSERT_EQUAL_INT16(80, t.l);
  TEST_ASSERT_EQUAL_INT16(80, t.r);
  t = mixLite(-80, 0, 0, 40);
  TEST_ASSERT_EQUAL_INT16(-80, t.l);
  TEST_ASSERT_EQUAL_INT16(-80, t.r);
  t = mixLite(0, 80, 0, 40);
  TEST_ASSERT_EQUAL_INT16(-80, t.l);
  TEST_ASSERT_EQUAL_INT16(80, t.r);
  t = mixLite(0, -80, 0, 40);
  TEST_ASSERT_EQUAL_INT16(80, t.l);
  TEST_ASSERT_EQUAL_INT16(-80, t.r);
}

void test_straight_with_correction_unchanged() {  // w_steer 0: the old arcade mix incl. turning priority
  const Tracks t = mixLite(100, 0, 30, 40);
  TEST_ASSERT_EQUAL_INT16(40, t.l);
  TEST_ASSERT_EQUAL_INT16(100, t.r);
}

void test_cap_keeps_ratio() {
  bool capped = false;
  const Tracks t = capLite(Tracks{40, 80}, 40, 0xFFFF, 15, 50, true, capped);
  TEST_ASSERT_TRUE(capped);
  TEST_ASSERT_EQUAL_INT16(20, t.l);
  TEST_ASSERT_EQUAL_INT16(40, t.r);
}

void test_cap_obstacle_slows_forward_only() {
  bool capped = false;
  Tracks t = capLite(Tracks{60, 60}, 100, 325, 15, 50, true, capped);  // halfway between 150 and 500 mm
  TEST_ASSERT_EQUAL_INT16(30, t.l);
  t = capLite(Tracks{-60, -60}, 100, 325, 15, 50, true, capped);       // reversing: not slowed
  TEST_ASSERT_EQUAL_INT16(-60, t.l);
  t = capLite(Tracks{60, 60}, 100, 325, 15, 50, false, capped);        // brake off: not slowed
  TEST_ASSERT_EQUAL_INT16(60, t.l);
  t = capLite(Tracks{60, 60}, 100, 0xFFFE, 15, 50, true, capped);      // sensor fault: not slowed
  TEST_ASSERT_EQUAL_INT16(60, t.l);
}

void test_ramp_accel_and_faster_decel() {
  RampLite r;
  r.step(100, 200);                         // 200 %/s → 2 % per 10 ms tick
  TEST_ASSERT_EQUAL_INT16(20, r.value());   // 0.1 % units
  for (int i = 0; i < 9; ++i) r.step(100, 200);
  TEST_ASSERT_EQUAL_INT16(200, r.value());
  r.step(0, 200);                           // decel twice as fast: −4 %
  TEST_ASSERT_EQUAL_INT16(160, r.value());
}

void test_to_pwm_deadband() {
  TEST_ASSERT_EQUAL_INT16(0, toPwmLite(4, false, 110, 90));
  TEST_ASSERT_EQUAL_INT16(110, toPwmLite(5, false, 110, 90));   // start PWM at the first 0.5 %
  TEST_ASSERT_EQUAL_INT16(222, toPwmLite(800, true, 110, 90));  // 90 + 0.8 × 165
  TEST_ASSERT_EQUAL_INT16(-255, toPwmLite(-1000, true, 110, 90));
}

// Final review M2: a DRIVE outside ±100 (the decoder accepts −128) is clamped, so the car-style reverse flip
// of −128 cannot stay −128 and turn the wrong way.
void test_drive_input_clamped() {
  TEST_ASSERT_EQUAL_INT8(-100, clampDriveLite(-128));
  TEST_ASSERT_EQUAL_INT8(100, clampDriveLite(127));
  TEST_ASSERT_EQUAL_INT8(-40, clampDriveLite(-40));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_gzl_starts_from_stored);
  RUN_TEST(test_gzl_learns_after_two_agreeing_windows);
  RUN_TEST(test_gzl_ignores_a_bump);
  RUN_TEST(test_gzl_motion_restarts_settle);
  RUN_TEST(test_gzl_disagreeing_windows_not_learned);
  RUN_TEST(test_gzl_rejects_out_of_range);
  RUN_TEST(test_gzl_reseed_only_on_change);
  RUN_TEST(test_yaw_integrates_ccw);
  RUN_TEST(test_yaw_wraps_below_zero);
  RUN_TEST(test_yaw_still_clamp_only_when_stationary);
  RUN_TEST(test_heading_gains_from_settings);
  RUN_TEST(test_heading_steering_is_open_loop);
  RUN_TEST(test_heading_captures_then_corrects_drift);
  RUN_TEST(test_heading_rate_damping);
  RUN_TEST(test_heading_waits_for_turn_to_settle_max_300ms);
  RUN_TEST(test_heading_wraps_short_way);
  RUN_TEST(test_heading_gyro_lost_is_open_loop);
  RUN_TEST(test_heading_stop_resets);
  RUN_TEST(test_steer_lite_car_flip_except_auto);
  RUN_TEST(test_mix_steering_scaled_gyro_not);
  RUN_TEST(test_mix_turning_priority);
  RUN_TEST(test_angle_mix_45_is_forward_arc);
  RUN_TEST(test_angle_mix_spin_only_near_sideways);
  RUN_TEST(test_angle_mix_never_over_max);
  RUN_TEST(test_angle_mix_reverse_both_tracks_back);
  RUN_TEST(test_ir_axes_unchanged);
  RUN_TEST(test_straight_with_correction_unchanged);
  RUN_TEST(test_cap_keeps_ratio);
  RUN_TEST(test_cap_obstacle_slows_forward_only);
  RUN_TEST(test_ramp_accel_and_faster_decel);
  RUN_TEST(test_to_pwm_deadband);
  RUN_TEST(test_drive_input_clamped);
  // LITE-MOTION-TESTS-END
  return UNITY_END();
}

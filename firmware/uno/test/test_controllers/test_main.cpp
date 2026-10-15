#include <unity.h>

#include "domain/motion/heading_controller.h"
#include "domain/motion/speed_controller.h"

using namespace cq;

void setUp() {}
void tearDown() {}

namespace {
SpeedConfig closedCfg() { return SpeedConfig{70, 55, 2500, {0.15f, 0.8f, 0.0f}, true}; }
HeadingConfig headingCfg() { return HeadingConfig{{1.2f, 0.4f, 0.02f}, {3.0f, 0.0f, 0.1f}, 180}; }
}  // namespace

void test_open_loop_is_feedforward_only() {
  SpeedController s;
  SpeedConfig c = closedCfg();
  c.closed_loop = false;
  s.configure(c);
  TEST_ASSERT_EQUAL(155, s.update(50.0f, 0.0f, 0.01f, 255, true, false));
  TEST_ASSERT_EQUAL(0, s.update(0.0f, 0.0f, 0.01f, 255, true, false));
}

void test_closed_loop_pushes_when_slow() {
  SpeedController s;
  s.configure(closedCfg());
  const int16_t first = s.update(50.0f, 1250.0f, 0.01f, 255, true, false);  // on target (50 % of 2500 cps)
  SpeedController t;
  t.configure(closedCfg());
  const int16_t slow = t.update(50.0f, 500.0f, 0.01f, 255, true, false);    // 20 % actual
  TEST_ASSERT_TRUE(slow > first);
}

void test_integrator_frozen_when_capped() {
  SpeedController s;
  s.configure(closedCfg());
  const int16_t limit = pwmLimitForCap(20, 55);
  TEST_ASSERT_EQUAL(115, limit);  // 20 % → 95 PWM + 20 headroom
  int16_t out = 0;
  float i_frozen = 0.0f;
  for (int i = 0; i < 500; ++i) {  // 5 s with the track held: actual stays 0 (output-saturation anti-windup)
    out = s.update(20.0f, 0.0f, 0.01f, limit, true, false);
    if (i == 99) i_frozen = s.integrator();  // the output reached the limit within ~0.5 s
  }
  TEST_ASSERT_TRUE(s.saturated());
  TEST_ASSERT_TRUE(s.integrator() < 15.0f);  // without the freeze it would reach 80
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, i_frozen, s.integrator());  // nothing integrated while on the limit
  out = s.update(20.0f, 500.0f, 0.01f, limit, true, false);  // released and on target
  TEST_ASSERT_TRUE(out < limit);
}

void test_integrator_frozen_while_target_capped() {
  SpeedController s;
  s.configure(closedCfg());
  for (int i = 0; i < 100; ++i) {  // TargetLimiter capped the target: no wind-up even below the PWM limit
    TEST_ASSERT_TRUE(s.update(20.0f, 0.0f, 0.01f, 255, true, true) < 255);
  }
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, s.integrator());
  s.update(20.0f, 0.0f, 0.01f, 255, true, false);  // cap lifted → integrates again
  TEST_ASSERT_TRUE(s.integrator() > 0.0f);
}

void test_rate_mode_feedforward_and_feedback() {
  HeadingController h;
  h.configure(headingCfg());
  const float w = h.update(HeadingMode::kRate, 0.0f, 90.0f, 0.0f, 0.0f, 0.0f, 0.01f, false);
  TEST_ASSERT_TRUE(w > 50.0f);  // 50 % feed-forward (90 of 180 °/s) plus P on the 90 °/s error
  HeadingController g;
  g.configure(headingCfg());
  const float w2 = g.update(HeadingMode::kRate, 0.0f, 90.0f, 0.0f, 0.0f, 90.0f, 0.01f, false);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 50.0f, w2);
}

void test_hold_corrects_drift_with_wraparound() {
  HeadingController h;
  h.configure(headingCfg());
  float w = h.update(HeadingMode::kHold, 0.0f, 0.0f, 359.0f, 4.0f, 0.0f, 0.01f, false);  // 5° left of target
  TEST_ASSERT_TRUE(w < 0.0f);  // turn back right (CW)
  w = h.update(HeadingMode::kHold, 0.0f, 0.0f, 1.0f, 356.0f, 0.0f, 0.01f, false);  // 5° right of target
  TEST_ASSERT_TRUE(w > 0.0f);
}

void test_off_mode_passes_stick_through() {
  HeadingController h;
  h.configure(headingCfg());
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 30.0f, h.update(HeadingMode::kOff, 30.0f, 0, 0, 0, 0, 0.01f, false));
}

void test_teleop_captures_heading_when_stick_centres() {
  HeadingController h;
  h.configure(headingCfg());
  h.teleop(50.0f, 40.0f, 10.0f, 60.0f, 0.01f, false);  // turning
  TEST_ASSERT_FALSE(h.holding());
  h.teleop(50.0f, 0.0f, 20.0f, 30.0f, 0.01f, false);   // centred, still rotating: not yet captured
  TEST_ASSERT_FALSE(h.holding());
  h.teleop(50.0f, 0.0f, 22.0f, 2.0f, 0.01f, false);    // rotation settled → capture 22°
  TEST_ASSERT_TRUE(h.holding());
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 22.0f, h.heldHeading());
  const float w = h.teleop(50.0f, 0.0f, 27.0f, 0.0f, 0.01f, false);  // drifted left 5°
  TEST_ASSERT_TRUE(w < 0.0f);
  h.teleop(0.0f, 0.0f, 27.0f, 0.0f, 0.01f, false);  // stopped: hold released
  TEST_ASSERT_FALSE(h.holding());
}

// design-notes §41 (owner): the gyro only keeps the tank straight. Any steering input is passed
// through open-loop - no rate loop fighting the tracks (that made turns step).
void test_teleop_steering_is_open_loop() {
  HeadingController h;
  h.configure(headingCfg());
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 40.0f, h.teleop(0.0f, 40.0f, 10.0f, 0.0f, 0.01f, false));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 40.0f, h.teleop(0.0f, 40.0f, 30.0f, 200.0f, 0.01f, false));  // fast spin: unchanged
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -25.0f, h.teleop(60.0f, -25.0f, 30.0f, -50.0f, 0.01f, false));  // curve
}

// After steering ends, nothing is commanded while the tank still rotates; then the heading is captured.
void test_teleop_no_rate_braking_after_steering() {
  HeadingController h;
  h.configure(headingCfg());
  h.teleop(50.0f, 40.0f, 10.0f, 60.0f, 0.01f, false);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, h.teleop(50.0f, 0.0f, 20.0f, 30.0f, 0.01f, false));
}

// The tank's own pull can keep it rotating above the capture rate after a curve (floor test 2026-10-14:
// ~12 % track difference needed). Waiting for the rate to settle must not disable heading hold for good:
// after kSettleS the heading is captured anyway and corrected.
void test_teleop_captures_after_settle_time_despite_pull() {
  HeadingController h;
  h.configure(headingCfg());
  h.teleop(50.0f, 40.0f, 10.0f, 60.0f, 0.01f, false);  // curve
  float w = 0.0f;
  for (int i = 0; i < 40; ++i) w = h.teleop(50.0f, 0.0f, 20.0f + i * 0.2f, 20.0f, 0.01f, false);  // 0.4 s at 20 deg/s
  TEST_ASSERT_TRUE(h.holding());
  TEST_ASSERT_TRUE(w < 0.0f);  // braking the pull
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_open_loop_is_feedforward_only);
  RUN_TEST(test_closed_loop_pushes_when_slow);
  RUN_TEST(test_integrator_frozen_when_capped);
  RUN_TEST(test_integrator_frozen_while_target_capped);
  RUN_TEST(test_rate_mode_feedforward_and_feedback);
  RUN_TEST(test_hold_corrects_drift_with_wraparound);
  RUN_TEST(test_off_mode_passes_stick_through);
  RUN_TEST(test_teleop_captures_heading_when_stick_centres);
  RUN_TEST(test_teleop_steering_is_open_loop);
  RUN_TEST(test_teleop_no_rate_braking_after_steering);
  RUN_TEST(test_teleop_captures_after_settle_time_despite_pull);
  return UNITY_END();
}

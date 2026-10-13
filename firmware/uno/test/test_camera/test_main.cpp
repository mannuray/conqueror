#include <unity.h>

#include "domain/camera/camera_head.h"
#include "domain/camera/servo_profile.h"

using namespace cq;

void setUp() {}
void tearDown() {}

namespace {
const CameraConfig kCfg{0, 180, 30, 110, 0, 0, 120, 400, 500, false};

void run(CameraHead& h, uint32_t& t, uint32_t until_ms) {
  for (; t < until_ms; t += 20) h.tick(0.02f, t);
}

// The ESP32 streams CAM_JOY at 20 Hz while the stick is held.
void runJoy(CameraHead& h, uint32_t& t, uint32_t until_ms, int8_t x, int8_t y) {
  for (; t < until_ms; t += 20) {
    if (t % 50 < 20) h.joy(x, y, true, t);
    h.tick(0.02f, t);
  }
}
}  // namespace

void test_profile_respects_speed_and_lands_exactly() {
  ServoProfile p;
  p.reset(90.0f);
  p.setLimits(120.0f, 400.0f);
  p.setTarget(150.0f);
  float max_step = 0.0f;
  float prev = 90.0f;
  int ticks = 0;
  while (p.moving() && ticks < 500) {
    const float pos = p.step(0.02f);
    const float stepd = pos - prev;
    if (stepd > max_step) max_step = stepd;
    prev = pos;
    ++ticks;
  }
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 150.0f, p.position());
  TEST_ASSERT_TRUE(max_step <= 120.0f * 0.02f + 1e-3f);
  TEST_ASSERT_INT_WITHIN(5, 40, ticks);  // 60° at 120 °/s ≈ 0.5 s + 0.3 s accel/decel
}

void test_joy_is_absolute_and_release_returns_home() {
  CameraHead h;
  h.configure(kCfg);
  uint32_t t = 0;
  runJoy(h, t, 2000, 100, 0);  // full right
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, h.panDeg());
  runJoy(h, t, 3000, 0, 100);  // straight, full up
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 90.0f, h.panDeg());
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 110.0f, h.tiltDeg());
  h.joy(0, 0, false, t);   // released
  run(h, t, t + 400);      // within the 500 ms return delay: stays
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 110.0f, h.tiltDeg());
  run(h, t, t + 1500);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, CameraHead::kHomeTilt, h.tiltDeg());
}

void test_lock_holds_position_on_release() {
  CameraHead h;
  h.configure(kCfg);
  uint32_t t = 0;
  h.lock(true);
  runJoy(h, t, 1500, -50, 0);
  h.joy(0, 0, false, t);
  run(h, t, t + 2000);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 135.0f, h.panDeg());
}

void test_joy_stream_timeout_counts_as_release() {
  CameraHead h;
  h.configure(kCfg);
  uint32_t t = 0;
  runJoy(h, t, 1500, 0, -100);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 30.0f, h.tiltDeg());
  run(h, t, 4000);  // stream stops: 500 ms timeout + 500 ms delay, then home
  TEST_ASSERT_FLOAT_WITHIN(0.5f, CameraHead::kHomeTilt, h.tiltDeg());
}

void test_set_clamps_and_nudge_is_relative() {
  CameraHead h;
  h.configure(kCfg);
  uint32_t t = 0;
  h.set(200.0f, 10.0f, 0.0f);
  run(h, t, 3000);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 180.0f, h.panDeg());
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 30.0f, h.tiltDeg());
  h.nudge(-10.0f, 5.0f);
  run(h, t, 4000);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 170.0f, h.panDeg());
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 35.0f, h.tiltDeg());
  h.home();
  run(h, t, 6000);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 90.0f, h.panDeg());
}

void test_trim_applies_to_output_only() {
  CameraConfig c = kCfg;
  c.pan_trim_dd = -25;
  CameraHead h;
  h.configure(c);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.0f, h.panDeg());
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 87.5f, h.panOutDeg());
}

void test_idle_detach() {
  CameraConfig c = kCfg;
  c.idle_detach = true;
  CameraHead h;
  h.configure(c);
  uint32_t t = 0;
  h.nudge(10.0f, 0.0f);
  run(h, t, 500);
  TEST_ASSERT_TRUE(h.wantAttached());
  run(h, t, 3000);
  TEST_ASSERT_FALSE(h.wantAttached());
  h.nudge(5.0f, 0.0f);
  TEST_ASSERT_TRUE(h.wantAttached());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_profile_respects_speed_and_lands_exactly);
  RUN_TEST(test_joy_is_absolute_and_release_returns_home);
  RUN_TEST(test_lock_holds_position_on_release);
  RUN_TEST(test_joy_stream_timeout_counts_as_release);
  RUN_TEST(test_set_clamps_and_nudge_is_relative);
  RUN_TEST(test_trim_applies_to_output_only);
  RUN_TEST(test_idle_detach);
  return UNITY_END();
}

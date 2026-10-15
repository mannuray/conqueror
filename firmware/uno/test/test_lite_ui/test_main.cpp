#include <unity.h>

#include "domain/lite/cam_lite.h"

using namespace cq;

void setUp() {}
void tearDown() {}

void test_cam_limits_and_us() {
  CamLite c;
  c.set(2000, 0);
  TEST_ASSERT_EQUAL_INT16(1800, c.pan());
  TEST_ASSERT_EQUAL_INT16(300, c.tilt());
  TEST_ASSERT_EQUAL_UINT16(544, CamLite::toUs(0));
  TEST_ASSERT_EQUAL_UINT16(2400, CamLite::toUs(1800));
  TEST_ASSERT_EQUAL_UINT16(1472, CamLite::toUs(900));
}

void test_cam_joy_release_returns_home_unless_locked() {
  CamLite c;
  c.joy(100, 0, true, 0);             // full right = 90° lower
  TEST_ASSERT_EQUAL_INT16(0, c.pan());
  c.joy(0, 0, false, 100);
  c.tick(599);
  TEST_ASSERT_EQUAL_INT16(0, c.pan());
  c.tick(600);                         // 500 ms after release
  TEST_ASSERT_EQUAL_INT16(900, c.pan());
  c.lock(true);
  c.joy(100, 0, true, 700);
  c.joy(0, 0, false, 800);
  c.tick(2000);
  TEST_ASSERT_EQUAL_INT16(0, c.pan());  // locked: stays
}

// LLD §8 extra 1 (space allowed it): the servo output follows the target at most 24 deci-degrees per 20 ms tick
// (120 °/s, the full firmware's servo_speed default); pan()/tilt() stay the targets.
void test_cam_smooth_steps() {
  CamLite c;
  c.set(1800, 900);
  c.step();
  TEST_ASSERT_EQUAL_INT16(924, c.outPan());
  TEST_ASSERT_EQUAL_INT16(900, c.outTilt());
  for (int i = 0; i < 100; ++i) c.step();
  TEST_ASSERT_EQUAL_INT16(1800, c.outPan());
  c.set(1790, 900);
  c.step();
  TEST_ASSERT_EQUAL_INT16(1790, c.outPan());  // a small move lands exactly, no overshoot
}

// Final review M1: a large CAM_NUDGE must stop at the limit, not wrap to the opposite one (16-bit int on AVR).
void test_cam_nudge_large_saturates() {
  CamLite c;
  c.set(1700, 900);
  c.nudge(32000, 0);
  TEST_ASSERT_EQUAL_INT16(1800, c.pan());
  c.nudge(-32000, 0);
  TEST_ASSERT_EQUAL_INT16(0, c.pan());
}

// Final review M3: an IR jog key held moves the camera 12 deci-degrees per 20 ms camera tick (60 °/s, as the full
// firmware), not one step per NEC repeat. Pan right (+1) = lower angle.
void test_cam_held_jog_rate() {
  CamLite c;
  c.jog(1, -1);
  for (int i = 0; i < 5; ++i) c.applyJog();
  TEST_ASSERT_EQUAL_INT16(840, c.pan());
  TEST_ASSERT_EQUAL_INT16(840, c.tilt());
  c.jog(0, 0);
  c.applyJog();
  TEST_ASSERT_EQUAL_INT16(840, c.pan());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_cam_limits_and_us);
  RUN_TEST(test_cam_joy_release_returns_home_unless_locked);
  RUN_TEST(test_cam_smooth_steps);
  RUN_TEST(test_cam_nudge_large_saturates);
  RUN_TEST(test_cam_held_jog_rate);
  return UNITY_END();
}

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

// Uno Lite LLD §23: the output follows the target smoothly - a quarter of the gap per tick, at most 24 dd per
// 20 ms tick (120 °/s), speeding up by at most 3 dd per tick, never past the target, landing exactly.
void test_cam_big_move_eases_in_and_lands() {
  CamLite c;
  c.set(1800, 900);
  int16_t last = c.outPan(), step_prev = 0;
  for (int i = 0; i < 120; ++i) {
    c.step();
    const int16_t d = static_cast<int16_t>(c.outPan() - last);
    TEST_ASSERT_TRUE(d >= 0 && d <= CamLite::kStepDd);  // one way, capped
    if (d > step_prev) TEST_ASSERT_TRUE(d - step_prev <= CamLite::kAccelDd);  // eases in
    TEST_ASSERT_TRUE(c.outPan() <= 1800);  // never past the target
    if (i < 3) TEST_ASSERT_EQUAL_INT16(3 * (i + 1), d);  // 3, 6, 9 ...
    step_prev = d;
    last = c.outPan();
  }
  TEST_ASSERT_EQUAL_INT16(1800, c.outPan());
  TEST_ASSERT_EQUAL_INT16(900, c.outTilt());
}

// The phone's stick: +9 dd (1 %) every 3 ticks. The output must move every tick - no stop-and-go.
void test_cam_staircase_target_moves_every_tick() {
  CamLite c;
  int16_t target = 900, last = 900;
  for (int i = 0; i < 90; ++i) {
    if (i % 3 == 0) target = static_cast<int16_t>(target + 9);
    c.set(target, 900);
    c.step();
    if (i >= 2) TEST_ASSERT_TRUE(c.outPan() > last);
    last = c.outPan();
  }
}

// A new target behind the camera mid-move: it turns back without going past the new target.
void test_cam_reversal_does_not_overshoot() {
  CamLite c;
  c.set(1800, 900);
  for (int i = 0; i < 20; ++i) c.step();  // at full speed
  const int16_t back = static_cast<int16_t>(c.outPan() - 100);
  c.set(back, 900);
  int16_t peak = c.outPan();
  for (int i = 0; i < 80; ++i) {
    c.step();
    TEST_ASSERT_TRUE(c.outPan() >= back);
    if (c.outPan() > peak) peak = c.outPan();
  }
  TEST_ASSERT_EQUAL_INT16(back, c.outPan());
  TEST_ASSERT_TRUE(peak - back <= 100 + CamLite::kStepDd);  // at most one more tick forward before turning
}

void test_cam_small_moves_land_exactly() {
  CamLite c;
  c.set(901, 900);
  c.step();
  TEST_ASSERT_EQUAL_INT16(901, c.outPan());
  c.set(891, 900);
  for (int i = 0; i < 20; ++i) {
    c.step();
    TEST_ASSERT_TRUE(c.outPan() >= 891);
  }
  TEST_ASSERT_EQUAL_INT16(891, c.outPan());
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
// firmware), not one step per NEC repeat. Pan right (+1) = lower angle; tilt down (-1) = higher angle (§45).
// Design-notes §45: on this tank a larger tilt angle looks down, so stick up = smaller tilt angle.
void test_cam_joy_up_is_smaller_tilt() {
  CamLite c;
  c.joy(0, 50, true, 0);
  TEST_ASSERT_EQUAL_INT16(450, c.tilt());
  c.joy(0, -10, true, 20);
  TEST_ASSERT_EQUAL_INT16(990, c.tilt());
}

void test_cam_held_jog_rate() {
  CamLite c;
  c.jog(1, -1);
  for (int i = 0; i < 5; ++i) c.applyJog();
  TEST_ASSERT_EQUAL_INT16(840, c.pan());
  TEST_ASSERT_EQUAL_INT16(960, c.tilt());
  c.jog(0, 0);
  c.applyJog();
  TEST_ASSERT_EQUAL_INT16(840, c.pan());
}

// Second review M3: a camera command cancels a pending joystick return (as the full CameraHead).
void test_cam_set_cancels_pending_return() {
  CamLite c;
  c.joy(100, 0, true, 0);
  c.joy(0, 0, false, 100);  // released: return home due at 600
  c.set(400, 900);          // CAM_SET / IR jog at 200
  c.tick(700);
  TEST_ASSERT_EQUAL_INT16(400, c.pan());
}

// §D5.3: a joystick held without refresh for 500 ms counts as released (phone lost) → home 500 ms later.
void test_cam_joy_timeout_returns_home() {
  CamLite c;
  c.joy(100, 0, true, 0);
  c.tick(499);
  TEST_ASSERT_EQUAL_INT16(0, c.pan());
  c.tick(500);   // no refresh for 500 ms: released at 500
  c.tick(999);
  TEST_ASSERT_EQUAL_INT16(0, c.pan());
  c.tick(1000);  // return delay passed
  TEST_ASSERT_EQUAL_INT16(900, c.pan());
}

// §D5.4: servos released after 2 s still, wanted again as soon as there is something to move.
void test_cam_idle_detach_after_2s() {
  CamLite c;
  c.tick(0);
  TEST_ASSERT_TRUE(c.wantAttached());
  c.tick(1999);
  TEST_ASSERT_TRUE(c.wantAttached());
  c.tick(2000);
  TEST_ASSERT_FALSE(c.wantAttached());
  c.set(1000, 900);
  c.tick(2010);
  TEST_ASSERT_TRUE(c.wantAttached());
}

// Spec 02 §D6.1 (bug): one IR jog step (12 dd) is completed inside a tick by the smooth step; it must still wake
// the detached servos.
void test_cam_small_jog_wakes_servos() {
  CamLite c;
  c.tick(0);
  c.tick(2000);
  TEST_ASSERT_FALSE(c.wantAttached());
  c.jog(1, 0);
  c.applyJog();
  c.step();  // the 12 dd move is done here, before the next tick
  c.tick(2020);
  TEST_ASSERT_TRUE(c.wantAttached());
}

// Uno Lite LLD §22: the trim is added only at the servo; the logical angles (telemetry) stay as they are.
void test_cam_trim_moves_only_the_servo() {
  CamLite c;
  c.setTrim(-50, 20);
  for (int i = 0; i < 10; ++i) c.step();
  TEST_ASSERT_EQUAL_INT16(900, c.outPan());
  TEST_ASSERT_EQUAL_INT16(900, c.outTilt());
  TEST_ASSERT_EQUAL_INT16(850, c.servoPan());
  TEST_ASSERT_EQUAL_INT16(920, c.servoTilt());
  c.set(30, 1100);
  for (int i = 0; i < 60; ++i) c.step();
  TEST_ASSERT_EQUAL_INT16(30, c.outPan());
  TEST_ASSERT_EQUAL_INT16(0, c.servoPan());  // the servo's own 0..180° stop
  TEST_ASSERT_EQUAL_INT16(1120, c.servoTilt());
  c.setTrim(50, 0);
  c.set(1800, 900);
  for (int i = 0; i < 100; ++i) c.step();  // 1770 dd at 24 per step
  TEST_ASSERT_EQUAL_INT16(1800, c.servoPan());
}

// A new trim wakes released servos, so the camera moves to the new centre at once.
void test_cam_trim_wakes_servos() {
  CamLite c;
  c.tick(0);
  c.tick(2000);
  TEST_ASSERT_FALSE(c.wantAttached());
  c.setTrim(-50, 0);
  c.tick(2020);
  TEST_ASSERT_TRUE(c.wantAttached());
}

// Final review M3: applySettings() runs after every SETTING_SET; an unchanged trim must not wake released servos.
void test_cam_same_trim_does_not_wake_servos() {
  CamLite c;
  c.setTrim(-50, 0);
  c.tick(0);
  c.tick(2000);
  TEST_ASSERT_FALSE(c.wantAttached());
  c.setTrim(-50, 0);
  c.tick(2020);
  TEST_ASSERT_FALSE(c.wantAttached());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_cam_limits_and_us);
  RUN_TEST(test_cam_joy_release_returns_home_unless_locked);
  RUN_TEST(test_cam_big_move_eases_in_and_lands);
  RUN_TEST(test_cam_staircase_target_moves_every_tick);
  RUN_TEST(test_cam_reversal_does_not_overshoot);
  RUN_TEST(test_cam_small_moves_land_exactly);
  RUN_TEST(test_cam_nudge_large_saturates);
  RUN_TEST(test_cam_held_jog_rate);
  RUN_TEST(test_cam_joy_up_is_smaller_tilt);
  RUN_TEST(test_cam_set_cancels_pending_return);
  RUN_TEST(test_cam_joy_timeout_returns_home);
  RUN_TEST(test_cam_idle_detach_after_2s);
  RUN_TEST(test_cam_small_jog_wakes_servos);
  RUN_TEST(test_cam_trim_moves_only_the_servo);
  RUN_TEST(test_cam_trim_wakes_servos);
  RUN_TEST(test_cam_same_trim_does_not_wake_servos);
  return UNITY_END();
}

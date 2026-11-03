// Plan N3 Task 4: the driven path from encoder counts and yaw - straight, back, after a turn, an arc (LLD §11).
#include <math.h>
#include <unity.h>

#include "logo/odometry.h"
#include "logo/relative.h"

using namespace cq::logo;

namespace {
constexpr uint16_t kCpm = 7514;
Odometry g_o;
}  // namespace

void setUp() { g_o.start(1000, 2000, 500, kCpm); }
void tearDown() {}

void test_straight_ahead_and_back() {
  Pose p = g_o.update(1000 + kCpm, 2000 + kCpm, 500, 40, 40);
  TEST_ASSERT_INT_WITHIN(1, 0, p.x_mm);
  TEST_ASSERT_INT_WITHIN(1, 1000, p.y_mm);
  // back 50 cm: the Uno counts rising edges only, so the counts RISE in reverse too - the motor command gives the
  // direction (owner's floor test 2026-10-20: FD 30 BK 30 was drawn as 120 cm forward)
  p = g_o.update(1000 + kCpm + kCpm / 2, 2000 + kCpm + kCpm / 2, 500, -40, -40);
  TEST_ASSERT_INT_WITHIN(1, 500, p.y_mm);
  TEST_ASSERT_EQUAL(0, p.heading_deg);
}

void test_rolling_on_after_reverse_keeps_going_back() {
  g_o.update(1000 + kCpm / 10, 2000 + kCpm / 10, 500, -40, -40);   // back 10 cm
  const Pose p = g_o.update(1000 + kCpm / 5, 2000 + kCpm / 5, 500, 0, 0);  // command 0: rolls 10 cm more back
  TEST_ASSERT_INT_WITHIN(1, -200, p.y_mm);
}

void test_after_a_right_turn_x_grows() {
  g_o.update(1000, 2000, static_cast<uint16_t>(500 + 36000 - 9000));  // spin right 90° on the spot
  const Pose p = g_o.update(1000 + kCpm / 2, 2000 + kCpm / 2, static_cast<uint16_t>(500 + 36000 - 9000));
  TEST_ASSERT_EQUAL(90, p.heading_deg);
  TEST_ASSERT_INT_WITHIN(1, 500, p.x_mm);
  TEST_ASSERT_INT_WITHIN(1, 0, p.y_mm);
}

void test_a_quarter_arc_ends_within_1_cm() {
  // ARC RIGHT 90 50: 90 steps of 1° along a 500 mm circle, centre to the right.
  const double step_mm = 500.0 * 3.14159265358979 / 180.0;
  int32_t l = 1000, r = 2000;
  Pose p;
  for (int i = 1; i <= 90; ++i) {
    l += static_cast<int32_t>(lround(step_mm * kCpm / 1000.0));
    r += static_cast<int32_t>(lround(step_mm * kCpm / 1000.0));
    p = g_o.update(l, r, static_cast<uint16_t>((500 + 36000 - i * 100) % 36000));
  }
  TEST_ASSERT_INT_WITHIN(10, 500, p.x_mm);
  TEST_ASSERT_INT_WITHIN(10, 500, p.y_mm);
  TEST_ASSERT_EQUAL(90, p.heading_deg);
}

void test_yaw_wraps_through_zero() {
  g_o.start(0, 0, 100, kCpm);
  const Pose p = g_o.update(0, 0, 35900);  // 2° right of the start through 0
  TEST_ASSERT_EQUAL(2, p.heading_deg);
}

// Plan Z2a Task 6 (Review Focus 4): a run-frame point seen from the tank now - forward / left, cm. Odometry's
// heading is right = + (the ledger's ruling), so "turned right 90°" is heading +90.
void test_relative_signs() {
  Pose p;
  p.y_mm = 500;  // FORWARD 50: the start is 50 cm behind
  FwdLeft r = relative(p, 0, 0);
  TEST_ASSERT_EQUAL(-50, r.fwd_cm);
  TEST_ASSERT_EQUAL(0, r.left_cm);
  p = Pose();
  p.heading_deg = 90;  // turned right on the spot
  r = relative(p, 0, 300);  // a point that was ahead of the start is now on the tank's left
  TEST_ASSERT_EQUAL(0, r.fwd_cm);
  TEST_ASSERT_EQUAL(30, r.left_cm);
  r = relative(p, 300, 0);  // a point to the start's right is now ahead
  TEST_ASSERT_EQUAL(30, r.fwd_cm);
  TEST_ASSERT_EQUAL(0, r.left_cm);
  p.heading_deg = -90;  // turned left: the start's right is now behind
  r = relative(p, 300, 0);
  TEST_ASSERT_EQUAL(-30, r.fwd_cm);
  TEST_ASSERT_EQUAL(0, r.left_cm);
}

void test_relative_rounds_and_clamps() {
  Pose p;
  FwdLeft r = relative(p, -14, 26);  // 2.6 cm ahead, 1.4 cm left
  TEST_ASSERT_EQUAL(3, r.fwd_cm);
  TEST_ASSERT_EQUAL(1, r.left_cm);
  r = relative(p, 0, 400000000);  // never wraps
  TEST_ASSERT_EQUAL(32767, r.fwd_cm);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_straight_ahead_and_back);
  RUN_TEST(test_rolling_on_after_reverse_keeps_going_back);
  RUN_TEST(test_after_a_right_turn_x_grows);
  RUN_TEST(test_a_quarter_arc_ends_within_1_cm);
  RUN_TEST(test_yaw_wraps_through_zero);
  RUN_TEST(test_relative_signs);
  RUN_TEST(test_relative_rounds_and_clamps);
  return UNITY_END();
}

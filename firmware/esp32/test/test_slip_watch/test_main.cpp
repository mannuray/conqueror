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
int32_t drive(SlipWatch& w, uint32_t from, uint32_t ms, int32_t l_per_s, int32_t r_per_s, int16_t gyro_ddps) {
  int32_t l = 0, r = 0;
  for (uint32_t t = 0; t <= ms; t += 20) {
    if (w.step(l, r, gyro_ddps, kCpm, from + t)) return static_cast<int32_t>(t);
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
    TEST_ASSERT_FALSE(w.step(l, r, 0, kCpm, 1000 + t));
    l += 35;
    r += t < 1000 ? 6 : 35;
  }
}

// The counts rise in both directions (the Uno counts rising edges), so the rule compares sizes only: backing up with a
// slipping track is held just the same (final review M1: no direction input - it could not change the decision).
void test_backing_up_with_a_slipping_track_is_held() {
  SlipWatch w;
  w.reset();
  const int32_t at = drive(w, 1000, 4000, 330, 1750, 0);
  TEST_ASSERT_TRUE(at >= 1500 && at <= 2100);
}

// Final review M2: a window much longer than 0.5 s (frames missing) is not judged - it starts again - so one long
// window can never reach the 1.5 s hold on its own.
void test_a_long_gap_is_not_judged() {
  SlipWatch w;
  w.reset();
  TEST_ASSERT_FALSE(w.step(0, 0, 0, kCpm, 1000));
  TEST_ASSERT_FALSE(w.step(2800, 500, 0, kCpm, 2700));  // 1.7 s later, slipping counts
  TEST_ASSERT_FALSE(w.step(2835, 507, 0, kCpm, 2720));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_table_leg_is_held_after_one_and_a_half_seconds);
  RUN_TEST(test_a_real_curve_is_never_held);
  RUN_TEST(test_equal_tracks_are_never_held);
  RUN_TEST(test_a_short_slip_is_not_held);
  RUN_TEST(test_backing_up_with_a_slipping_track_is_held);
  RUN_TEST(test_a_long_gap_is_not_judged);
  return UNITY_END();
}

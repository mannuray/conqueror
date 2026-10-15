#include <unity.h>

#include "domain/lite/tilt_lite.h"

using namespace cq;

void setUp() {}
void tearDown() {}

namespace {
// Feeds n samples every 5 ms of a constant accel vector; returns the last Change seen that was not kNone.
TiltLite::Change feedA(TiltLite& t, int16_t ax, int16_t ay, int16_t az, int n, uint32_t& now) {
  TiltLite::Change last = TiltLite::Change::kNone;
  for (int i = 0; i < n; ++i, now += 5) {
    const TiltLite::Change c = t.update(ax, ay, az, 45, now);
    if (c != TiltLite::Change::kNone) last = c;
  }
  return last;
}
}  // namespace

void test_tilt_level_stays_level() {
  TiltLite t;
  uint32_t now = 0;
  feedA(t, 0, 0, 8192, 200, now);
  TEST_ASSERT_FALSE(t.tilted());
  TEST_ASSERT_FALSE(t.lifted());
}

void test_tilt_over_limit_needs_300ms() {
  TiltLite t;
  uint32_t now = 0;
  feedA(t, 0, 0, 8192, 100, now);
  feedA(t, 6554, 0, 4588, 40, now);  // ≈ 55° nose up, 200 ms (filter settles in ~40 ms)
  TEST_ASSERT_FALSE(t.tilted());
  TEST_ASSERT_EQUAL(TiltLite::Change::kTilted, feedA(t, 6554, 0, 4588, 40, now));
  TEST_ASSERT_TRUE(t.tilted());
  TEST_ASSERT_EQUAL(TiltLite::Change::kLevel, feedA(t, 0, 0, 8192, 40, now));  // back level (< 40°)
}

void test_tilt_upside_down_is_tilted() {
  TiltLite t;
  uint32_t now = 0;
  feedA(t, 0, 0, -8192, 200, now);
  TEST_ASSERT_TRUE(t.tilted());
}

void test_lift_and_land() {
  TiltLite t;
  uint32_t now = 0;
  feedA(t, 0, 0, 8192, 100, now);
  TEST_ASSERT_EQUAL(TiltLite::Change::kLifted, feedA(t, 0, 0, 2000, 40, now));  // |az| < 0.6 g for 60 ms
  TEST_ASSERT_TRUE(t.lifted());
  TEST_ASSERT_EQUAL(TiltLite::Change::kLanded, feedA(t, 0, 0, 8192, 200, now));  // 1 g for 500 ms
  TEST_ASSERT_FALSE(t.lifted());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_tilt_level_stays_level);
  RUN_TEST(test_tilt_over_limit_needs_300ms);
  RUN_TEST(test_tilt_upside_down_is_tilted);
  RUN_TEST(test_lift_and_land);
  // LITE-SENSORS-TESTS-END
  return UNITY_END();
}

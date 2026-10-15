#include <unity.h>

#include "domain/lite/tilt_lite.h"
#include "domain/lite/range_lite.h"

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

void test_range_median_of_three() {
  RangeLite r;
  r.onReading(EchoState::kReady, 1166, 0);  // 200 mm
  r.onReading(EchoState::kReady, 5831, 0);  // 1000 mm (spike)
  r.onReading(EchoState::kReady, 1166, 0);
  TEST_ASSERT_UINT16_WITHIN(1, 200, r.distMm());
}

void test_range_fault_after_three_failures_and_recovers() {
  RangeLite r;
  r.onReading(EchoState::kNoEcho, 0, 0);
  r.onReading(EchoState::kNoEcho, 0, 0);
  TEST_ASSERT_EQUAL(RangeLite::Change::kFault, r.onReading(EchoState::kNoEcho, 0, 0));
  TEST_ASSERT_EQUAL_UINT16(RangeLite::kFault, r.distMm());
  TEST_ASSERT_EQUAL(RangeLite::Change::kRecovered, r.onReading(EchoState::kClear, 0, 0));
}

void test_range_clear_never_faults() {
  RangeLite r;
  for (int i = 0; i < 10; ++i) r.onReading(EchoState::kClear, 0, 0);
  TEST_ASSERT_FALSE(r.fault());
  TEST_ASSERT_EQUAL_UINT16(RangeLite::kClear, r.distMm());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_tilt_level_stays_level);
  RUN_TEST(test_tilt_over_limit_needs_300ms);
  RUN_TEST(test_tilt_upside_down_is_tilted);
  RUN_TEST(test_lift_and_land);
  RUN_TEST(test_range_median_of_three);
  RUN_TEST(test_range_fault_after_three_failures_and_recovers);
  RUN_TEST(test_range_clear_never_faults);
  // LITE-SENSORS-TESTS-END
  return UNITY_END();
}

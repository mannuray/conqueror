#include <unity.h>

#include "domain/lite/gyro_zero_lite.h"
#include "domain/lite/yaw_lite.h"

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
  // LITE-MOTION-TESTS-END
  return UNITY_END();
}

// Uno Lite LLD §21: integer atan2 (0.01°) and isqrt32 - no floats on the Lite.
#include <math.h>
#include <unity.h>

#include "domain/lite/int_math.h"

using cq::iatan2Cdeg;
using cq::ihypot16;
using cq::isqrt16;

void setUp() {}
void tearDown() {}

void test_iatan2_matches_atan2_every_degree() {
  for (int deg = -179; deg <= 180; ++deg) {
    const double r = deg * M_PI / 180.0;
    const int32_t y = static_cast<int32_t>(lround(sin(r) * 128)), x = static_cast<int32_t>(lround(cos(r) * 128));
    const double want = atan2(static_cast<double>(y), static_cast<double>(x)) * 18000.0 / M_PI;
    TEST_ASSERT_INT_WITHIN_MESSAGE(50, static_cast<int>(lround(want)), iatan2Cdeg(y, x), "deg");
  }
}

void test_iatan2_axes_and_zero() {
  TEST_ASSERT_EQUAL(0, iatan2Cdeg(0, 0));
  TEST_ASSERT_EQUAL(9000, iatan2Cdeg(100, 0));
  TEST_ASSERT_EQUAL(-9000, iatan2Cdeg(-100, 0));
  TEST_ASSERT_EQUAL(18000, iatan2Cdeg(0, -100));
}

void test_isqrt_and_hypot() {
  TEST_ASSERT_EQUAL(0u, isqrt16(0));
  TEST_ASSERT_EQUAL(181u, isqrt16(32768));
  TEST_ASSERT_EQUAL(255u, isqrt16(65535));
  TEST_ASSERT_EQUAL(181u, ihypot16(128, 128));
  TEST_ASSERT_UINT_WITHIN(2u, 724u, ihypot16(512, 512));  // scaled down first
  TEST_ASSERT_EQUAL(128u, ihypot16(0, -128));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_iatan2_matches_atan2_every_degree);
  RUN_TEST(test_iatan2_axes_and_zero);
  RUN_TEST(test_isqrt_and_hypot);
  return UNITY_END();
}

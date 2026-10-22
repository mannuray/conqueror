// HeadingScale (bridge LLD R19): the Uno's gyro heading and rate corrected by one scale.
#include <unity.h>

#include "core/heading_scale.h"

using cq::core::HeadingScale;

void setUp() {}
void tearDown() {}

namespace {
struct Out {
  uint16_t yaw;
  int16_t rate;
};
Out feed(HeadingScale& h, uint16_t raw, int16_t rate = 0) {
  Out o{0, 0};
  h.apply(raw, rate, o.yaw, o.rate);
  return o;
}
// Spin `turns` full turns from raw 0 in 3° frames (+ = left), returns the last corrected heading.
uint16_t spin(HeadingScale& h, int turns) {
  int32_t raw = 0;
  uint16_t yaw = feed(h, 0).yaw;
  const int frames = (turns < 0 ? -turns : turns) * 120;
  for (int i = 0; i < frames; ++i) {
    raw += turns < 0 ? -300 : 300;
    yaw = feed(h, static_cast<uint16_t>(((raw % 36000) + 36000) % 36000)).yaw;
  }
  return yaw;
}
}  // namespace

void test_default_scale_passes_the_heading_through() {
  HeadingScale h;
  TEST_ASSERT_EQUAL_UINT16(10000, h.scale());
  TEST_ASSERT_EQUAL_UINT16(12345, feed(h, 12345).yaw);
  TEST_ASSERT_EQUAL_UINT16(12645, feed(h, 12645, 900).yaw);
  TEST_ASSERT_EQUAL_INT16(900, feed(h, 12700, 900).rate);
  TEST_ASSERT_EQUAL_UINT16(0, spin(h, 4));
}

void test_scale_corrects_four_turns_both_ways() {
  HeadingScale left;
  left.setScale(10230);
  TEST_ASSERT_EQUAL_UINT16(3312, spin(left, 4));  // 1440 x 1.023 = 1473.12° -> 33.12°
  HeadingScale right;
  right.setScale(10230);
  TEST_ASSERT_EQUAL_UINT16(32688, spin(right, -4));  // -1473.12° -> 326.88°
}

void test_no_rounding_build_up() {
  HeadingScale h;
  h.setScale(10230);
  uint16_t yaw = feed(h, 0).yaw;
  for (int32_t i = 1; i <= 100000; ++i) yaw = feed(h, static_cast<uint16_t>(i % 36000)).yaw;
  TEST_ASSERT_EQUAL_UINT16(30300, yaw);  // 100 000 cdeg x 1.023 = 102 300 -> wrapped 303.00°
}

void test_first_frame_and_reset_anchor_to_the_raw_heading() {
  HeadingScale h;
  h.setScale(10230);
  TEST_ASSERT_EQUAL_UINT16(9000, feed(h, 9000).yaw);  // first frame: no change
  TEST_ASSERT_EQUAL_UINT16(10023, feed(h, 10000).yaw);  // +10° -> +10.23°
  h.reset();  // the Uno restarted: its heading starts again
  TEST_ASSERT_EQUAL_UINT16(0, feed(h, 0).yaw);
  TEST_ASSERT_EQUAL_UINT16(1023, feed(h, 1000).yaw);
}

void test_a_wrap_is_taken_the_short_way() {
  HeadingScale h;
  h.setScale(10230);
  feed(h, 35900);
  TEST_ASSERT_EQUAL_UINT16(105, feed(h, 100).yaw);  // +2° across 0 -> +2.046°: 35900 + 204.6 -> 104.6 -> 105
  TEST_ASSERT_EQUAL_UINT16(35900, feed(h, 35900).yaw);  // back -2° -> -2.046°: where it was
}

void test_rate_is_scaled_and_rounded() {
  HeadingScale h;
  h.setScale(10230);
  TEST_ASSERT_EQUAL_INT16(665, feed(h, 0, 650).rate);    // 650 x 1.023 = 664.95
  TEST_ASSERT_EQUAL_INT16(-665, feed(h, 0, -650).rate);
  TEST_ASSERT_EQUAL_INT16(0, feed(h, 0, 0).rate);
  h.setScale(11000);
  TEST_ASSERT_EQUAL_INT16(32767, feed(h, 0, 32000).rate);  // kept inside int16
  TEST_ASSERT_EQUAL_INT16(-32767, feed(h, 0, -32000).rate);
}

void test_out_of_range_scale_is_refused() {
  HeadingScale h;
  TEST_ASSERT_FALSE(h.setScale(8999));
  TEST_ASSERT_FALSE(h.setScale(11001));
  TEST_ASSERT_EQUAL_UINT16(10000, h.scale());
  TEST_ASSERT_TRUE(h.setScale(9000));
  TEST_ASSERT_TRUE(h.setScale(11000));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_default_scale_passes_the_heading_through);
  RUN_TEST(test_scale_corrects_four_turns_both_ways);
  RUN_TEST(test_no_rounding_build_up);
  RUN_TEST(test_first_frame_and_reset_anchor_to_the_raw_heading);
  RUN_TEST(test_a_wrap_is_taken_the_short_way);
  RUN_TEST(test_rate_is_scaled_and_rounded);
  RUN_TEST(test_out_of_range_scale_is_refused);
  return UNITY_END();
}

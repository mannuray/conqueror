// Plan I (spec 02 Part F §F1, LLD uno-lite §19): encoder features phase 1 on the Uno Lite.
#include <unity.h>

#include "domain/lite/motion_latch_lite.h"

using namespace cq;

void setUp() {}
void tearDown() {}

// ---------- MotionLatchLite ----------
void test_latch_trips_and_masks_both_tracks() {
  MotionLatchLite l;
  TEST_ASSERT_FALSE(l.latched());
  TEST_ASSERT_EQUAL_UINT8(0, l.stallMask());
  l.trip(MotionLatchLite::Reason::kStall, 1);
  TEST_ASSERT_TRUE(l.latched());
  TEST_ASSERT_EQUAL_UINT8(0x03, l.stallMask());  // owner: stop BOTH tracks
  TEST_ASSERT_EQUAL_INT8(1, l.side());
  TEST_ASSERT_EQUAL(static_cast<int>(MotionLatchLite::Reason::kStall), static_cast<int>(l.reason()));
}

void test_latch_stays_while_command_held() {
  MotionLatchLite l;
  l.trip(MotionLatchLite::Reason::kHeading, -1);
  for (int i = 0; i < 100; ++i) l.clearIfReleased(40, 0);  // driver keeps the stick forward
  TEST_ASSERT_TRUE(l.latched());
  l.clearIfReleased(0, 0);  // released
  TEST_ASSERT_FALSE(l.latched());
  TEST_ASSERT_EQUAL_INT8(-1, l.side());
}

void test_latch_clear_by_stop_and_first_trip_wins() {
  MotionLatchLite l;
  l.trip(MotionLatchLite::Reason::kStall, 0);
  l.trip(MotionLatchLite::Reason::kHeading, -1);  // already latched: keeps the first reason
  TEST_ASSERT_EQUAL(static_cast<int>(MotionLatchLite::Reason::kStall), static_cast<int>(l.reason()));
  l.clear();  // STOP
  TEST_ASSERT_FALSE(l.latched());
  TEST_ASSERT_EQUAL(static_cast<int>(MotionLatchLite::Reason::kNone), static_cast<int>(l.reason()));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_latch_trips_and_masks_both_tracks);
  RUN_TEST(test_latch_stays_while_command_held);
  RUN_TEST(test_latch_clear_by_stop_and_first_trip_wins);
  return UNITY_END();
}

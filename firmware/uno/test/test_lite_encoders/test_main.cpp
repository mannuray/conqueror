// Plan I (spec 02 Part F §F1, LLD uno-lite §19): encoder features phase 1 on the Uno Lite.
#include <unity.h>

#include "domain/lite/motion_latch_lite.h"
#include "domain/lite/track_sync_lite.h"

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

// ---------- TrackSyncLite ----------
void test_sync_equal_counts_no_change() {
  TrackSyncLite s;
  Tracks t{40, 40};
  t = s.apply(t, true, 1000, 1000, 1200, 1200);  // activation: base taken
  t = s.apply(Tracks{40, 40}, true, 1500, 1500, 1200, 1200);
  TEST_ASSERT_EQUAL_INT16(40, t.l);
  TEST_ASSERT_EQUAL_INT16(40, t.r);
}

void test_sync_slows_faster_left() {
  TrackSyncLite s;
  s.apply(Tracks{40, 40}, true, 0, 0, 1200, 1200);
  const Tracks t = s.apply(Tracks{40, 40}, true, 200, 100, 1300, 1200);  // left 100 counts ahead, 100 cps faster
  // c = 100/20 + 100/50 = 7 %: left slowed by 3, right sped up by 4
  TEST_ASSERT_EQUAL_INT16(37, t.l);
  TEST_ASSERT_EQUAL_INT16(44, t.r);
  TEST_ASSERT_EQUAL_INT16(7, s.correctionPct());
}

void test_sync_reverse_slows_faster_side() {
  TrackSyncLite s;
  s.apply(Tracks{-40, -40}, true, 0, 0, 1200, 1200);
  const Tracks t = s.apply(Tracks{-40, -40}, true, 200, 100, 1300, 1200);  // counts always rise
  TEST_ASSERT_EQUAL_INT16(-37, t.l);  // magnitude down: slower
  TEST_ASSERT_EQUAL_INT16(-44, t.r);
}

void test_sync_clamped() {
  TrackSyncLite s;
  s.apply(Tracks{40, 40}, true, 0, 0, 0, 0);
  const Tracks t = s.apply(Tracks{40, 40}, true, 5000, 0, 4000, 0);  // huge error
  TEST_ASSERT_EQUAL_INT16(15, s.correctionPct());
  TEST_ASSERT_EQUAL_INT16(33, t.l);
  TEST_ASSERT_EQUAL_INT16(48, t.r);
}

void test_sync_inactive_when_disabled() {
  TrackSyncLite s;
  const Tracks t = s.apply(Tracks{40, 30}, false, 500, 0, 2000, 0);
  TEST_ASSERT_EQUAL_INT16(40, t.l);
  TEST_ASSERT_EQUAL_INT16(30, t.r);
  TEST_ASSERT_EQUAL_INT16(0, s.correctionPct());
}

void test_sync_rebases_after_inactive() {
  TrackSyncLite s;
  s.apply(Tracks{40, 40}, true, 0, 0, 1200, 1200);
  s.apply(Tracks{40, 40}, true, 300, 100, 1200, 1200);
  s.apply(Tracks{40, 40}, false, 300, 100, 1200, 1200);              // steering: off
  const Tracks t = s.apply(Tracks{40, 40}, true, 300, 100, 1200, 1200);  // new straight stretch: new base
  TEST_ASSERT_EQUAL_INT16(40, t.l);
  TEST_ASSERT_EQUAL_INT16(40, t.r);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_latch_trips_and_masks_both_tracks);
  RUN_TEST(test_latch_stays_while_command_held);
  RUN_TEST(test_latch_clear_by_stop_and_first_trip_wins);
  RUN_TEST(test_sync_equal_counts_no_change);
  RUN_TEST(test_sync_slows_faster_left);
  RUN_TEST(test_sync_reverse_slows_faster_side);
  RUN_TEST(test_sync_clamped);
  RUN_TEST(test_sync_inactive_when_disabled);
  RUN_TEST(test_sync_rebases_after_inactive);
  return UNITY_END();
}

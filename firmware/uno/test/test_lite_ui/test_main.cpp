#include <unity.h>

#include "domain/lite/annunciator_lite.h"
#include "domain/lite/beeper.h"
#include "domain/lite/cam_lite.h"

using namespace cq;

void setUp() {}
void tearDown() {}

namespace {
LiteUiState ready() {
  LiteUiState s{};
  s.synced = true;
  s.owner = proto::Owner::kNone;
  s.driving = true;  // steady base at full level (no breathing) for exact colours
  s.speed_pct = 100;
  return s;
}
}  // namespace

void test_ann_base_colours() {
  AnnunciatorLite a;
  LedSequencer l;
  LiteUiState s = ready();
  a.update(s, 0, l);
  TEST_ASSERT_TRUE((l.output(0) == AnnunciatorLite::kReady));
  s.synced = false;
  a.update(s, 10, l);
  TEST_ASSERT_TRUE((l.output(10) == AnnunciatorLite::kNoLink));
  s.owner = proto::Owner::kIr;
  a.update(s, 20, l);
  TEST_ASSERT_TRUE((l.output(20) == AnnunciatorLite::kIr));
}

void test_ann_alarm_priority() {
  AnnunciatorLite a;
  LedSequencer l;
  LiteUiState s = ready();
  s.locked = true;
  a.update(s, 0, l);
  TEST_ASSERT_TRUE((l.output(500) == AnnunciatorLite::kAlarm));  // solid
  s.locked = false;
  s.batt_low = true;
  a.update(s, 1000, l);
  TEST_ASSERT_TRUE((l.output(1050) == AnnunciatorLite::kLow));    // flash
  TEST_ASSERT_TRUE((l.output(1500) == AnnunciatorLite::kReady));  // base between flashes
}

void test_beeper_plays_then_stops() {
  Beeper b;
  b.beep(1500, 80, 0);
  TEST_ASSERT_EQUAL_UINT16(1500, b.tick(10));
  TEST_ASSERT_EQUAL_UINT16(0, b.tick(80));
}

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

// LLD §8 extra 1 (space allowed it): the servo output follows the target at most 24 deci-degrees per 20 ms tick
// (120 °/s, the full firmware's servo_speed default); pan()/tilt() stay the targets.
void test_cam_smooth_steps() {
  CamLite c;
  c.set(1800, 900);
  c.step();
  TEST_ASSERT_EQUAL_INT16(924, c.outPan());
  TEST_ASSERT_EQUAL_INT16(900, c.outTilt());
  for (int i = 0; i < 100; ++i) c.step();
  TEST_ASSERT_EQUAL_INT16(1800, c.outPan());
  c.set(1790, 900);
  c.step();
  TEST_ASSERT_EQUAL_INT16(1790, c.outPan());  // a small move lands exactly, no overshoot
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ann_base_colours);
  RUN_TEST(test_ann_alarm_priority);
  RUN_TEST(test_beeper_plays_then_stops);
  RUN_TEST(test_cam_limits_and_us);
  RUN_TEST(test_cam_joy_release_returns_home_unless_locked);
  RUN_TEST(test_cam_smooth_steps);
  return UNITY_END();
}

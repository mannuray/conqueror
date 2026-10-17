// Plan I (spec 02 Part F §F1, LLD uno-lite §19): encoder features phase 1 on the Uno Lite.
#include <unity.h>

#include "domain/lite/motion_latch_lite.h"
#include "domain/lite/heading_hold_lite.h"
#include "domain/lite/runaway_guard_lite.h"
#include "domain/lite/stall_guard_lite.h"
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
  for (int i = 0; i < 100; ++i) l.onDriverCommand(40, 0);  // driver keeps the stick forward
  TEST_ASSERT_TRUE(l.latched());
  l.onDriverCommand(0, 0);  // the driver's own 0/0 (released stick)
  TEST_ASSERT_FALSE(l.latched());
  TEST_ASSERT_EQUAL_INT8(-1, l.side());
}

// Final review I1: only the driver's actions release it - the firmware zeroing the command (deadman after a 300 ms
// stream gap, link loss) must not; the still-held stick's next frame must find the tank still stopped.
void test_latch_not_released_by_firmware_zeroing() {
  MotionLatchLite l;
  l.trip(MotionLatchLite::Reason::kStall, 1);
  // (deadman / link loss zero teleop inside the Uno: no driver call here)
  for (int i = 0; i < 10; ++i) l.onDriverCommand(40, 0);  // stream resumes with the stick still held
  TEST_ASSERT_TRUE(l.latched());
  l.onDriverRelease();  // IR drive key let go / STOP
  TEST_ASSERT_FALSE(l.latched());
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

// ---------- StallGuardLite ----------
namespace {
StallGuardLite::Inputs drive(int16_t pl, int16_t pr, uint16_t cl, uint16_t cr, int16_t yaw = 0) {
  return StallGuardLite::Inputs{pl, pr, 90, 90, cl, cr, yaw, true};
}
// expected cps at pwm 130, keep 90: 4000 * 40 / 165 = 969; 15 % = 145
StallGuardLite::Result run(StallGuardLite& g, const StallGuardLite::Inputs& in, uint32_t ms, int8_t& side) {
  StallGuardLite::Result r = StallGuardLite::Result::kNone;
  for (uint32_t t = 0; t <= ms && r == StallGuardLite::Result::kNone; t += 20) r = g.update(in, t, side);
  return r;
}
int R(StallGuardLite::Result r) { return static_cast<int>(r); }
}  // namespace

void test_stall_held_track_trips_in_half_a_second() {
  StallGuardLite g;
  int8_t side = -1;
  // right held: creeping at 40 counts/s (< 145), tank turning toward it (yaw -25 °/s)
  TEST_ASSERT_EQUAL(R(StallGuardLite::Result::kNone), R(g.update(drive(130, 130, 969, 40, -25), 0, side)));
  TEST_ASSERT_EQUAL(R(StallGuardLite::Result::kNone), R(g.update(drive(130, 130, 969, 40, -25), 480, side)));
  TEST_ASSERT_EQUAL(R(StallGuardLite::Result::kStall), R(g.update(drive(130, 130, 969, 40, -25), 500, side)));
  TEST_ASSERT_EQUAL_INT8(1, side);
}

// Owner 2026-10-17: threshold 30 % of the expected speed (a held, slipping track ran at ~40 % and took 3-4 s at 15 %).
void test_stall_slow_but_moving_is_not_stall() {
  StallGuardLite g;
  int8_t side = -1;
  TEST_ASSERT_EQUAL(R(StallGuardLite::Result::kNone), R(run(g, drive(130, 130, 969, 340, -5), 2000, side)));  // 35 %
}

void test_stall_slipping_track_at_25_pct_trips() {
  StallGuardLite g;
  int8_t side = -1;
  TEST_ASSERT_EQUAL(R(StallGuardLite::Result::kStall), R(run(g, drive(130, 130, 969, 242, -20), 600, side)));
  TEST_ASSERT_EQUAL_INT8(1, side);
}

void test_stall_dead_encoder_while_straight_is_fault() {
  StallGuardLite g;
  int8_t side = -1;
  // left reads 0 while the tank drives straight (right moving, no yaw): broken encoder, not a stall
  TEST_ASSERT_EQUAL(R(StallGuardLite::Result::kFault), R(run(g, drive(130, 130, 0, 969, 0), 600, side)));
  TEST_ASSERT_EQUAL_INT8(0, side);
  TEST_ASSERT_TRUE(g.faulted(0));
  TEST_ASSERT_EQUAL(R(StallGuardLite::Result::kNone), R(run(g, drive(130, 130, 0, 969, 0), 600, side)));  // once
}

void test_stall_below_keep_is_not_driven() {
  StallGuardLite g;
  int8_t side = -1;
  TEST_ASSERT_EQUAL(R(StallGuardLite::Result::kNone), R(run(g, drive(100, 100, 0, 0, 0), 2000, side)));
}

void test_stall_disabled_does_nothing() {
  StallGuardLite g;
  int8_t side = -1;
  StallGuardLite::Inputs in = drive(130, 130, 0, 0, 0);
  in.enabled = false;
  TEST_ASSERT_EQUAL(R(StallGuardLite::Result::kNone), R(run(g, in, 2000, side)));
}

void test_stall_both_held_is_stall() {
  StallGuardLite g;
  int8_t side = -1;
  TEST_ASSERT_EQUAL(R(StallGuardLite::Result::kStall), R(run(g, drive(130, 130, 0, 0, 0), 600, side)));
}

// Floor test 2026-10-17: the left track did not move while the heading hold flipped its PWM (+130 / 100 / -110);
// each dip below "driven" restarted the timer, so it never tripped. Driven-and-slow time now adds up; it pauses while
// not driven and resets only when the side moves properly or idles at 0.
void test_stall_flapping_pwm_still_trips() {
  StallGuardLite g;
  int8_t side = -1;
  const int16_t pattern[] = {130, 130, 130, 100, 100, -110, -110, 100, 130, 130, 130};  // 220 ms of driven per loop
  StallGuardLite::Result r = StallGuardLite::Result::kNone;
  uint32_t t = 0;
  for (int loop = 0; loop < 10 && r == StallGuardLite::Result::kNone; ++loop) {
    for (int16_t p : pattern) {
      r = g.update(StallGuardLite::Inputs{p, 160, 90, 90, 0, 800, -30, true}, t, side);  // left never moves
      t += 20;
      if (r != StallGuardLite::Result::kNone) break;
    }
  }
  TEST_ASSERT_EQUAL(R(StallGuardLite::Result::kStall), R(r));
  TEST_ASSERT_EQUAL_INT8(0, side);
}

void test_stall_idle_resets_the_timer() {
  StallGuardLite g;
  int8_t side = -1;
  uint32_t t = 0;
  for (; t < 300; t += 20) TEST_ASSERT_EQUAL(R(StallGuardLite::Result::kNone), R(g.update(drive(130, 130, 0, 0), t, side)));
  for (; t < 600; t += 20) TEST_ASSERT_EQUAL(R(StallGuardLite::Result::kNone), R(g.update(drive(0, 0, 0, 0), t, side)));
  for (; t < 900; t += 20) TEST_ASSERT_EQUAL(R(StallGuardLite::Result::kNone), R(g.update(drive(130, 130, 0, 0), t, side)));
  // 300 ms + idle + 300 ms: not yet (the idle reset it); 200 ms more trips
  StallGuardLite::Result r = StallGuardLite::Result::kNone;
  for (; t < 1200 && r == StallGuardLite::Result::kNone; t += 20) r = g.update(drive(130, 130, 0, 0), t, side);
  TEST_ASSERT_EQUAL(R(StallGuardLite::Result::kStall), R(r));
}

// ---------- RunawayGuardLite / HeadingHoldLite::errorCdeg ----------
void test_runaway_reversed_motor_trips() {
  RunawayGuardLite g;
  bool tripped = false;
  int32_t err = 3100;
  for (int i = 0; i < 60 && !tripped; ++i) {
    tripped = g.step(true, err, 100);  // saturated correction, error growing 1° per tick
    err += 100;
  }
  TEST_ASSERT_TRUE(tripped);
}

void test_runaway_normal_correction_never_trips() {
  RunawayGuardLite g;
  int32_t err = 4000;  // big error after a bump, shrinking under a saturated correction
  for (int i = 0; i < 200; ++i) {
    TEST_ASSERT_FALSE(g.step(true, err, err > 1000 ? 100 : 40));
    if (err > 0) err -= 30;
  }
}

void test_runaway_needs_saturation_and_holding() {
  RunawayGuardLite g;
  for (int i = 0; i < 100; ++i) TEST_ASSERT_FALSE(g.step(true, 3500 + i * 50, 80));   // not saturated
  for (int i = 0; i < 100; ++i) TEST_ASSERT_FALSE(g.step(false, 3500 + i * 50, 100));  // not holding
}

void test_heading_hold_exposes_error() {
  HeadingHoldLite h;
  int8_t ws = 0, wc = 0;
  TEST_ASSERT_EQUAL_INT32(0, h.errorCdeg());
  h.teleop(40, 0, true, 1000, 0, ws, wc);   // captures 10.00° (still)
  h.teleop(40, 0, true, 1250, 0, ws, wc);   // drifted to 12.50°: error -2.50°
  TEST_ASSERT_TRUE(h.holding());
  TEST_ASSERT_EQUAL_INT32(-250, h.errorCdeg());
  h.teleop(0, 0, true, 1250, 0, ws, wc);    // stopped: reset
  TEST_ASSERT_EQUAL_INT32(0, h.errorCdeg());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_latch_trips_and_masks_both_tracks);
  RUN_TEST(test_latch_stays_while_command_held);
  RUN_TEST(test_latch_not_released_by_firmware_zeroing);
  RUN_TEST(test_latch_clear_by_stop_and_first_trip_wins);
  RUN_TEST(test_sync_equal_counts_no_change);
  RUN_TEST(test_sync_slows_faster_left);
  RUN_TEST(test_sync_reverse_slows_faster_side);
  RUN_TEST(test_sync_clamped);
  RUN_TEST(test_sync_inactive_when_disabled);
  RUN_TEST(test_sync_rebases_after_inactive);
  RUN_TEST(test_stall_held_track_trips_in_half_a_second);
  RUN_TEST(test_stall_slow_but_moving_is_not_stall);
  RUN_TEST(test_stall_slipping_track_at_25_pct_trips);
  RUN_TEST(test_stall_dead_encoder_while_straight_is_fault);
  RUN_TEST(test_stall_below_keep_is_not_driven);
  RUN_TEST(test_stall_disabled_does_nothing);
  RUN_TEST(test_stall_both_held_is_stall);
  RUN_TEST(test_stall_flapping_pwm_still_trips);
  RUN_TEST(test_stall_idle_resets_the_timer);
  RUN_TEST(test_runaway_reversed_motor_trips);
  RUN_TEST(test_runaway_normal_correction_never_trips);
  RUN_TEST(test_runaway_needs_saturation_and_holding);
  RUN_TEST(test_heading_hold_exposes_error);
  return UNITY_END();
}

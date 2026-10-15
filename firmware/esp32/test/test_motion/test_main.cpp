// Plan H (bridge LLD R4): motion router, turn target, turn controller, rate loop, motion service.
#include <math.h>
#include <unity.h>

#include "core/motion_config.h"
#include "core/motion_router.h"
#include "core/motion_service.h"
#include "core/rate_loop.h"
#include "support/tank_model.h"
#include "core/turn_controller.h"
#include "cq_protocol.h"

using namespace cq::core;
namespace proto = cq::proto;

namespace {
uint8_t bit(proto::CapBit b) { return static_cast<uint8_t>(1u << static_cast<uint8_t>(b)); }
const uint8_t kLiteCaps = bit(proto::CapBit::kAudioLed);
int R(Route r) { return static_cast<int>(r); }
}  // namespace

void setUp() {}
void tearDown() {}

// ---------- router ----------
void test_router_lite_runs_turn_and_rate_locally() {
  TEST_ASSERT_EQUAL(R(Route::kLocal), R(routeTurn(kLiteCaps, 0)));
  TEST_ASSERT_EQUAL(R(Route::kLocal), R(routeDriveRate(kLiteCaps)));
  TEST_ASSERT_EQUAL(R(Route::kUnavailable), R(routeTurn(kLiteCaps, 30)));  // arc radius needs encoders
  TEST_ASSERT_EQUAL(R(Route::kUnavailable), R(routeMove(kLiteCaps)));
}

void test_router_full_body_forwards() {
  const uint8_t full = bit(proto::CapBit::kTurn) | bit(proto::CapBit::kDriveRate) | bit(proto::CapBit::kMove);
  TEST_ASSERT_EQUAL(R(Route::kForward), R(routeTurn(full, 0)));
  TEST_ASSERT_EQUAL(R(Route::kForward), R(routeTurn(full, 30)));
  TEST_ASSERT_EQUAL(R(Route::kForward), R(routeDriveRate(full)));
  TEST_ASSERT_EQUAL(R(Route::kForward), R(routeMove(full)));
}

// ---------- turn target (0.01°, + = CCW / left) ----------
void test_turn_target_wrap() {
  TEST_ASSERT_EQUAL_INT32(2000, turnDelta(35000, true, 0, 1000));    // 350° -> to 10°: +20° (short way)
  TEST_ASSERT_EQUAL_INT32(-2000, turnDelta(1000, true, 0, 35000));   // 10° -> to 350°: -20°
  TEST_ASSERT_EQUAL_INT32(18000, turnDelta(0, true, 0, 18000));      // exactly opposite: +180°
  TEST_ASSERT_EQUAL_INT32(36000, turnDelta(12345, false, 36000, 0)); // by 360 keeps its size
  TEST_ASSERT_EQUAL_INT32(-72000, turnDelta(12345, false, -72000, 0));
}

// ---------- turn controller (open-loop facts) ----------
void test_turn_far_error_runs_at_speed_toward_target() {
  TurnController tc;
  tc.begin(0, 9000, 40, 0);  // by +90° (left) at 40 %
  TurnStep s = tc.step(0, 0, 20);
  TEST_ASSERT_EQUAL_INT8(40, s.w);
  tc.begin(0, -9000, 40, 0);
  s = tc.step(0, 0, 20);
  TEST_ASSERT_EQUAL_INT8(-40, s.w);
  TEST_ASSERT_FALSE(s.done);
}

void test_turn_near_error_keeps_minimum() {
  TurnController tc;
  tc.begin(0, 9000, 40, 0);
  const TurnStep s = tc.step(8500, 0, 20);  // 5° left to go, still: 5 % < w_min -> w_min
  TEST_ASSERT_EQUAL_INT8(motion::kTurnWMin, s.w);
}

void test_turn_latency_compensation_brakes_early() {
  TurnController tc;
  tc.begin(0, 9000, 40, 0);
  // 8° to go but turning at 120 °/s: 80 ms ahead it would be at -1.6° -> command reverses at w_min
  const TurnStep s = tc.step(8200, 1200, 20);
  TEST_ASSERT_EQUAL_INT8(-motion::kTurnWMin, s.w);
}

void test_turn_done_after_200ms_in_window() {
  TurnController tc;
  tc.begin(0, 9000, 40, 0);
  uint32_t t = 20;
  TurnStep s = tc.step(8950, 10, t);  // 0.5° off, 1 °/s: in the window
  TEST_ASSERT_EQUAL_INT8(0, s.w);
  TEST_ASSERT_FALSE(s.done);
  for (int i = 0; i < 9; ++i) s = tc.step(8960, 0, t += 20);
  TEST_ASSERT_FALSE(s.done);  // 180 ms
  s = tc.step(8960, 0, t += 20);
  TEST_ASSERT_TRUE(s.done);   // 200 ms
}

void test_turn_window_hysteresis() {
  TurnController tc;
  tc.begin(0, 9000, 40, 0);
  TurnStep s = tc.step(8900, 0, 20);  // 1° off: in
  TEST_ASSERT_EQUAL_INT8(0, s.w);
  s = tc.step(8800, 0, 40);  // 2° off: still settling (exit band 2.5°)
  TEST_ASSERT_EQUAL_INT8(0, s.w);
  s = tc.step(8700, 0, 60);  // 3° off: leaves the window, drives again
  TEST_ASSERT_EQUAL_INT8(motion::kTurnWMin, s.w);
}

void test_turn_stalled_and_timeout() {
  TurnController tc;
  tc.begin(0, 9000, 40, 0);
  TurnStep s{};
  for (uint32_t t = 20; t <= 1500; t += 20) s = tc.step(0, 0, t);
  TEST_ASSERT_FALSE(s.stalled);
  s = tc.step(0, 0, 1520);
  TEST_ASSERT_TRUE(s.stalled);
  tc.begin(0, 9000, 40, 0);  // timeout = 90 / 20 + 3 = 7.5 s; keep it "moving" so it is not stalled
  for (uint32_t t = 20; t < 7500; t += 20) s = tc.step(static_cast<uint16_t>((t / 20) % 2 ? 100 : 0), 300, t);
  TEST_ASSERT_FALSE(s.timeout);
  s = tc.step(0, 300, 7520);
  TEST_ASSERT_TRUE(s.timeout);
}

void test_turn_tracks_multi_turn() {
  TurnController tc;
  tc.begin(35000, 36000, 40, 0);  // by 360 from 350°
  // pass through 0° twice-worth of wrap: 350 -> 170 (crossing 0) -> 349
  tc.step(100, 900, 20);    // +11° (wrapped)
  tc.step(17000, 900, 40);  // +169°
  const TurnStep s = tc.step(34900, 900, 60);  // +359° total, 1° to go
  TEST_ASSERT_NOT_EQUAL(0, s.w);  // still turning (latency term), not done yet
  TEST_ASSERT_TRUE(tc.progressPct() >= 99);
}

// ---------- rate loop (0.1 °/s in, % out) ----------
void test_rate_feed_forward_at_zero_error() {
  RateLoop rl;
  rl.reset();
  const int8_t w = rl.step(900, 900, 20);  // 90 °/s wanted and measured: ff only, 0.45 x 90 = 40.5
  TEST_ASSERT_INT_WITHIN(1, 40, w);
  TEST_ASSERT_INT_WITHIN(1, -40, rl.step(-900, -900, 20));
}

void test_rate_integral_grows_on_shortfall() {
  RateLoop rl;
  rl.reset();
  const int8_t first = rl.step(600, 300, 20);
  int8_t later = first;
  for (int i = 0; i < 25; ++i) later = rl.step(600, 300, 20);  // 0.5 s short by 30 °/s
  TEST_ASSERT_TRUE(later > first);
}

void test_rate_anti_windup_and_clamp() {
  RateLoop rl;
  rl.reset();
  int8_t w = 0;
  for (int i = 0; i < 100; ++i) w = rl.step(1800, 0, 20);  // 180 °/s wanted, tank stuck: saturated 2 s
  TEST_ASSERT_EQUAL_INT8(100, w);
  w = rl.step(1800, 1800, 20);  // suddenly on target: no stored wind-up, back near feed-forward (81)
  TEST_ASSERT_TRUE(w <= 86);
}

void test_rate_sign_change_resets_integral() {
  RateLoop rl;
  rl.reset();
  for (int i = 0; i < 50; ++i) rl.step(600, 0, 20);  // build an integral to the left
  const int8_t w = rl.step(-600, -600, 20);         // target flips right, on target: ff only
  TEST_ASSERT_INT_WITHIN(1, -27, w);
}

// ---------- motion service: closed loop against the tank model ----------
namespace {
struct TurnRun {
  PrimitiveEnd end{};
  bool ended = false;
  double error_deg = 0;    // turned - requested, after 1 s of settling
  int8_t last_w = 99;      // the final command the service sent
  uint32_t ms = 0;
};

TurnRun runTurn(double start_yaw, bool has_to, int32_t by_cdeg, uint16_t to_cdeg, uint8_t speed = 40) {
  cqtest::TankModel tank(start_yaw);
  MotionService svc;
  const uint16_t yaw0 = tank.reportedYawCdeg();
  const int32_t delta = turnDelta(yaw0, has_to, by_cdeg, to_cdeg);
  svc.startTurn(7, yaw0, delta, speed, 42, 0);
  TurnRun r;
  int8_t w = 0;
  for (uint32_t t = 20; t <= 30000 && !r.ended; t += 20) {
    tank.step(w);
    const MotionOut o = svc.onTelemetry(tank.reportedYawCdeg(), tank.reportedRateDdps(), t);
    if (o.send) {
      w = o.w;
      r.last_w = o.w;
    }
    if (o.ended) {
      r.ended = true;
      r.end = o.end;
      r.ms = t;
    }
  }
  for (int i = 0; i < 50; ++i) tank.step(0);  // 1 s with w = 0 (the Uno's heading hold takes over)
  r.error_deg = tank.turnedDeg() - delta / 100.0;
  return r;
}
}  // namespace

void test_service_turn_closed_loop() {
  const struct { double start; bool to; int32_t by; uint16_t to_c; } cases[] = {
      {0, false, 9000, 0}, {0, false, -9000, 0}, {350, true, 0, 1000}, {10, true, 0, 35000}, {123, false, 36000, 0}};
  for (const auto& c : cases) {
    const TurnRun r = runTurn(c.start, c.to, c.by, c.to_c);
    TEST_ASSERT_TRUE(r.ended);
    TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDone), static_cast<int>(r.end.reason));
    TEST_ASSERT_TRUE(r.end.ok);
    TEST_ASSERT_EQUAL_INT32(42, r.end.id);
    TEST_ASSERT_EQUAL_INT8(0, r.last_w);
    TEST_ASSERT_TRUE_MESSAGE(fabs(r.error_deg) <= 2.0, "turn error above 2 degrees");
  }
}

void test_turn_every_end_sends_zero_then_stop() {
  const EndReason reasons[] = {EndReason::kCancelled, EndReason::kStop, EndReason::kNotOwner};
  for (EndReason why : reasons) {
    MotionService svc;
    svc.startTurn(3, 0, 9000, 40, 1, 0);
    svc.onTelemetry(0, 0, 20);
    const MotionOut o = svc.cancel(why);
    TEST_ASSERT_TRUE(o.ended);
    TEST_ASSERT_TRUE(o.send);
    TEST_ASSERT_EQUAL_INT8(0, o.w);
    TEST_ASSERT_EQUAL_INT8(0, o.v);
    TEST_ASSERT_FALSE(o.end.ok);
    TEST_ASSERT_EQUAL(static_cast<int>(why), static_cast<int>(o.end.reason));
    TEST_ASSERT_FALSE(svc.active());
    TEST_ASSERT_FALSE(svc.cancel(why).ended);  // nothing left to end
  }
}

void test_service_owner_loss_ends() {
  MotionService svc;
  svc.driveRate(5, 30, 300, 0);
  TEST_ASSERT_EQUAL_INT(5, svc.ownerFd());
  const MotionOut o = svc.cancel(EndReason::kNotOwner);
  TEST_ASSERT_EQUAL(static_cast<int>(PrimKind::kRate), static_cast<int>(o.end.kind));
  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kNotOwner), static_cast<int>(o.end.reason));
}

void test_service_telemetry_gap_ends_link() {
  MotionService svc;
  svc.startTurn(3, 0, 9000, 40, 1, 0);
  svc.onTelemetry(0, 0, 20);
  TEST_ASSERT_FALSE(svc.tick(200).ended);  // 180 ms since the last frame
  const MotionOut o = svc.tick(221);
  TEST_ASSERT_TRUE(o.ended);
  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kLink), static_cast<int>(o.end.reason));
  TEST_ASSERT_EQUAL_INT8(0, o.w);
}

void test_service_stuck_tank_ends_stalled() {
  MotionService svc;
  svc.startTurn(3, 0, 9000, 40, 1, 0);
  MotionOut o;
  uint32_t t = 0;
  while (!o.ended && t < 10000) o = svc.onTelemetry(0, 0, t += 20);  // never moves
  TEST_ASSERT_TRUE(o.ended);
  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kStalled), static_cast<int>(o.end.reason));
  TEST_ASSERT_EQUAL_INT8(0, o.w);
}

void test_rate_deadman() {
  MotionService svc;
  svc.driveRate(5, 20, 450, 0);
  TEST_ASSERT_TRUE(svc.onTelemetry(0, 0, 20).send);
  TEST_ASSERT_FALSE(svc.onTelemetry(0, 0, 300).ended);  // 300 ms since the last drive_rate: still ok
  const MotionOut o = svc.onTelemetry(0, 0, 320);
  TEST_ASSERT_TRUE(o.ended);
  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDeadman), static_cast<int>(o.end.reason));
  TEST_ASSERT_EQUAL_INT8(0, o.w);
  TEST_ASSERT_EQUAL_INT8(0, o.v);
}

void test_rate_converges_in_model() {
  cqtest::TankModel tank(0);
  MotionService svc;
  int8_t w = 0;
  double sum = 0;
  int n = 0;
  for (uint32_t t = 20; t <= 3000; t += 20) {
    if (t % 100 == 20) svc.driveRate(5, 0, 600, t);  // the phone streams 60 °/s at 10 Hz
    tank.step(w);
    const MotionOut o = svc.onTelemetry(tank.reportedYawCdeg(), tank.reportedRateDdps(), t);
    if (o.send) w = o.w;
    if (t >= 1000) {
      sum += tank.rateDps();
      ++n;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(fabs(sum / n - 60.0) <= 6.0, "drive_rate mean not within 10 % of 60 deg/s after 1 s");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_router_lite_runs_turn_and_rate_locally);
  RUN_TEST(test_router_full_body_forwards);
  RUN_TEST(test_turn_target_wrap);
  RUN_TEST(test_turn_far_error_runs_at_speed_toward_target);
  RUN_TEST(test_turn_near_error_keeps_minimum);
  RUN_TEST(test_turn_latency_compensation_brakes_early);
  RUN_TEST(test_turn_done_after_200ms_in_window);
  RUN_TEST(test_turn_window_hysteresis);
  RUN_TEST(test_turn_stalled_and_timeout);
  RUN_TEST(test_turn_tracks_multi_turn);
  RUN_TEST(test_rate_feed_forward_at_zero_error);
  RUN_TEST(test_rate_integral_grows_on_shortfall);
  RUN_TEST(test_rate_anti_windup_and_clamp);
  RUN_TEST(test_rate_sign_change_resets_integral);
  RUN_TEST(test_service_turn_closed_loop);
  RUN_TEST(test_turn_every_end_sends_zero_then_stop);
  RUN_TEST(test_service_owner_loss_ends);
  RUN_TEST(test_service_telemetry_gap_ends_link);
  RUN_TEST(test_service_stuck_tank_ends_stalled);
  RUN_TEST(test_rate_deadman);
  RUN_TEST(test_rate_converges_in_model);
  return UNITY_END();
}

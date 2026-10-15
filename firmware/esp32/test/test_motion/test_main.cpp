// Plan H (bridge LLD R4): motion router, turn target, turn controller, rate loop, motion service.
#include <unity.h>

#include "core/motion_config.h"
#include "core/motion_router.h"
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
  return UNITY_END();
}

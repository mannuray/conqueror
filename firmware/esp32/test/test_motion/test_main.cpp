// Plan H (bridge LLD R4, R18): motion router, turn target, turn controller, rate loop, motion service.
#include <math.h>
#include <unity.h>

#include "core/link_owner.h"
#include "core/motion_config.h"
#include "core/motion_link.h"
#include "core/motion_router.h"
#include "core/motion_service.h"
#include "core/move_controller.h"
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

// Plan K (bridge LLD R7.2): an Uno with encoders gets MOVE from the bridge's motion service.
void test_router_move_needs_encoders() {
  const uint8_t enc = kLiteCaps | bit(proto::CapBit::kEncoders);
  TEST_ASSERT_EQUAL(R(Route::kLocal), R(routeMove(enc)));
  TEST_ASSERT_EQUAL(R(Route::kUnavailable), R(routeMove(kLiteCaps)));  // encoders off -> no MOVE
  TEST_ASSERT_EQUAL(R(Route::kForward), R(routeMove(enc | bit(proto::CapBit::kMove))));
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

// ---------- R18: slow zone, cut at the predicted stop, one nudge ----------
void test_turn_slow_zone_holds_the_slowest_smooth_spin() {
  TurnController tc;
  tc.begin(0, 9000, 40, 0);
  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kApproach), static_cast<int>(tc.phase()));
  const TurnStep s = tc.step(7000, motion::kSpinMinDdps, 20);  // 20° to go at 65 °/s: rate control, no cut yet
  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kSlow), static_cast<int>(tc.phase()));
  TEST_ASSERT_EQUAL_INT8(29, s.w);  // feed-forward 0.45 x 65 °/s, no error
}

void test_turn_short_turn_starts_slow() {
  TurnController tc;
  tc.begin(0, -1500, 40, 0);  // 15° right
  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kSlow), static_cast<int>(tc.phase()));
  const TurnStep s = tc.step(0, 0, 20);
  TEST_ASSERT_TRUE(s.w < 0);  // toward the target under rate control
}

void test_turn_cuts_when_the_predicted_stop_reaches_the_target() {
  TurnController tc;
  tc.begin(0, 9000, 40, 0, 60);
  TurnStep s = tc.step(8500, 650, 20);  // 5° to go, 65 °/s x 60 ms = 3.9°: not yet
  TEST_ASSERT_TRUE(s.w > 0);
  s = tc.step(8700, 650, 40);  // 3° to go < 3.9°: cut
  TEST_ASSERT_EQUAL_INT8(0, s.w);
  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kCoast), static_cast<int>(tc.phase()));
  s = tc.step(8900, 300, 60);  // still gliding: w stays 0, not done
  TEST_ASSERT_EQUAL_INT8(0, s.w);
  TEST_ASSERT_FALSE(s.done);
}

namespace {
// After a cut at 8700 (65 °/s), glide to `stop` and hold there: returns the step that ended the hold.
TurnStep glideTo(TurnController& tc, uint16_t stop, uint32_t& t) {
  TurnStep s{};
  for (int i = 0; i < 40 && !s.done && tc.phase() != TurnPhase::kNudge; ++i) s = tc.step(stop, 0, t += 20);
  return s;
}
}  // namespace

void test_turn_done_once_stopped_with_the_coast_reading() {
  TurnController tc;
  tc.begin(0, 9000, 40, 0, 60);
  uint32_t t = 0;
  tc.step(8700, 650, t += 20);  // cut at 87° turning 65 °/s
  const uint32_t cut_at = t;
  const TurnStep s = glideTo(tc, 8990, t);  // glides 2.9°, stops 0.1° short
  TEST_ASSERT_TRUE(s.done);
  TEST_ASSERT_TRUE(t >= cut_at + motion::kWindowHoldMs);  // done only after the stillness hold
  TEST_ASSERT_EQUAL_UINT16(44, s.coast_ms);  // 290 cdeg x 100 / 650 ddps
}

void test_turn_one_nudge_when_it_stops_short() {
  TurnController tc;
  tc.begin(0, 9000, 40, 0, 60);
  uint32_t t = 0;
  tc.step(8000, 650, t += 20);
  tc.step(8700, 650, t += 20);  // cut
  TurnStep s = glideTo(tc, 8500, t);  // went back?? no: stopped 5° short (the heading did not reach the cut point)
  TEST_ASSERT_FALSE(s.done);
  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kNudge), static_cast<int>(tc.phase()));
  s = tc.step(8500, 0, t += 20);
  TEST_ASSERT_TRUE(s.w > 0);  // the nudge: toward the target
  s = tc.step(8700, 650, t += 20);  // the nudge's own cut
  TEST_ASSERT_EQUAL_INT8(0, s.w);
  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kNudgeCoast), static_cast<int>(tc.phase()));
  s = glideTo(tc, 8650, t);  // still 3.5° short: done anyway - never a second nudge
  TEST_ASSERT_TRUE(s.done);
  TEST_ASSERT_EQUAL_UINT16(0, s.coast_ms);  // learned from the first coast only
}

void test_turn_nudge_back_after_an_overshoot() {
  TurnController tc;
  tc.begin(0, 9000, 40, 0, 60);
  uint32_t t = 0;
  tc.step(8700, 650, t += 20);  // cut
  glideTo(tc, 9500, t);         // glided 5° past
  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kNudge), static_cast<int>(tc.phase()));
  TurnStep s = tc.step(9500, 0, t += 20);
  TEST_ASSERT_TRUE(s.w < 0);  // back toward the target
  s = tc.step(9400, -650, t += 20);  // 4° to go back at 65 °/s: no cut yet (3.9°)
  TEST_ASSERT_TRUE(s.w < 0);
  s = tc.step(9300, -650, t += 20);  // 3° to go: cut
  TEST_ASSERT_EQUAL_INT8(0, s.w);
  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kNudgeCoast), static_cast<int>(tc.phase()));
}

// A turn within 3° of its target and not turning is already there: the slowest spin would glide past further than
// that, so it ends without a push (also a "to" heading it already faces, and a turn of 0-3°).
void test_turn_within_three_degrees_at_rest_does_not_push() {
  const int32_t deltas[] = {0, 250, -300, 9000};
  for (int32_t d : deltas) {
    TurnController tc;
    tc.begin(0, d, 40, 0);
    uint32_t t = 0;
    const uint16_t at = static_cast<uint16_t>(d == 9000 ? 8990 : 0);  // 9000: arrives 0.1° short, still
    TurnStep s{};
    for (int i = 0; i < 30 && !s.done; ++i) {
      s = tc.step(at, 0, t += 20);
      TEST_ASSERT_EQUAL_INT8(0, s.w);
    }
    TEST_ASSERT_TRUE(s.done);
    TEST_ASSERT_FALSE(s.stalled);
  }
  TurnController tc;  // 4° at rest: it does push (then glides)
  tc.begin(0, 400, 40, 0);
  TEST_ASSERT_TRUE(tc.step(0, 0, 20).w > 0);
}

void test_turn_coast_reading_ignores_bad_cuts() {
  {  // cut while turning slower than 30 °/s: no reading
    TurnController tc;
    tc.begin(0, 9000, 40, 0, 60);
    uint32_t t = 0;
    tc.step(8950, 200, t += 20);
    TEST_ASSERT_EQUAL_UINT16(0, glideTo(tc, 9000, t).coast_ms);
  }
  {  // coasted 9° from 65 °/s = 138 ms: kept; 30° = 461 ms: above 400 ms, dropped
    TurnController tc;
    tc.begin(0, 9000, 40, 0, 60);
    uint32_t t = 0;
    tc.step(8700, 650, t += 20);
    TEST_ASSERT_EQUAL_UINT16(0, glideTo(tc, 11700, t).coast_ms);  // also 27° past: nudge, reading dropped
  }
}

void test_coast_average_moves_a_quarter_and_stays_in_range() {
  TEST_ASSERT_EQUAL_UINT16(70, coastAverage(60, 100));  // (180 + 100 + 2) / 4
  TEST_ASSERT_EQUAL_UINT16(50, coastAverage(60, 20));   // (180 + 20 + 2) / 4
  TEST_ASSERT_EQUAL_UINT16(motion::kCoastMinMs, coastAverage(20, 20));
  TEST_ASSERT_EQUAL_UINT16(motion::kCoastMaxMs, coastAverage(400, 400));
}

// Floor test 2026-10-16 (marble): on target the tank shook left-right (gyro +-55 °/s, heading +-1°). Coasting, the
// command is 0 whatever the rate, and "stopped" looks at the heading, not the gyro: the turn ends after the hold.
void test_turn_shaking_at_target_finishes_without_corrections() {
  TurnController tc;
  tc.begin(0, 9000, 40, 0);
  uint32_t t = 0;
  tc.step(8700, 650, t += 20);  // cut
  TurnStep s{};
  bool done = false;
  for (int i = 0; i < 40 && !done; ++i) {
    const bool up = i % 2 == 0;
    s = tc.step(up ? 8900 : 9050, up ? 550 : -550, t += 20);  // -1° / +0.5°, jerking +-55 °/s
    TEST_ASSERT_EQUAL_INT8(0, s.w);
    done = s.done;
  }
  TEST_ASSERT_TRUE(done);
  // the first hold restarts (the glide from the cut moved it 2° or more), the second one ends the turn
  TEST_ASSERT_TRUE(t <= 20 + 2 * motion::kWindowHoldMs + 40);
}

// A real slow drift (8 °/s = 2.4° per 300 ms) is not "stopped": no end, no nudge while it moves.
void test_turn_slow_drift_is_not_done() {
  TurnController tc;
  tc.begin(0, 9000, 40, 0);
  uint32_t t = 0;
  tc.step(8700, 650, t += 20);  // cut
  int32_t yaw = 8860;
  for (int i = 0; i < 40; ++i) {
    const TurnStep s = tc.step(static_cast<uint16_t>(yaw), 80, t += 20);
    TEST_ASSERT_FALSE(s.done);
    TEST_ASSERT_EQUAL_INT8(0, s.w);
    yaw += 16;
  }
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

void test_turn_stalled_in_the_slow_zone() {
  TurnController tc;
  tc.begin(0, 1500, 40, 0);  // Slow from the start; the tank never moves
  TurnStep s{};
  for (uint32_t t = 20; t <= 1700 && !s.stalled; t += 20) s = tc.step(0, 0, t);
  TEST_ASSERT_TRUE(s.stalled);
}

void test_turn_tracks_multi_turn() {
  TurnController tc;
  tc.begin(35000, 36000, 40, 0);  // by 360 from 350°
  // pass through 0° twice-worth of wrap: 350 -> 170 (crossing 0) -> 349
  tc.step(100, 900, 20);    // +11° (wrapped)
  tc.step(17000, 900, 40);  // +169°
  const TurnStep s = tc.step(34900, 900, 60);  // +359° total, 1° to go at 90 °/s: cut
  TEST_ASSERT_EQUAL_INT8(0, s.w);
  TEST_ASSERT_EQUAL(static_cast<int>(TurnPhase::kCoast), static_cast<int>(tc.phase()));
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
  int pushes = 0;          // R18: times the command went from 0 to turning (1 = no nudge, 2 = one nudge)
  int flips = 0;           // R18: times the tank's real turn changed direction (a wobble)
  double slow_rate = 0;    // R18: |rate| (°/s) 10° before the target
};

struct TurnOpts {
  bool realistic = false;  // R18: the floor model (deadband lift + glide)
  uint16_t coast_ms = motion::kCoastDefaultMs;
  double decel = 1000.0;
  bool pivot = false;
};

TurnRun runTurn(double start_yaw, bool has_to, int32_t by_cdeg, uint16_t to_cdeg, uint8_t speed = 40,
               uint32_t seed = 12345, const TurnOpts& opt = TurnOpts{}) {
  cqtest::TankModel tank(start_yaw, seed);
  if (opt.realistic) tank.setRealistic(opt.decel);
  MotionService svc;
  const uint16_t yaw0 = tank.reportedYawCdeg();
  const int32_t delta = turnDelta(yaw0, has_to, by_cdeg, to_cdeg);
  if (opt.pivot) {
    svc.startPivot(7, yaw0, delta, speed, 42, 0, opt.coast_ms);
  } else {
    svc.startTurn(7, yaw0, delta, speed, 42, 0, opt.coast_ms);
  }
  TurnRun r;
  int8_t w = 0;
  for (uint32_t t = 20; t <= 30000 && !r.ended; t += 20) {
    tank.step(w);
    if (r.slow_rate == 0 && fabs(delta / 100.0 - tank.turnedDeg()) <= 10.0) r.slow_rate = fabs(tank.rateDps());
    const MotionOut o = svc.onTelemetry(tank.reportedYawCdeg(), tank.reportedRateDdps(), t);
    if (o.send) {
      w = o.w;  // a pivot's w is the spin part; the model turns by it
      r.last_w = o.w;
    }
    if (o.ended) {
      r.ended = true;
      r.end = o.end;
      r.ms = t;
    }
  }
  for (int i = 0; i < 50; ++i) tank.step(0);  // 1 s coasting with w = 0 (the bridge then sends STOP)
  r.error_deg = tank.turnedDeg() - delta / 100.0;
  r.pushes = tank.pushes();
  r.flips = tank.directionChanges();
  return r;
}
}  // namespace

void test_service_turn_closed_loop() {
  const struct { double start; bool to; int32_t by; uint16_t to_c; uint8_t speed; } cases[] = {
      {0, false, 9000, 0, 40},  {0, false, -9000, 0, 40}, {350, true, 0, 1000, 40}, {10, true, 0, 35000, 40},
      {123, false, 36000, 0, 40}, {0, false, 1000, 0, 40}, {0, false, 18000, 0, 40}, {0, false, -72000, 0, 40},
      {0, false, 9000, 0, 12},  {0, false, 9000, 0, 100}};
  const uint32_t seeds[] = {1u, 7u, 99u};
  TurnOpts o;
  o.coast_ms = 170;  // R18: this smooth model glides on its 150 ms lag (~170 ms coast, as the learning would find)
  for (const auto& c : cases) for (uint32_t seed : seeds) {
    const TurnRun r = runTurn(c.start, c.to, c.by, c.to_c, c.speed, seed, o);
    TEST_ASSERT_TRUE(r.ended);
    TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDone), static_cast<int>(r.end.reason));
    TEST_ASSERT_TRUE(r.end.ok);
    TEST_ASSERT_EQUAL_INT32(42, r.end.id);
    TEST_ASSERT_EQUAL_INT8(0, r.last_w);
    TEST_ASSERT_TRUE_MESSAGE(fabs(r.error_deg) <= 3.0, "turn error above 3 degrees");
    TEST_ASSERT_TRUE_MESSAGE(r.pushes <= 2, "more than one nudge");
  }
}

// R18 (design-notes §112): on the floor model a turn slows to ~65 °/s, glides onto the target with one cut, needs at
// most one nudge and never wobbles - the old controller made 8-25 alternating pushes here.
void test_service_turn_realistic_one_smooth_stop() {
  const struct { int32_t by; uint8_t speed; } cases[] = {{9000, 40}, {-9000, 40}, {36000, 40}, {1000, 40},
                                                         {18000, 60}, {-4500, 100}, {9000, 12}};
  const uint32_t seeds[] = {1u, 7u, 99u};
  for (const auto& c : cases) for (uint32_t seed : seeds) {
    TurnOpts o;
    o.realistic = true;
    o.coast_ms = 70;  // the model's own coast, as learned (see the learning test)
    const TurnRun r = runTurn(0, false, c.by, 0, c.speed, seed, o);
    TEST_ASSERT_TRUE(r.ended);
    TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDone), static_cast<int>(r.end.reason));
    TEST_ASSERT_EQUAL_INT8(0, r.last_w);
    TEST_ASSERT_TRUE_MESSAGE(fabs(r.error_deg) <= 3.0, "turn error above 3 degrees");
    TEST_ASSERT_TRUE_MESSAGE(r.pushes <= 2, "more than one nudge");
    TEST_ASSERT_TRUE_MESSAGE(r.flips <= 1, "the turn wobbled");
    if (c.by >= 3000 || c.by <= -3000) TEST_ASSERT_TRUE_MESSAGE(r.slow_rate <= 80.0, "not slowed near the target");
  }
}

// A wrong coast (the default 60 ms against a slipperier floor) still ends within one nudge; the readings pull the
// learned value toward the floor's real coast within about ten turns, and then the turns need no nudge.
void test_service_turn_realistic_learns_the_coast() {
  const double decels[] = {1000.0, 500.0};
  for (double decel : decels) {
    uint16_t coast = motion::kCoastDefaultMs;
    TurnRun r;
    for (int i = 0; i < 12; ++i) {
      TurnOpts o;
      o.realistic = true;
      o.coast_ms = coast;
      o.decel = decel;
      r = runTurn(0, false, i % 2 ? -9000 : 9000, 0, 40, 11u + i, o);
      TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDone), static_cast<int>(r.end.reason));
      TEST_ASSERT_TRUE(r.pushes <= 2);
      if (r.end.coast_ms != 0) coast = coastAverage(coast, r.end.coast_ms);
    }
    TEST_ASSERT_TRUE_MESSAGE(fabs(r.error_deg) <= 1.5, "learned turn not within 1.5 degrees");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, r.pushes, "learned turn still needs a nudge");
  }
}

void test_service_pivot_realistic_one_smooth_stop() {
  for (int dir = -1; dir <= 1; dir += 2) {
    TurnOpts o;
    o.realistic = true;
    o.coast_ms = 70;
    o.pivot = true;
    const TurnRun r = runTurn(0, false, dir * 9000, 0, 40, 5u, o);
    TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDone), static_cast<int>(r.end.reason));
    TEST_ASSERT_TRUE(fabs(r.error_deg) <= 3.0);
    TEST_ASSERT_TRUE(r.pushes <= 2);
    TEST_ASSERT_TRUE(r.flips <= 1);
  }
}

void test_service_turn_end_carries_the_coast_reading() {
  TurnOpts o;
  o.realistic = true;
  const TurnRun r = runTurn(0, false, 9000, 0, 40, 3u, o);
  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDone), static_cast<int>(r.end.reason));
  TEST_ASSERT_TRUE(r.end.coast_ms >= motion::kCoastMinMs && r.end.coast_ms <= motion::kCoastMaxMs);
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
    if (t % 100 == 20) svc.driveRate(5, 0, 800, t);  // the phone streams 80 °/s at 10 Hz (above the 65 °/s floor, Plan L)
    tank.step(w);
    const MotionOut o = svc.onTelemetry(tank.reportedYawCdeg(), tank.reportedRateDdps(), t);
    if (o.send) w = o.w;
    if (t >= 1000) {
      sum += tank.rateDps();
      ++n;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(fabs(sum / n - 80.0) <= 8.0, "drive_rate mean not within 10 % of 80 deg/s after 1 s");
}

// Plan H review M2: a zero target clears the integral (no leftover w that makes the Uno's heading hold chatter).
void test_rate_zero_target_clears_integral() {
  RateLoop rl;
  rl.reset();
  for (int i = 0; i < 100; ++i) rl.step(600, 300, 20);  // 2 s short: integral built up
  TEST_ASSERT_EQUAL_INT8(0, rl.step(0, 0, 20));
}

// ---------- Plan H review C1: reversing drive_rate ----------
namespace {
double rateRun(int8_t v, int16_t target_ddps, bool body_flips, EndReason* why, bool* ended) {
  cqtest::TankModel tank(0);
  tank.setCarFlip(body_flips);
  MotionService svc;
  int8_t w = 0, vv = 0;
  double sum = 0;
  int n = 0;
  *ended = false;
  for (uint32_t t = 20; t <= 3000 && !*ended; t += 20) {
    if (t % 100 == 20) svc.driveRate(5, v, target_ddps, t);
    tank.step(w, vv);
    const MotionOut o = svc.onTelemetry(tank.reportedYawCdeg(), tank.reportedRateDdps(), t);
    if (o.send) w = o.w, vv = o.v;
    if (o.ended) *ended = true, *why = o.end.reason, w = o.w;
    if (t >= 1000) sum += tank.rateDps(), ++n;
  }
  return n ? sum / n : 0.0;
}
}  // namespace

void test_rate_reverse_converges() {  // the Uno honours src AUTO: no flip when reversing
  EndReason why = EndReason::kDone;
  bool ended = false;
  const double mean = rateRun(-30, 300, false, &why, &ended);
  TEST_ASSERT_FALSE(ended);
  TEST_ASSERT_TRUE_MESSAGE(fabs(mean - 30.0) <= 3.0, "reversing drive_rate not within 10 % of 30 deg/s");
}

void test_rate_diverging_body_ends() {  // a body that flips anyway: guard ends it instead of a full-power spin
  EndReason why = EndReason::kDone;
  bool ended = false;
  rateRun(-30, 300, true, &why, &ended);
  TEST_ASSERT_TRUE(ended);
  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDiverged), static_cast<int>(why));
}

// Final review I1: a jammed track (or the Uno's safety gate cutting the PWM) lets the integral climb to full power;
// drive_rate must end "stalled" instead of holding 100 % while the phone keeps streaming (design-notes §56).
void test_rate_stalled_body_ends() {
  MotionService svc;
  svc.driveRate(5, 0, 300, 0);  // spin at 30 °/s
  uint32_t t = 0;
  bool ended = false;
  EndReason why = EndReason::kDone;
  uint32_t saturated_at = 0;
  for (t = 20; t <= 15000 && !ended; t += 20) {
    if (t % 100 == 0) svc.driveRate(5, 0, 300, t);  // the phone keeps streaming
    const MotionOut o = svc.onTelemetry(0, 0, t);   // the tank does not turn at all
    if (!saturated_at && (o.w >= motion::kRateSaturatedPct || o.w <= -motion::kRateSaturatedPct)) saturated_at = t;
    ended = o.ended;
    if (o.ended) why = o.end.reason;
  }
  TEST_ASSERT_TRUE(ended);
  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kStalled), static_cast<int>(why));
  TEST_ASSERT_TRUE(saturated_at > 0);
  TEST_ASSERT_TRUE(t - saturated_at >= motion::kStallMs);         // not before the stall time at full power
  TEST_ASSERT_TRUE(t - saturated_at <= motion::kStallMs + 100);
}

// ... but a straight drive_rate (target 0, tank not turning) is never "stalled".
void test_rate_straight_is_not_stalled() {
  MotionService svc;
  svc.driveRate(5, 30, 0, 0);
  for (uint32_t t = 20; t <= 5000; t += 20) {
    if (t % 100 == 0) svc.driveRate(5, 30, 0, t);
    TEST_ASSERT_FALSE(svc.onTelemetry(0, 0, t).ended);
  }
}

// ---------- Plan H review I1 / I2 / M3 / M4: MotionLink (service + owner + frames), with the real LinkOwner ----------
namespace {
constexpr int kFd = 21;
struct Rig {
  LinkOwner owner;
  MotionLink ml{owner};
  Rig() {
    owner.begin();  // R14: the bridge is master from boot
    owner.onWsOpen(kFd);
    owner.onSynced(true);
    Actions a;
    owner.onDrive(kFd, 0, a);  // the turn command took ownership
    ml.service().startTurn(kFd, 0, 9000, 40, 1, 0);
  }
};
}  // namespace

void test_link_running_turn_sends_auto_drive() {
  Rig r;
  const LinkOut o = r.ml.onTelemetry(0, 0, 20);
  TEST_ASSERT_TRUE(o.drive);
  TEST_ASSERT_EQUAL_INT8(40, o.w);
  TEST_ASSERT_FALSE(o.stop);
  TEST_ASSERT_FALSE(o.event);
}

void test_link_release_ends_and_never_retakes() {
  Rig r;
  r.ml.onTelemetry(0, 0, 20);
  r.owner.onRelease(kFd);  // release_control: the owner logic already sends the STOP
  const LinkOut e = r.ml.onEvent(MotionEvent::kRelease, 30);
  TEST_ASSERT_TRUE(e.event);
  TEST_ASSERT_FALSE(e.drive);
  TEST_ASSERT_FALSE(e.stop);
  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kCancelled), static_cast<int>(e.end.reason));
  TEST_ASSERT_FALSE(r.ml.onTelemetry(0, 0, 40).drive);
  TEST_ASSERT_EQUAL(static_cast<int>(LinkState::kWebReady), static_cast<int>(r.owner.state()));
}

void test_link_lost_owner_never_retakes_even_if_not_told() {  // robustness: a path that forgets the event
  Rig r;
  r.ml.onTelemetry(0, 0, 20);
  r.owner.onRelease(kFd);
  const LinkOut o = r.ml.onTelemetry(100, 0, 40);
  TEST_ASSERT_TRUE(o.event);
  TEST_ASSERT_FALSE(o.drive);
  TEST_ASSERT_FALSE(o.stop);
  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kNotOwner), static_cast<int>(o.end.reason));
  TEST_ASSERT_EQUAL(LinkOwner::kNoFd, r.owner.driverFd());
}

void test_link_ir_takeover_ends_without_frames() {
  Rig r;
  r.ml.onTelemetry(0, 0, 20);
  r.owner.onUnoOwner(proto::Owner::kIr);
  const LinkOut o = r.ml.onTelemetry(100, 0, 40);
  TEST_ASSERT_TRUE(o.event);
  TEST_ASSERT_FALSE(o.drive);
  TEST_ASSERT_FALSE(o.stop);  // a bridge STOP would also stop the IR driver
  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kNotOwner), static_cast<int>(o.end.reason));
}

void test_link_after_release_ends_without_frames() {
  Rig r;
  r.owner.onRelease(kFd);  // release already sent the STOP
  const LinkOut o = r.ml.onTelemetry(0, 0, 20);
  TEST_ASSERT_TRUE(o.event);
  TEST_ASSERT_FALSE(o.drive);
  TEST_ASSERT_FALSE(o.stop);
}

void test_link_events_frames_policy() {
  const struct { MotionEvent e; bool stop; EndReason why; } cases[] = {
      {MotionEvent::kStop, false, EndReason::kStop},          {MotionEvent::kOwnerClosed, false, EndReason::kCancelled},
      {MotionEvent::kOta, false, EndReason::kCancelled},      {MotionEvent::kBodyOffline, false, EndReason::kLink},
      {MotionEvent::kOwnerCommand, false, EndReason::kCancelled}, {MotionEvent::kCancel, true, EndReason::kCancelled},
      // Final review minor 12: the TX queue was full while the body is still synced - end "link" but send the STOP
      // (urgent slot, purges queued motion) instead of leaving the last w running until the Uno's deadman.
      {MotionEvent::kTxFull, true, EndReason::kLink}};
  for (const auto& c : cases) {
    Rig r;
    r.ml.onTelemetry(0, 0, 20);
    const LinkOut o = r.ml.onEvent(c.e, 30);
    TEST_ASSERT_TRUE(o.event);
    TEST_ASSERT_FALSE(o.drive);
    TEST_ASSERT_EQUAL(c.stop, o.stop);
    TEST_ASSERT_EQUAL(static_cast<int>(c.why), static_cast<int>(o.end.reason));
    TEST_ASSERT_FALSE(r.ml.service().active());
    TEST_ASSERT_FALSE(r.ml.onEvent(c.e, 40).event);  // nothing left to end
  }
}

void test_link_done_sends_stop_but_not_after_ir() {
  Rig r;  // reaches the window and is done after 200 ms; the STOP goes out while still the owner
  LinkOut o;
  uint32_t t = 20;
  while (!o.event && t < 2000) o = r.ml.onTelemetry(8990, 0, t += 20);
  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDone), static_cast<int>(o.end.reason));
  TEST_ASSERT_TRUE(o.stop);
  Rig r2;  // same, but IR took over just before the final frame (review M3): no bridge STOP
  t = 20;
  for (int i = 0; i < 10; ++i) r2.ml.onTelemetry(8990, 0, t += 20);
  r2.owner.onUnoOwner(proto::Owner::kIr);
  o = r2.ml.onTelemetry(8990, 0, t += 20);
  TEST_ASSERT_TRUE(o.event);
  TEST_ASSERT_FALSE(o.stop);
}

void test_link_telemetry_gap_stops_while_owner() {
  Rig r;
  r.ml.onTelemetry(0, 0, 20);
  const LinkOut o = r.ml.tick(300);
  TEST_ASSERT_TRUE(o.event);
  TEST_ASSERT_TRUE(o.stop);
  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kLink), static_cast<int>(o.end.reason));
}

// ---------- Plan K (bridge LLD R7.4 / R7.6): moves in the service and the link ----------
namespace {
MotionTelem encAt(int32_t counts) {
  MotionTelem t;
  t.enc_l = counts;
  t.enc_r = counts;
  return t;
}
struct MoveRig {
  LinkOwner owner;
  MotionLink ml{owner};
  MoveRig() {
    owner.begin();  // R14: the bridge is master from boot
    owner.onWsOpen(kFd);
    owner.onSynced(true);
    Actions a;
    owner.onDrive(kFd, 0, a);  // the move command took ownership
    ml.service().startMove(kFd, encAt(0), 10, 40, motion::kCpmDefault, 4, 0);  // 10 cm = 756 counts
  }
};
}  // namespace

void test_service_move_runs_and_ends_done_with_moved() {
  MotionService svc;
  svc.startMove(3, encAt(500), 10, 40, motion::kCpmDefault, 5, 0);
  TEST_ASSERT_EQUAL(static_cast<int>(PrimKind::kMove), static_cast<int>(svc.kind()));
  MotionOut o;
  int32_t counts = 500;
  uint32_t t = 0;
  for (int i = 0; i < 1000 && !o.ended; ++i) {
    o = svc.onTelemetry(encAt(counts), t += 20);
    if (!o.ended) {
      TEST_ASSERT_TRUE(o.send);
      TEST_ASSERT_EQUAL_INT8(0, o.w);
      TEST_ASSERT_TRUE(o.v > 0);
    }
    counts += 20;
  }
  TEST_ASSERT_TRUE(o.ended);
  TEST_ASSERT_TRUE(o.end.ok);
  TEST_ASSERT_EQUAL(static_cast<int>(PrimKind::kMove), static_cast<int>(o.end.kind));
  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDone), static_cast<int>(o.end.reason));
  TEST_ASSERT_TRUE(o.end.has_moved);
  TEST_ASSERT_TRUE(o.end.moved_mm >= 100 && o.end.moved_mm <= 103);
  TEST_ASSERT_EQUAL_INT32(5, o.end.id);
  TEST_ASSERT_FALSE(svc.active());
}

void test_service_move_progress_and_blocked_end() {
  MotionService svc;
  svc.startMove(3, encAt(0), 10, 40, motion::kCpmDefault, -1, 0);
  svc.onTelemetry(encAt(378), 20);  // half way
  TEST_ASSERT_EQUAL_UINT8(50, svc.progressPct());
  MotionTelem held = encAt(378);
  held.flags = 1u << static_cast<unsigned>(proto::FastFlagBit::kBlocked);
  MotionOut o;
  for (uint32_t t = 40; t <= 2100 && !o.ended; t += 20) o = svc.onTelemetry(held, t);
  TEST_ASSERT_TRUE(o.ended);
  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kBlocked), static_cast<int>(o.end.reason));
  TEST_ASSERT_TRUE(o.end.has_moved);
}

void test_turn_end_has_no_moved() {
  MotionService svc;
  svc.startTurn(3, 0, 9000, 40, 1, 0);
  const MotionOut o = svc.cancel(EndReason::kCancelled);
  TEST_ASSERT_TRUE(o.ended);
  TEST_ASSERT_FALSE(o.end.has_moved);
}

void test_link_move_sends_auto_drive_straight() {
  MoveRig r;
  const LinkOut o = r.ml.onTelemetry(encAt(0), 20);
  TEST_ASSERT_TRUE(o.drive);
  TEST_ASSERT_EQUAL_INT8(40, o.v);
  TEST_ASSERT_EQUAL_INT8(0, o.w);
}

void test_link_uno_stall_and_fault_end_a_move_with_stop() {
  const struct { MotionEvent e; EndReason why; } cases[] = {{MotionEvent::kBodyStall, EndReason::kStalled},
                                                            {MotionEvent::kBodyEncoderFault, EndReason::kEncoderFault}};
  for (const auto& c : cases) {
    MoveRig r;
    r.ml.onTelemetry(encAt(0), 20);
    const LinkOut o = r.ml.onEvent(c.e, 30);
    TEST_ASSERT_TRUE(o.event);
    TEST_ASSERT_TRUE(o.stop);  // still the owner: the move ends cleanly with a STOP
    TEST_ASSERT_FALSE(o.drive);
    TEST_ASSERT_EQUAL(static_cast<int>(c.why), static_cast<int>(o.end.reason));
    TEST_ASSERT_EQUAL(static_cast<int>(PrimKind::kMove), static_cast<int>(o.end.kind));
    TEST_ASSERT_FALSE(r.ml.service().active());
  }
}

void test_link_uno_stall_leaves_a_turn_alone() {
  Rig r;  // the Plan H turn rig
  r.ml.onTelemetry(0, 0, 20);
  TEST_ASSERT_FALSE(r.ml.onEvent(MotionEvent::kBodyStall, 30).event);
  TEST_ASSERT_FALSE(r.ml.onEvent(MotionEvent::kBodyEncoderFault, 40).event);
  TEST_ASSERT_TRUE(r.ml.service().active());
}

void test_link_move_ends_like_turn() {  // cancel / STOP / owner loss / gap - the Plan H rules for a move
  {
    MoveRig r;
    r.ml.onTelemetry(encAt(0), 20);
    const LinkOut o = r.ml.onEvent(MotionEvent::kCancel, 30);
    TEST_ASSERT_TRUE(o.stop);
    TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kCancelled), static_cast<int>(o.end.reason));
  }
  {
    MoveRig r;
    r.ml.onTelemetry(encAt(0), 20);
    const LinkOut o = r.ml.onEvent(MotionEvent::kStop, 30);
    TEST_ASSERT_FALSE(o.stop);  // the STOP is already on its way
    TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kStop), static_cast<int>(o.end.reason));
  }
  {
    MoveRig r;
    r.ml.onTelemetry(encAt(0), 20);
    r.owner.onUnoOwner(proto::Owner::kIr);
    const LinkOut o = r.ml.onTelemetry(encAt(20), 40);
    TEST_ASSERT_TRUE(o.event);
    TEST_ASSERT_FALSE(o.stop);
    TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kNotOwner), static_cast<int>(o.end.reason));
  }
  {
    MoveRig r;
    r.ml.onTelemetry(encAt(0), 20);
    const LinkOut o = r.ml.tick(300);
    TEST_ASSERT_TRUE(o.stop);
    TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kLink), static_cast<int>(o.end.reason));
  }
}

// ---------- Plan L (bridge LLD R8): arcs and pivots ----------
void test_router_arc_and_pivot_need_encoders() {
  const uint8_t enc = kLiteCaps | bit(proto::CapBit::kEncoders);
  TEST_ASSERT_EQUAL(R(Route::kLocal), R(routeTurn(enc, 60)));
  TEST_ASSERT_EQUAL(R(Route::kLocal), R(routeTurn(enc, 0, true)));
  TEST_ASSERT_EQUAL(R(Route::kUnavailable), R(routeTurn(kLiteCaps, 0, true)));
  TEST_ASSERT_EQUAL(R(Route::kLocal), R(routeTurn(kLiteCaps, 0)));  // the spin needs only the gyro
  TEST_ASSERT_EQUAL(R(Route::kForward), R(routeTurn(enc | bit(proto::CapBit::kMove), 60)));
}

void test_service_pivot_keeps_inside_track_still() {
  for (int dir = -1; dir <= 1; dir += 2) {
    MotionService svc;
    svc.startPivot(3, 0, dir * 9000, 40, 1, 0);
    TEST_ASSERT_EQUAL(static_cast<int>(PrimKind::kPivot), static_cast<int>(svc.kind()));
    const MotionOut o = svc.onTelemetry(0, 0, 20);
    TEST_ASSERT_TRUE(o.send);
    const int l = o.v - o.w, r = o.v + o.w;  // the Uno's plain AUTO mix
    TEST_ASSERT_EQUAL_INT(0, dir > 0 ? l : r);           // inside track stopped
    TEST_ASSERT_TRUE((dir > 0 ? r : l) <= 40);           // outside at most the asked speed
    TEST_ASSERT_TRUE((dir > 0 ? r : l) > 0);
  }
}

void test_service_pivot_speed_floor() {  // Review Focus 4
  MotionService svc;
  svc.startPivot(3, 0, 9000, 12, 1, 0);
  const MotionOut o = svc.onTelemetry(0, 0, 20);
  TEST_ASSERT_EQUAL_INT8(motion::kTurnWMin, o.w);
  TEST_ASSERT_EQUAL_INT8(motion::kTurnWMin, o.v);  // outside track 24 %, never a stalled 12 %
}

void test_service_arc_runs_and_cancels() {
  MotionService svc;
  svc.startArc(3, encAt(0), 9000, 60, 40, motion::kCpmDefault, 2, 0);
  TEST_ASSERT_EQUAL(static_cast<int>(PrimKind::kArc), static_cast<int>(svc.kind()));
  const MotionOut o = svc.onTelemetry(encAt(30), 20);
  TEST_ASSERT_TRUE(o.send);
  TEST_ASSERT_EQUAL_INT8(40, o.v);
  const MotionOut e = svc.cancel(EndReason::kCancelled);
  TEST_ASSERT_TRUE(e.ended);
  TEST_ASSERT_EQUAL(static_cast<int>(PrimKind::kArc), static_cast<int>(e.end.kind));
  TEST_ASSERT_FALSE(e.end.has_moved);
  TEST_ASSERT_EQUAL_INT32(2, e.end.id);
}

void test_link_uno_stall_ends_an_arc_not_a_pivot() {
  {
    LinkOwner owner;
    MotionLink ml{owner};
    owner.begin();  // R14: the bridge is master from boot
    owner.onWsOpen(kFd);
    owner.onSynced(true);
    Actions a;
    owner.onDrive(kFd, 0, a);
    ml.service().startArc(kFd, encAt(0), 9000, 60, 40, motion::kCpmDefault, 4, 0);
    ml.onTelemetry(encAt(0), 20);
    const LinkOut o = ml.onEvent(MotionEvent::kBodyStall, 30);
    TEST_ASSERT_TRUE(o.event);
    TEST_ASSERT_TRUE(o.stop);
    TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kStalled), static_cast<int>(o.end.reason));
  }
  {
    LinkOwner owner;
    MotionLink ml{owner};
    owner.begin();  // R14: the bridge is master from boot
    owner.onWsOpen(kFd);
    owner.onSynced(true);
    Actions a;
    owner.onDrive(kFd, 0, a);
    ml.service().startPivot(kFd, 0, 9000, 40, 5, 0);
    ml.onTelemetry(0, 0, 20);
    TEST_ASSERT_FALSE(ml.onEvent(MotionEvent::kBodyStall, 30).event);  // pivots keep the spin's own stall check
    TEST_ASSERT_TRUE(ml.service().active());
  }
}

// Plan L (spec 02 §H6): a spinning drive_rate is never slower than the slowest smooth spin.
void test_rate_clamp_to_minimum_spin() {  // Review Focus 5
  const struct { int8_t v; int16_t asked; int16_t got; bool limited; } cases[] = {
      {0, 50, motion::kSpinMinDdps, true},   {0, -50, -motion::kSpinMinDdps, true}, {0, -10, -motion::kSpinMinDdps, true},
      {0, 0, 0, false},                       {0, 800, 800, false},                  {0, motion::kSpinMinDdps, motion::kSpinMinDdps, false},
      {30, 50, 50, false}};
  for (const auto& c : cases) {
    MotionService svc;
    svc.driveRate(3, c.v, c.asked, 0);
    TEST_ASSERT_EQUAL_INT16(c.got, svc.rateTargetDdps());
    TEST_ASSERT_EQUAL(c.limited, svc.limited());
  }
}


// Floor 2026-10-18 (review M5): a pivot that corrects back past its target must still hold the same (inside) track -
// the outside track reverses; it never swings round the other track.
void test_service_pivot_correction_keeps_the_same_track() {
  for (int dir = -1; dir <= 1; dir += 2) {
    MotionService svc;
    svc.startPivot(3, 0, dir * 9000, 40, 1, 0);
    // 5° past the target, still: R18 coasts (w = 0) until stopped, then nudges back
    const uint16_t past = static_cast<uint16_t>(dir > 0 ? 9500 : 36000 - 9500);
    MotionOut o;
    for (uint32_t t = 20; t <= 1000 && !(o.send && o.w != 0); t += 20) o = svc.onTelemetry(past, 0, t);
    TEST_ASSERT_TRUE(o.send);
    TEST_ASSERT_TRUE(dir > 0 ? o.w < 0 : o.w > 0);  // turning back
    const int l = o.v - o.w, r = o.v + o.w;
    TEST_ASSERT_EQUAL_INT(0, dir > 0 ? l : r);     // the inside track stays still
    TEST_ASSERT_TRUE((dir > 0 ? r : l) < 0);       // the outside track backs up
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_router_lite_runs_turn_and_rate_locally);
  RUN_TEST(test_router_full_body_forwards);
  RUN_TEST(test_router_move_needs_encoders);
  RUN_TEST(test_turn_target_wrap);
  RUN_TEST(test_turn_far_error_runs_at_speed_toward_target);
  RUN_TEST(test_turn_slow_zone_holds_the_slowest_smooth_spin);
  RUN_TEST(test_turn_short_turn_starts_slow);
  RUN_TEST(test_turn_cuts_when_the_predicted_stop_reaches_the_target);
  RUN_TEST(test_turn_done_once_stopped_with_the_coast_reading);
  RUN_TEST(test_turn_one_nudge_when_it_stops_short);
  RUN_TEST(test_turn_nudge_back_after_an_overshoot);
  RUN_TEST(test_turn_within_three_degrees_at_rest_does_not_push);
  RUN_TEST(test_turn_coast_reading_ignores_bad_cuts);
  RUN_TEST(test_coast_average_moves_a_quarter_and_stays_in_range);
  RUN_TEST(test_turn_shaking_at_target_finishes_without_corrections);
  RUN_TEST(test_turn_slow_drift_is_not_done);
  RUN_TEST(test_turn_stalled_and_timeout);
  RUN_TEST(test_turn_stalled_in_the_slow_zone);
  RUN_TEST(test_turn_tracks_multi_turn);
  RUN_TEST(test_rate_feed_forward_at_zero_error);
  RUN_TEST(test_rate_integral_grows_on_shortfall);
  RUN_TEST(test_rate_anti_windup_and_clamp);
  RUN_TEST(test_rate_sign_change_resets_integral);
  RUN_TEST(test_rate_zero_target_clears_integral);
  RUN_TEST(test_service_turn_closed_loop);
  RUN_TEST(test_service_turn_realistic_one_smooth_stop);
  RUN_TEST(test_service_turn_realistic_learns_the_coast);
  RUN_TEST(test_service_pivot_realistic_one_smooth_stop);
  RUN_TEST(test_service_turn_end_carries_the_coast_reading);
  RUN_TEST(test_turn_every_end_sends_zero_then_stop);
  RUN_TEST(test_service_owner_loss_ends);
  RUN_TEST(test_service_telemetry_gap_ends_link);
  RUN_TEST(test_service_stuck_tank_ends_stalled);
  RUN_TEST(test_rate_deadman);
  RUN_TEST(test_rate_converges_in_model);
  RUN_TEST(test_rate_reverse_converges);
  RUN_TEST(test_rate_diverging_body_ends);
  RUN_TEST(test_rate_stalled_body_ends);
  RUN_TEST(test_rate_straight_is_not_stalled);
  RUN_TEST(test_link_running_turn_sends_auto_drive);
  RUN_TEST(test_link_release_ends_and_never_retakes);
  RUN_TEST(test_link_lost_owner_never_retakes_even_if_not_told);
  RUN_TEST(test_link_ir_takeover_ends_without_frames);
  RUN_TEST(test_link_after_release_ends_without_frames);
  RUN_TEST(test_link_events_frames_policy);
  RUN_TEST(test_link_done_sends_stop_but_not_after_ir);
  RUN_TEST(test_link_telemetry_gap_stops_while_owner);
  RUN_TEST(test_service_move_runs_and_ends_done_with_moved);
  RUN_TEST(test_service_move_progress_and_blocked_end);
  RUN_TEST(test_turn_end_has_no_moved);
  RUN_TEST(test_link_move_sends_auto_drive_straight);
  RUN_TEST(test_link_uno_stall_and_fault_end_a_move_with_stop);
  RUN_TEST(test_link_uno_stall_leaves_a_turn_alone);
  RUN_TEST(test_link_move_ends_like_turn);
  RUN_TEST(test_router_arc_and_pivot_need_encoders);
  RUN_TEST(test_service_pivot_keeps_inside_track_still);
  RUN_TEST(test_service_pivot_speed_floor);
  RUN_TEST(test_service_arc_runs_and_cancels);
  RUN_TEST(test_link_uno_stall_ends_an_arc_not_a_pivot);
  RUN_TEST(test_rate_clamp_to_minimum_spin);
  RUN_TEST(test_service_pivot_correction_keeps_the_same_track);
  return UNITY_END();
}

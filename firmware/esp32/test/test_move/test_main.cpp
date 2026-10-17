// Plan K (bridge LLD R7.3 / R7.8): MoveController against a straight-driving plant.
#include <unity.h>

#include "core/motion_config.h"
#include "core/move_controller.h"
#include "core/roll_learner.h"
#include "cq_protocol.h"

using namespace cq::core;
namespace proto = cq::proto;

namespace {
constexpr uint16_t kCpm = 7560;  // 75.6 counts / cm
const uint32_t kBlockedBit = 1u << static_cast<unsigned>(proto::FastFlagBit::kBlocked);

// Counts per 20 ms follow |v| with a 3-tick (60 ms) lag at 0.4 count per % per tick (40 % ~ 800 counts/s, both
// tracks) - like the Uno Lite they only count up, forward or reverse (rising edges, final review C1); after the
// command drops to 0 the tank rolls on for 5 ticks at half its last step (~0.2 cm at a crawl).
struct Plant {
  int32_t enc_l = 1000, enc_r = 2000;  // arbitrary start counts
  uint32_t flags = 0;
  int8_t hist[3] = {0, 0, 0};
  uint8_t head = 0;
  int32_t last_step = 0;
  int roll_ticks = 0;
  void step(int8_t v) {
    const int8_t applied = hist[head];
    hist[head] = v;
    head = static_cast<uint8_t>((head + 1) % 3);
    int32_t d = applied * 4 / 10;
    if (d < 0) d = -d;  // unsigned edge counts
    if (applied == 0 && last_step != 0 && roll_ticks == 0) roll_ticks = 5;
    if (applied == 0 && roll_ticks > 0) {
      d = last_step / 2;
      if (--roll_ticks == 0) last_step = 0;
    } else {
      last_step = d;
    }
    enc_l += d;
    enc_r += d;
  }
  MotionTelem telem() const {
    MotionTelem t;
    t.enc_l = enc_l;
    t.enc_r = enc_r;
    t.flags = flags;
    return t;
  }
};

struct Run {
  MoveStep last;
  uint32_t t = 0;
  int8_t max_v = 0;
  int8_t min_abs_v = 127;
};

// Steps controller + plant every 20 ms until an end or `limit_ms`.
Run runUntilEnd(MoveController& mc, Plant& p, uint32_t limit_ms) {
  Run r;
  while (r.t < limit_ms) {
    r.t += 20;
    r.last = mc.step(p.telem(), r.t);
    if (r.last.done || r.last.stalled || r.last.blocked || r.last.timeout) break;
    const int8_t a = r.last.v < 0 ? static_cast<int8_t>(-r.last.v) : r.last.v;
    if (a > r.max_v) r.max_v = a;
    if (a < r.min_abs_v) r.min_abs_v = a;
    p.step(r.last.v);
  }
  return r;
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_move_done_at_target() {
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 100, 40, kCpm, 0);
  const Run r = runUntilEnd(mc, p, 30000);
  TEST_ASSERT_TRUE(r.last.done);
  TEST_ASSERT_EQUAL_INT8(0, r.last.v);
  TEST_ASSERT_INT32_WITHIN(5, 1000, mc.movedMm());  // counted at the stop: at most one crawl tick past 100 cm
  TEST_ASSERT_GREATER_OR_EQUAL_INT32(1000, mc.movedMm());
  TEST_ASSERT_EQUAL_UINT8(100, mc.progressPct());
}

void test_move_profile_cruise_then_crawl() {
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 100, 40, kCpm, 0);
  const int32_t target = 100 * kCpm / 100, slow = motion::kMoveSlowCm * kCpm / 100;
  const int32_t start = (p.enc_l + p.enc_r) / 2;
  int8_t prev = 127;
  uint32_t t = 0;
  for (;;) {
    t += 20;
    const MoveStep s = mc.step(p.telem(), t);
    if (s.done) break;
    const int32_t remaining = target - ((p.enc_l + p.enc_r) / 2 - start);
    if (remaining >= slow) {
      TEST_ASSERT_EQUAL_INT8(40, s.v);  // cruise
    } else {
      TEST_ASSERT_TRUE(s.v <= prev);    // only falls in the slow zone ...
      TEST_ASSERT_TRUE(s.v >= motion::kMoveCrawlPct);  // ... never below the crawl
    }
    prev = s.v;
    p.step(s.v);
  }
  TEST_ASSERT_TRUE(prev <= motion::kMoveCrawlPct + 1);  // arrives at a crawl
}

void test_move_reverse_with_rising_counts() {
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, -50, 40, kCpm, 0);
  const MoveStep first = mc.step(p.telem(), 20);
  TEST_ASSERT_EQUAL_INT8(-40, first.v);
  p.step(first.v);
  const Run r = runUntilEnd(mc, p, 30000);
  TEST_ASSERT_TRUE(r.last.done);
  TEST_ASSERT_TRUE(p.enc_l > 1000);  // the Uno's counts rise in reverse too
  TEST_ASSERT_INT32_WITHIN(5, -500, mc.movedMm());
}

void test_move_progress_rises() {
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 100, 40, kCpm, 0);
  TEST_ASSERT_EQUAL_UINT8(0, mc.progressPct());
  uint8_t last = 0;
  uint32_t t = 0;
  for (int i = 0; i < 200; ++i) {
    const MoveStep s = mc.step(p.telem(), t += 20);
    TEST_ASSERT_TRUE(mc.progressPct() >= last);
    last = mc.progressPct();
    p.step(s.v);
  }
  TEST_ASSERT_TRUE(last > 10 && last < 100);
}

void test_move_stalled_without_progress() {
  Plant p;  // the plant is never stepped: no counts at all
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 100, 40, kCpm, 0);
  MoveStep s;
  for (uint32_t t = 20; t < motion::kMoveStallMs; t += 20) {
    s = mc.step(p.telem(), t);
    TEST_ASSERT_FALSE(s.stalled);
  }
  s = mc.step(p.telem(), motion::kMoveStallMs);
  TEST_ASSERT_TRUE(s.stalled);
}

void test_move_blocked_after_two_seconds_not_stalled() {
  Plant p;
  p.flags = kBlockedBit;  // obstacle brake holds the tank: no counts
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 100, 40, kCpm, 0);
  MoveStep s;
  for (uint32_t t = 20; t < motion::kMoveBlockedMs; t += 20) {
    s = mc.step(p.telem(), t);
    TEST_ASSERT_FALSE(s.stalled);
    TEST_ASSERT_FALSE(s.blocked);
  }
  s = mc.step(p.telem(), motion::kMoveBlockedMs + 20);
  TEST_ASSERT_TRUE(s.blocked);
}

void test_move_brief_block_continues() {  // Review Focus 2
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 30, 40, kCpm, 0);
  uint32_t t = 0;
  for (int i = 0; i < 50; ++i) p.step(mc.step(p.telem(), t += 20).v);  // 1 s driving
  p.flags = kBlockedBit;
  for (int i = 0; i < 50; ++i) TEST_ASSERT_FALSE(mc.step(p.telem(), t += 20).blocked);  // 1 s held, no counts
  p.flags = 0;
  MoveStep s;
  do {
    s = mc.step(p.telem(), t += 20);
    TEST_ASSERT_FALSE(s.stalled || s.blocked || s.timeout);
    p.step(s.v);
  } while (!s.done && t < 30000);
  TEST_ASSERT_TRUE(s.done);
}

void test_move_timeout() {
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 100, 40, kCpm, 0);
  const uint32_t deadline = 100 * 1000 / motion::kMoveTimeoutCmPerS + motion::kMoveTimeoutBaseMs;  // 23 s
  MoveStep s;
  for (uint32_t t = 20; t < deadline; t += 20) {
    if (t % 1400 == 0) p.enc_l += 80, p.enc_r += 80;  // just over 1 cm every 1.4 s: never stalled, far too slow
    s = mc.step(p.telem(), t);
    TEST_ASSERT_FALSE(s.timeout || s.stalled || s.done);
  }
  s = mc.step(p.telem(), deadline);
  TEST_ASSERT_TRUE(s.timeout);
}

void test_move_counts_reset_ends_stalled() {  // Review Focus 1: the Uno restarted mid-move
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 200, 40, kCpm, 0);
  uint32_t t = 0;
  for (int i = 0; i < 100; ++i) p.step(mc.step(p.telem(), t += 20).v);  // 2 s driving
  p.enc_l = 0;
  p.enc_r = 0;
  const uint32_t reset_at = t;
  MoveStep s;
  do {
    s = mc.step(p.telem(), t += 20);
    TEST_ASSERT_FALSE(s.done);
    p.step(s.v);
  } while (!s.stalled && t < reset_at + 5000);
  TEST_ASSERT_TRUE(s.stalled);
  TEST_ASSERT_TRUE(t - reset_at <= motion::kMoveStallMs + 100);
}

void test_move_speed_below_crawl_never_zero() {  // Review Focus 3
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 10, motion::kMoveSpeedMin, kCpm, 0);  // 10 cm at 10 % ~ 4 s (timeout 5 s)
  const Run r = runUntilEnd(mc, p, 30000);
  TEST_ASSERT_TRUE(r.last.done);
  TEST_ASSERT_EQUAL_INT8(motion::kMoveSpeedMin, r.min_abs_v);
  TEST_ASSERT_EQUAL_INT8(motion::kMoveSpeedMin, r.max_v);
}

void test_move_short_move_stays_slow() {  // Review Focus 4
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 1, 100, kCpm, 0);
  const Run r = runUntilEnd(mc, p, 10000);
  TEST_ASSERT_TRUE(r.last.done);
  TEST_ASSERT_TRUE(r.max_v <= 23);  // 15 + 85 × 75 / 756 at the start
  TEST_ASSERT_TRUE(r.min_abs_v >= motion::kMoveCrawlPct);
}

void test_cpm_limits() {
  TEST_ASSERT_TRUE(motion::validCpm(7560));
  TEST_ASSERT_TRUE(motion::validCpm(3000));
  TEST_ASSERT_TRUE(motion::validCpm(15000));
  TEST_ASSERT_FALSE(motion::validCpm(2999));
  TEST_ASSERT_FALSE(motion::validCpm(15001));
  TEST_ASSERT_FALSE(motion::validCpm(0));
}


// ---------- spec 02 §G6 / LLD R7.9: learned early stop ----------
void test_move_stops_early_by_roll() {
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 100, 40, kCpm, 0, 25);  // roll-on 25 mm = 189 counts
  const int32_t start = (p.enc_l + p.enc_r) / 2;
  const Run r = runUntilEnd(mc, p, 30000);
  TEST_ASSERT_TRUE(r.last.done);
  const int32_t at_stop = (p.enc_l + p.enc_r) / 2 - start;
  TEST_ASSERT_INT32_WITHIN(8, 7560 - 189, at_stop);  // stopped 2.5 cm short of the target ...
  TEST_ASSERT_UINT8_WITHIN(1, 97, mc.progressPct());  // ... progress still against the full target
}

void test_move_early_stop_never_before_half() {
  Plant p;
  MoveController mc;
  mc.begin(p.enc_l, p.enc_r, 3, 40, kCpm, 0, 25);  // 3 cm = 226 counts, roll 189 -> capped at 113
  const int32_t start = (p.enc_l + p.enc_r) / 2;
  const Run r = runUntilEnd(mc, p, 10000);
  TEST_ASSERT_TRUE(r.last.done);
  TEST_ASSERT_TRUE((p.enc_l + p.enc_r) / 2 - start >= 113);
}

void test_roll_learner_settles_and_measures() {
  RollLearner rl;
  rl.begin(5000, 0);
  int32_t mm = -1;
  const int32_t counts[] = {5040, 5080, 5110, 5130, 5140, 5143, 5143, 5143, 5143, 5143, 5143};
  uint32_t t = 0;
  bool ready = false;
  for (int32_t c : counts) {
    ready = rl.observe(c, false, kCpm, t += 20, mm);
    if (ready) break;
  }
  TEST_ASSERT_TRUE(ready);   // 100 ms without a change
  TEST_ASSERT_EQUAL_INT32(18, mm);  // 143 counts at 75.6 / cm = 18.9 -> 18 mm
  TEST_ASSERT_FALSE(rl.watching());
}

void test_roll_learner_gives_up_and_discards() {
  RollLearner rl;
  int32_t mm = 0;
  rl.begin(0, 0);  // counts keep changing: a reading at the 600 ms limit
  bool ready = false;
  for (uint32_t t = 20; t <= motion::kRollWindowMs && !ready; t += 20) ready = rl.observe(static_cast<int32_t>(t / 4), false, kCpm, t, mm);
  TEST_ASSERT_TRUE(ready);
  TEST_ASSERT_EQUAL_INT32(19, mm);  // 150 counts
  rl.begin(0, 0);  // driven again in the window (after the PWM was 0): no reading
  TEST_ASSERT_FALSE(rl.observe(10, false, kCpm, 20, mm));
  TEST_ASSERT_FALSE(rl.observe(30, true, kCpm, 40, mm));
  TEST_ASSERT_FALSE(rl.watching());
  rl.begin(0, 0);  // more than 80 mm: no reading
  ready = false;
  for (uint32_t t = 20; t <= 200 && !ready; t += 20) ready = rl.observe(1000, false, kCpm, t, mm);
  TEST_ASSERT_FALSE(ready);
  TEST_ASSERT_FALSE(rl.watching());
}

void test_roll_average() {
  TEST_ASSERT_EQUAL_UINT16(25, rollAverage(25, 25));
  TEST_ASSERT_EQUAL_UINT16(26, rollAverage(25, 28));  // (75 + 28 + 2) / 4 = 26
  TEST_ASSERT_EQUAL_UINT16(19, rollAverage(25, 0));   // (75 + 0 + 2) / 4 = 19
  TEST_ASSERT_EQUAL_UINT16(39, rollAverage(25, 80));
}


void test_roll_learner_ignores_frames_still_in_flight() {  // floor 2026-10-17: TELEM_FAST sent before the STOP arrived
  RollLearner rl;
  rl.begin(0, 0);
  int32_t mm = 0;
  TEST_ASSERT_FALSE(rl.observe(20, true, kCpm, 20, mm));  // the old PWM is still reported ...
  TEST_ASSERT_FALSE(rl.observe(40, true, kCpm, 40, mm));
  TEST_ASSERT_TRUE(rl.watching());                         // ... not "driven again"
  bool ready = false;
  for (uint32_t t = 60; t <= 400 && !ready; t += 20) ready = rl.observe(t < 200 ? 50 + static_cast<int32_t>(t) : 250, false, kCpm, t, mm);
  TEST_ASSERT_TRUE(ready);
  TEST_ASSERT_EQUAL_INT32(33, mm);  // 250 counts
  rl.begin(0, 0);  // after the PWM was 0, a driven frame is a new drive: no reading
  TEST_ASSERT_FALSE(rl.observe(10, false, kCpm, 20, mm));
  TEST_ASSERT_FALSE(rl.observe(20, true, kCpm, 40, mm));
  TEST_ASSERT_FALSE(rl.watching());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_move_done_at_target);
  RUN_TEST(test_move_profile_cruise_then_crawl);
  RUN_TEST(test_move_reverse_with_rising_counts);
  RUN_TEST(test_move_progress_rises);
  RUN_TEST(test_move_stalled_without_progress);
  RUN_TEST(test_move_blocked_after_two_seconds_not_stalled);
  RUN_TEST(test_move_brief_block_continues);
  RUN_TEST(test_move_timeout);
  RUN_TEST(test_move_counts_reset_ends_stalled);
  RUN_TEST(test_move_speed_below_crawl_never_zero);
  RUN_TEST(test_move_short_move_stays_slow);
  RUN_TEST(test_cpm_limits);
  RUN_TEST(test_move_stops_early_by_roll);
  RUN_TEST(test_move_early_stop_never_before_half);
  RUN_TEST(test_roll_learner_settles_and_measures);
  RUN_TEST(test_roll_learner_gives_up_and_discards);
  RUN_TEST(test_roll_average);
  RUN_TEST(test_roll_learner_ignores_frames_still_in_flight);
  return UNITY_END();
}

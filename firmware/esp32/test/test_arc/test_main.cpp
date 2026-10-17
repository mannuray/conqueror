// Plan L (bridge LLD R8): arc speed band, minimum radius, refusal text; ArcController (Task 2).
#include <math.h>
#include <string.h>
#include <unity.h>

#include "core/arc_controller.h"
#include "core/arc_limits.h"
#include "core/motion_config.h"
#include "cq_protocol.h"

using namespace cq::core;
namespace proto = cq::proto;

void setUp() {}
void tearDown() {}

void test_arc_band_matches_spec_table() {
  const struct { uint16_t r; uint8_t lo, hi; } cases[] = {{31, 56, 56},  {40, 43, 64},  {49, 36, 69}, {60, 31, 74},
                                                           {80, 26, 80},  {100, 23, 83}, {150, 19, 88}};
  for (const auto& c : cases) {
    const ArcBand b = arcBand(c.r);
    TEST_ASSERT_TRUE(b.possible);
    TEST_ASSERT_EQUAL_UINT8(c.lo, b.lo);
    TEST_ASSERT_EQUAL_UINT8(c.hi, b.hi);
  }
}

void test_arc_too_tight_and_min_radius() {
  TEST_ASSERT_FALSE(arcBand(30).possible);  // inner needs > outer allows
  TEST_ASSERT_FALSE(arcBand(9).possible);   // inside B/2: the inner track would reverse
  TEST_ASSERT_FALSE(arcBand(0).possible);
  TEST_ASSERT_EQUAL_UINT16(31, arcMinRadiusCm());
  const ArcBand wide = arcBand(1000);
  TEST_ASSERT_TRUE(wide.possible);
  TEST_ASSERT_TRUE(wide.lo >= motion::kTurnWMin && wide.hi <= 100);
}

void test_arc_refusal_messages() {
  char buf[96];
  TEST_ASSERT_TRUE(arcRefusal(buf, sizeof(buf), 40) > 0);
  TEST_ASSERT_EQUAL_STRING("radius 40 cm: use speed 43-64 %, or pivot", buf);
  arcRefusal(buf, sizeof(buf), 31);
  TEST_ASSERT_EQUAL_STRING("radius 31 cm: use speed 56-56 %, or pivot", buf);
  arcRefusal(buf, sizeof(buf), 20);
  TEST_ASSERT_EQUAL_STRING("radius 20 cm too tight (min 31): use pivot", buf);
  TEST_ASSERT_EQUAL(0, arcRefusal(buf, 8, 40));  // does not fit -> 0
}

// ---------- ArcController against a plant ----------
namespace {
constexpr uint16_t kCpm = 7560;
// Counts follow v (spec H1 speed model, 75.6 counts / cm) and the turn rate follows w (2.2 °/s per %, 150 ms lag),
// both acting on the command sent 3 frames earlier (60 ms).
struct ArcPlant {
  double yaw_deg = 0.0, rate_dps = 0.0, counts = 0.0;
  int8_t hv[3] = {0, 0, 0}, hw[3] = {0, 0, 0};
  uint8_t head = 0;
  uint32_t flags = 0;
  uint16_t dist_mm = 0xFFFF;
  void step(int8_t v, int8_t w) {
    const int8_t av = hv[head], aw = hw[head];
    hv[head] = v;
    hw[head] = w;
    head = static_cast<uint8_t>((head + 1) % 3);
    const double v_cm = av > 0 ? motion::kTrackSpeedOffsetCmS + motion::kTrackSpeedSlopeCmS * av : 0.0;
    counts += v_cm * 75.6 * 0.02;
    rate_dps += (aw * 2.2 - rate_dps) * (0.02 / 0.15);
    yaw_deg += rate_dps * 0.02;
  }
  MotionTelem telem() const {
    MotionTelem t;
    double y = fmod(yaw_deg, 360.0);
    if (y < 0) y += 360.0;
    t.yaw_cdeg = static_cast<uint16_t>(lround(y * 100.0) % 36000);
    t.rate_ddps = static_cast<int16_t>(lround(rate_dps * 10.0));
    t.enc_l = t.enc_r = static_cast<int32_t>(counts);
    t.flags = flags;
    t.dist_mm = dist_mm;
    return t;
  }
};

ArcStep runArc(ArcController& ac, ArcPlant& p, uint32_t& t, uint32_t limit_ms) {
  ArcStep s;
  while (t < limit_ms) {
    t += 20;
    s = ac.step(p.telem(), t);
    if (s.done || s.stalled || s.blocked || s.timeout || s.diverged) break;
    p.step(s.v, s.w);
  }
  return s;
}
}  // namespace

void test_arc_left_and_right_end_at_heading() {
  for (int dir = -1; dir <= 1; dir += 2) {
    ArcPlant p;
    ArcController ac;
    uint32_t t = 0;
    ac.begin(p.telem(), dir * 9000, 60, 40, kCpm, 0);
    const ArcStep s = runArc(ac, p, t, 40000);
    TEST_ASSERT_TRUE(s.done);
    TEST_ASSERT_FLOAT_WITHIN(static_cast<float>(2.0), static_cast<float>(dir * 90.0), static_cast<float>(p.yaw_deg));
    TEST_ASSERT_EQUAL_UINT8(100, ac.progressPct());
  }
}

void test_arc_rate_tracks_v_over_r() {
  // 3 s in: v = 22.7 cm/s -> ω = 57.3 · 22.7 / 60 = 21.7 °/s; the gyro rate is within 15 % of the target
  ArcPlant q;
  ArcController bc;
  uint32_t u = 0;
  bc.begin(q.telem(), 18000, 60, 40, kCpm, 0);
  for (int i = 0; i < 150; ++i) {
    const ArcStep s = bc.step(q.telem(), u += 20);
    q.step(s.v, s.w);
  }
  TEST_ASSERT_INT_WITHIN(25, 217, bc.targetDdps());
  TEST_ASSERT_FLOAT_WITHIN(static_cast<float>(0.15 * 21.7), static_cast<float>(21.7), static_cast<float>(q.rate_dps));
  TEST_ASSERT_TRUE(bc.progressPct() > 0 && bc.progressPct() < 100);
}

void test_arc_stalled_and_blocked() {
  {
    ArcPlant p;  // never stepped: no counts, no heading
    ArcController ac;
    ac.begin(p.telem(), 9000, 60, 40, kCpm, 0);
    ArcStep s;
    for (uint32_t t = 20; t < motion::kArcStallMs; t += 20) TEST_ASSERT_FALSE(ac.step(p.telem(), t).stalled);
    s = ac.step(p.telem(), motion::kArcStallMs);
    TEST_ASSERT_TRUE(s.stalled);
  }
  {
    ArcPlant p;
    p.dist_mm = 150;  // stopped with a box close ahead -> blocked
    ArcController ac;
    ac.begin(p.telem(), 9000, 60, 40, kCpm, 0);
    TEST_ASSERT_TRUE(ac.step(p.telem(), motion::kArcStallMs).blocked);
  }
  {
    ArcPlant p;
    p.flags = 1u << static_cast<unsigned>(proto::FastFlagBit::kBlocked);
    ArcController ac;
    ac.begin(p.telem(), 9000, 60, 40, kCpm, 0);
    ArcStep s;
    for (uint32_t t = 20; t < motion::kMoveBlockedMs; t += 20) {
      s = ac.step(p.telem(), t);
      TEST_ASSERT_FALSE(s.blocked || s.stalled);
    }
    TEST_ASSERT_TRUE(ac.step(p.telem(), motion::kMoveBlockedMs + 20).blocked);
  }
}

void test_arc_timeout() {
  ArcController ac;
  MotionTelem tel;
  ac.begin(tel, 9000, 60, 40, kCpm, 0);  // 94 cm of arc -> 18.8 s + 3 s
  const uint32_t deadline = 94u * 1000u / motion::kArcTimeoutCmPerS + motion::kArcTimeoutBaseMs;
  for (uint32_t t = 20; t < deadline; t += 20) {
    if (t % 1400 == 0) tel.enc_l += 80, tel.enc_r += 80;  // creeping: never stalled, never turning
    TEST_ASSERT_FALSE(ac.step(tel, t).timeout);
  }
  TEST_ASSERT_TRUE(ac.step(tel, deadline).timeout);
}

void test_arc_diverged() {
  ArcController ac;
  MotionTelem tel;
  ac.begin(tel, 9000, 60, 40, kCpm, 0);
  ArcStep s;
  double yaw = 0.0;
  uint32_t t = 0;
  for (int i = 0; i < 400 && !s.diverged; ++i) {
    tel.enc_l += 30;  // moving
    tel.enc_r += 30;
    yaw -= 0.6;  // ... but turning right at 30 °/s while asked left
    double y = fmod(yaw, 360.0);
    if (y < 0) y += 360.0;
    tel.yaw_cdeg = static_cast<uint16_t>(lround(y * 100.0) % 36000);
    tel.rate_ddps = -300;
    s = ac.step(tel, t += 20);
    TEST_ASSERT_FALSE(s.done || s.stalled || s.timeout);
  }
  TEST_ASSERT_TRUE(s.diverged);
  TEST_ASSERT_EQUAL_INT8(0, s.w);
}

void test_arc_counts_reset_never_turns_backwards() {  // Review Focus 1
  ArcPlant p;
  ArcController ac;
  uint32_t t = 0;
  ac.begin(p.telem(), 9000, 60, 40, kCpm, 0);
  for (int i = 0; i < 50; ++i) {
    const ArcStep s = ac.step(p.telem(), t += 20);
    p.step(s.v, s.w);
  }
  p.counts = 0.0;  // Uno restarted
  for (int i = 0; i < 10; ++i) {
    const ArcStep s = ac.step(p.telem(), t += 20);
    TEST_ASSERT_TRUE(ac.targetDdps() >= 0);  // a left arc never asks for a right turn
    if (s.done || s.stalled || s.blocked) break;
    p.step(s.v, s.w);
  }
}

void test_arc_more_than_one_turn() {  // Review Focus 2
  ArcPlant p;
  ArcController ac;
  uint32_t t = 0;
  ac.begin(p.telem(), 40000, 100, 40, kCpm, 0);  // 400° on a 1 m radius
  const ArcStep s = runArc(ac, p, t, 120000);
  TEST_ASSERT_TRUE(s.done);
  TEST_ASSERT_FLOAT_WITHIN(static_cast<float>(3.0), static_cast<float>(400.0), static_cast<float>(p.yaw_deg));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_arc_band_matches_spec_table);
  RUN_TEST(test_arc_too_tight_and_min_radius);
  RUN_TEST(test_arc_refusal_messages);
  RUN_TEST(test_arc_left_and_right_end_at_heading);
  RUN_TEST(test_arc_rate_tracks_v_over_r);
  RUN_TEST(test_arc_stalled_and_blocked);
  RUN_TEST(test_arc_timeout);
  RUN_TEST(test_arc_diverged);
  RUN_TEST(test_arc_counts_reset_never_turns_backwards);
  RUN_TEST(test_arc_more_than_one_turn);
  return UNITY_END();
}

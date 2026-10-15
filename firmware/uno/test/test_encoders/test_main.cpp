#include <unity.h>

#include "domain/estimation/encoder_health.h"
#include "domain/estimation/encoder_track.h"

using namespace cq;

void setUp() {}
void tearDown() {}

void test_track_counts_signed_and_speed() {
  EncoderTrack t;
  t.update(0, 0, 0, 0, 1);
  t.update(100, 1000, 99000, 100000, 1);  // 1 ms between edges → 1000 cps
  TEST_ASSERT_EQUAL(100, t.count());
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 1000.0f, t.cps());
  t.update(150, 1000, 149000, 150000, -1);
  TEST_ASSERT_EQUAL(50, t.count());
  TEST_ASSERT_FLOAT_WITHIN(1.0f, -1000.0f, t.cps());
  t.update(150, 1000, 149000, 260000, -1);  // no edge for 111 ms
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, t.cps());
  TEST_ASSERT_FALSE(t.recent());
}

namespace {
uint8_t runHealth(EncoderHealth& h, EncoderHealth::Inputs in, uint32_t from_ms, uint32_t to_ms) {
  uint8_t stalls = 0;
  for (uint32_t t = from_ms; t < to_ms; t += 10) {
    h.update(in, t);
    stalls |= h.takeNewStalls();
  }
  return stalls;
}
}  // namespace

void test_fault_vs_stall() {
  // A: both tracks held against a wall, straight drive, no yaw → stall after 0.5 s, never a fault
  EncoderHealth a;
  const EncoderHealth::Inputs held{150, 150, 150, 150, false, false, 0, 55, 55};
  TEST_ASSERT_EQUAL(0, runHealth(a, held, 0, 500));  // t = 0…490 ms: not yet
  TEST_ASSERT_EQUAL(0x03, runHealth(a, held, 500, 510));
  TEST_ASSERT_EQUAL(0x03, a.stalledMask());
  TEST_ASSERT_EQUAL(0, a.takeNewFaults());

  // B: right encoder ticks, left silent, tank goes straight → left encoder broken (latched)
  EncoderHealth b;
  runHealth(b, {150, 150, 150, 150, false, true, 0, 55, 55}, 0, 510);
  TEST_ASSERT_EQUAL(0x01, b.takeNewFaults());
  runHealth(b, {0, 0, 0, 0, false, false, 0, 55, 55}, 510, 1000);
  TEST_ASSERT_TRUE(b.faulted(0));  // latched

  // C: left track held while the right drives → tank pivots (yaw) → left is a stall
  EncoderHealth c;
  TEST_ASSERT_EQUAL(0x01, runHealth(c, {150, 150, 150, 150, false, true, 35, 55, 55}, 0, 510));
  TEST_ASSERT_EQUAL(0, c.takeNewFaults());

  // D: spin, both encoders silent but the gyro shows rotation → both faulted
  EncoderHealth d;
  runHealth(d, {-150, 150, -150, 150, false, false, 60, 55, 55}, 0, 510);
  TEST_ASSERT_EQUAL(0x03, d.takeNewFaults());
}

void test_stall_clears_on_reverse_or_after_1s() {
  const EncoderHealth::Inputs held{150, 150, 150, 150, false, false, 0, 55, 55};
  const EncoderHealth::Inputs cut{0, 0, 150, 150, false, false, 0, 55, 55};  // supervisor cuts, still asked
  EncoderHealth h;
  runHealth(h, held, 0, 510);  // stalled at t = 500
  TEST_ASSERT_TRUE(h.stalled(0));
  runHealth(h, cut, 510, 1500);  // the cut output (PWM 0) does not clear it
  TEST_ASSERT_TRUE(h.stalled(0));
  runHealth(h, cut, 1500, 1510);  // 1 s after the stall
  TEST_ASSERT_FALSE(h.stalled(0));
  TEST_ASSERT_EQUAL(0, runHealth(h, held, 1510, 2000));  // re-detection needs another 0.5 s of drive
  TEST_ASSERT_EQUAL(0x03, runHealth(h, held, 2000, 2020));

  EncoderHealth r;
  runHealth(r, held, 0, 510);
  runHealth(r, {0, 0, -150, -150, false, false, 0, 55, 55}, 510, 520);  // asked to back away
  TEST_ASSERT_EQUAL(0, r.stalledMask());
}

void test_low_pwm_is_not_a_stall() {
  EncoderHealth h;
  TEST_ASSERT_EQUAL(0, runHealth(h, {60, 60, 60, 60, false, false, 0, 55, 55}, 0, 1000));  // below keep + 15
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_track_counts_signed_and_speed);
  RUN_TEST(test_fault_vs_stall);
  RUN_TEST(test_stall_clears_on_reverse_or_after_1s);
  RUN_TEST(test_low_pwm_is_not_a_stall);
  return UNITY_END();
}

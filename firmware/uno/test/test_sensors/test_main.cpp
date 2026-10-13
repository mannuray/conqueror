#include <unity.h>

#include "domain/estimation/battery_monitor.h"
#include "domain/util/edge_detector.h"
#include "domain/estimation/ultrasonic_filter.h"
#include "domain/input/nec_decoder.h"

using namespace cq;

void setUp() {}
void tearDown() {}

void test_ultrasonic_distance_and_median() {
  UltrasonicFilter f;
  f.onReading(EchoState::kReady, 2915, 0, 0);    // 500 mm
  f.onReading(EchoState::kReady, 20000, 0, 50);  // glitch 3430 mm
  f.onReading(EchoState::kReady, 2915, 0, 100);
  TEST_ASSERT_UINT16_WITHIN(3, 500, f.distMm());
  TEST_ASSERT_TRUE(f.valid());
}

void test_ultrasonic_offset_and_clear() {
  UltrasonicFilter f;
  for (uint32_t t = 0; t < 150; t += 50) f.onReading(EchoState::kReady, 2915, 20, t);
  TEST_ASSERT_UINT16_WITHIN(3, 480, f.distMm());
  for (uint32_t t = 150; t < 300; t += 50) f.onReading(EchoState::kClear, 0, 20, t);
  TEST_ASSERT_EQUAL_HEX16(UltrasonicFilter::kClear, f.distMm());
}

void test_ultrasonic_closing_speed() {
  UltrasonicFilter f;
  uint32_t t = 0;
  uint16_t us = 5830;  // 1000 mm, approaching 300 mm/s = 15 mm per 50 ms ≈ 87 µs
  for (int i = 0; i < 20; ++i, t += 50, us = static_cast<uint16_t>(us - 87)) f.onReading(EchoState::kReady, us, 0, t);
  TEST_ASSERT_INT16_WITHIN(5, 30, f.closingCmS());
}

void test_ultrasonic_classify_slow_clone_and_no_answer() {
  uint16_t echo = 0;
  // Bench module (2026-10-13): echo rises 2.2 ms after the trigger, 1496 µs wide = 25 cm.
  TEST_ASSERT_EQUAL(static_cast<int>(EchoState::kReady),
                    static_cast<int>(UltrasonicFilter::classify(true, 2208, true, 1496, echo)));
  TEST_ASSERT_EQUAL_UINT16(1496, echo);
  TEST_ASSERT_EQUAL(static_cast<int>(EchoState::kNoEcho),
                    static_cast<int>(UltrasonicFilter::classify(false, 0, false, 0, echo)));
  TEST_ASSERT_EQUAL(static_cast<int>(EchoState::kNoEcho),
                    static_cast<int>(UltrasonicFilter::classify(true, UltrasonicFilter::kMaxRiseUs + 1, true, 500, echo)));
  TEST_ASSERT_EQUAL(static_cast<int>(EchoState::kClear),
                    static_cast<int>(UltrasonicFilter::classify(true, 600, false, 0, echo)));      // still high
  TEST_ASSERT_EQUAL(static_cast<int>(EchoState::kClear),
                    static_cast<int>(UltrasonicFilter::classify(true, 600, true, 25001, echo)));  // > 25 ms
}

void test_ultrasonic_fault_after_three_no_echo_and_recovery() {
  UltrasonicFilter f;
  UltrasonicFilter::Change c = UltrasonicFilter::Change::kNone;
  for (int i = 0; i < 2; ++i) c = f.onReading(EchoState::kNoEcho, 0, 0, static_cast<uint32_t>(i) * 50);
  TEST_ASSERT_FALSE(f.fault());
  c = f.onReading(EchoState::kNoEcho, 0, 0, 100);  // 3rd consecutive failed measurement (LLD §9.16)
  TEST_ASSERT_EQUAL(static_cast<int>(UltrasonicFilter::Change::kFault), static_cast<int>(c));
  TEST_ASSERT_EQUAL_HEX16(UltrasonicFilter::kFault, f.distMm());
  c = f.onReading(EchoState::kReady, 2915, 0, 150);
  TEST_ASSERT_EQUAL(static_cast<int>(UltrasonicFilter::Change::kRecovered), static_cast<int>(c));
}

void test_ultrasonic_open_space_and_clone_hold_are_not_faults() {
  UltrasonicFilter f;
  uint32_t t = 0;
  for (int i = 0; i < 100; ++i, t += 50) {  // open room: echo high > 25 ms every time
    TEST_ASSERT_EQUAL(static_cast<int>(UltrasonicFilter::Change::kNone),
                      static_cast<int>(f.onReading(EchoState::kClear, 0, 0, t)));
  }
  for (int i = 0; i < 20; ++i) {  // clone holding the echo 200 ms: Clear, 4 skipped pings, Clear, …
    f.onReading(EchoState::kClear, 0, 0, t);
    t += 50;
    for (int k = 0; k < 4; ++k, t += 50) f.onReading(EchoState::kPending, 0, 0, t);
  }
  TEST_ASSERT_FALSE(f.fault());
  TEST_ASSERT_EQUAL_HEX16(UltrasonicFilter::kClear, f.distMm());
  f.onReading(EchoState::kNoEcho, 0, 0, t);  // failures must be consecutive
  f.onReading(EchoState::kNoEcho, 0, 0, t + 50);
  f.onReading(EchoState::kClear, 0, 0, t + 100);
  f.onReading(EchoState::kNoEcho, 0, 0, t + 150);
  TEST_ASSERT_FALSE(f.fault());
}

void test_ultrasonic_stuck_high_echo_faults() {
  UltrasonicFilter f;  // unplugged: the pull-up holds the echo line high, so no ping is ever sent
  UltrasonicFilter::Change c = UltrasonicFilter::Change::kNone;
  uint32_t t = 0;
  for (uint8_t i = 0; i < UltrasonicFilter::kMaxPending + 2; ++i, t += 50) {
    TEST_ASSERT_EQUAL(static_cast<int>(UltrasonicFilter::Change::kNone),
                      static_cast<int>(f.onReading(EchoState::kPending, 0, 0, t)));
  }
  c = f.onReading(EchoState::kPending, 0, 0, t);  // 3rd pending beyond 250 ms → fault (400 ms in all)
  TEST_ASSERT_EQUAL(static_cast<int>(UltrasonicFilter::Change::kFault), static_cast<int>(c));
}

namespace {
// Builds the falling-edge times of one NEC frame (MSB-first code as printed by IRremote 2.x).
int necEdges(uint32_t code, uint32_t t0, uint32_t* out) {
  int n = 0;
  uint32_t t = t0;
  out[n++] = t;
  t += 13500;
  out[n++] = t;
  for (int i = 31; i >= 0; --i) {
    t += ((code >> i) & 1u) ? 2250 : 1125;
    out[n++] = t;
  }
  return n;
}
}  // namespace

void test_nec_decodes_frame_and_repeat() {
  NecDecoder d;
  uint32_t edges[40];
  const int n = necEdges(0x00FF629DUL, 1000000, edges);
  IrEvent e{};
  int frames = 0;
  for (int i = 0; i < n; ++i) frames += d.feed(edges[i], edges[i] / 1000, e) ? 1 : 0;
  TEST_ASSERT_EQUAL(1, frames);
  TEST_ASSERT_EQUAL_HEX32(0x00FF629DUL, e.code);
  TEST_ASSERT_FALSE(e.repeat);
  // repeat frame 108 ms after the frame start: 9 ms + 2.25 ms then the stop mark
  const uint32_t r0 = 1000000 + 108000;
  TEST_ASSERT_FALSE(d.feed(r0, r0 / 1000, e));
  TEST_ASSERT_TRUE(d.feed(r0 + 11250, (r0 + 11250) / 1000, e));
  TEST_ASSERT_TRUE(e.repeat);
  TEST_ASSERT_EQUAL_HEX32(0x00FF629DUL, e.code);
}

void test_nec_rejects_bad_inverse_and_noise() {
  NecDecoder d;
  uint32_t edges[40];
  const int n = necEdges(0x00FF6200UL, 0, edges);  // command inverse wrong
  IrEvent e{};
  int frames = 0;
  for (int i = 0; i < n; ++i) frames += d.feed(edges[i], edges[i] / 1000, e) ? 1 : 0;
  TEST_ASSERT_EQUAL(0, frames);
  TEST_ASSERT_FALSE(d.feed(500000, 500, e));  // lone repeat without a recent frame
  TEST_ASSERT_FALSE(d.feed(511250, 511, e));
}

void test_battery_levels_with_persistence() {
  BatteryMonitor b;
  // Default scale 4050 = 40.5 mV per count (10-bit ADC behind the divider): 198 counts ≈ 8019 mV
  for (uint32_t t = 0; t < 3000; t += 100) b.update(198, 4050, 7200, 6600, 200, t);
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryMonitor::Level::kOk), static_cast<int>(b.level()));
  TEST_ASSERT_UINT16_WITHIN(20, 8000, b.mv());
  for (uint32_t t = 3000; t < 4000; t += 100) b.update(170, 4050, 7200, 6600, 200, t);  // 6885 mV, < 2 s
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryMonitor::Level::kOk), static_cast<int>(b.level()));
  for (uint32_t t = 4000; t < 9000; t += 100) b.update(170, 4050, 7200, 6600, 200, t);
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryMonitor::Level::kLow), static_cast<int>(b.level()));
  for (uint32_t t = 9000; t < 14000; t += 100) b.update(160, 4050, 7200, 6600, 200, t);  // 6480 mV
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryMonitor::Level::kCritical), static_cast<int>(b.level()));
}

// LLD §9.13: one sample per tick, plain average of the last 4 ticks.
void test_battery_four_sample_average() {
  BatteryMonitor b;
  b.update(200, 4050, 7200, 6600, 200, 0);  // 8100 mV; the first sample alone is the average
  TEST_ASSERT_EQUAL(8100, b.mv());
  for (uint32_t t = 100; t < 400; t += 100) b.update(200, 4050, 7200, 6600, 200, t);
  TEST_ASSERT_EQUAL(8100, b.mv());
  b.update(100, 4050, 7200, 6600, 200, 400);  // 4050 mV: (3 x 8100 + 4050) / 4
  TEST_ASSERT_EQUAL(7087, b.mv());
  for (uint32_t t = 500; t < 800; t += 100) b.update(100, 4050, 7200, 6600, 200, t);
  TEST_ASSERT_EQUAL(4050, b.mv());  // the old samples are gone after 4 ticks
}

void test_battery_usb_power_is_not_an_alarm() {
  BatteryMonitor b;
  for (uint32_t t = 0; t < 5000; t += 100) b.update(10, 4050, 7200, 6600, 200, t);  // 405 mV: no battery
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryMonitor::Level::kUsb), static_cast<int>(b.level()));
}

// LLD §10.6: low 7.2 V / critical 6.6 V; leaving a level needs its threshold + batt_hyst (200 mV), 2 s.
void test_battery_hysteresis_on_recovery() {
  BatteryMonitor b;
  uint32_t t = 0;
  auto feed = [&](uint16_t adc, uint32_t ms) {
    for (const uint32_t end = t + ms; t < end; t += 100) b.update(adc, 4050, 7200, 6600, 200, t);
  };
  feed(160, 3000);  // 6480 mV
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryMonitor::Level::kCritical), static_cast<int>(b.level()));
  feed(166, 3000);  // 6723 mV: above critical but not by 200 mV
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryMonitor::Level::kCritical), static_cast<int>(b.level()));
  feed(172, 3000);  // 6966 mV > 6800: back to low
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryMonitor::Level::kLow), static_cast<int>(b.level()));
  feed(180, 3000);  // 7290 mV: above low but not by 200 mV
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryMonitor::Level::kLow), static_cast<int>(b.level()));
  feed(184, 3000);  // 7452 mV > 7400: ok
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryMonitor::Level::kOk), static_cast<int>(b.level()));
  feed(177, 3000);  // 7168 mV < 7200: low again (no hysteresis on the way down)
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryMonitor::Level::kLow), static_cast<int>(b.level()));
}

void test_edge_detector_falling_and_rising() {
  // Mega IR sampler (LLD §11.4): falling/rising edges between two port samples.
  cq::EdgeDetector d;  // starts with all lines high (idle IR level)
  TEST_ASSERT_EQUAL_HEX8(0x00, d.update(0xFF).falling);
  const cq::EdgeDetector::Edges e = d.update(0xBF);  // bit 6 went low
  TEST_ASSERT_EQUAL_HEX8(0x40, e.falling);
  TEST_ASSERT_EQUAL_HEX8(0x00, e.rising);
  TEST_ASSERT_EQUAL_HEX8(0x00, d.update(0xBF).falling);  // still low: no new edge
  TEST_ASSERT_EQUAL_HEX8(0x40, d.update(0xFF).rising);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ultrasonic_distance_and_median);
  RUN_TEST(test_ultrasonic_offset_and_clear);
  RUN_TEST(test_ultrasonic_closing_speed);
  RUN_TEST(test_ultrasonic_fault_after_three_no_echo_and_recovery);
  RUN_TEST(test_ultrasonic_open_space_and_clone_hold_are_not_faults);
  RUN_TEST(test_ultrasonic_stuck_high_echo_faults);
  RUN_TEST(test_nec_decodes_frame_and_repeat);
  RUN_TEST(test_nec_rejects_bad_inverse_and_noise);
  RUN_TEST(test_battery_levels_with_persistence);
  RUN_TEST(test_battery_four_sample_average);
  RUN_TEST(test_battery_usb_power_is_not_an_alarm);
  RUN_TEST(test_battery_hysteresis_on_recovery);
  RUN_TEST(test_edge_detector_falling_and_rising);
  RUN_TEST(test_ultrasonic_classify_slow_clone_and_no_answer);
  return UNITY_END();
}

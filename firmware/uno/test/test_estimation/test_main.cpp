#include <unity.h>

#include "domain/estimation/accel_variance.h"
#include "domain/estimation/attitude.h"
#include "domain/estimation/bump_detector.h"
#include "domain/estimation/gyro_zero_tracker.h"
#include "domain/estimation/imu_scale.h"
#include "domain/estimation/lift_detector.h"
#include "domain/estimation/yaw_integrator.h"

using namespace cq;

void setUp() {}
void tearDown() {}

void test_attitude_level_stays_zero() {
  Attitude a;
  a.reset(0, 0);
  for (int i = 0; i < 200; ++i) a.update(0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.005f);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.0f, a.pitchDeg());
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.0f, a.rollDeg());
}

void test_attitude_converges_to_accel_tilt() {
  Attitude a;
  a.reset(0, 0);
  // nose up 20 deg: the accelerometer reads specific force (+1 g up when level), so it has a
  // positive x component
  const float s = 0.3420f, c = 0.9397f;
  for (int i = 0; i < 1000; ++i) a.update(s, 0.0f, c, 0.0f, 0.0f, 0.005f);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 20.0f, a.pitchDeg());
}

// LLD §9.13: gyro integration at 200 Hz (every call), atan2/sqrt correction at 50 Hz (every 4th).
void test_attitude_accel_correction_every_fourth_call() {
  Attitude a;
  a.reset(0, 0);
  const float s = 0.3420f, c = 0.9397f;  // accel says nose up 20 deg
  for (int i = 0; i < 3; ++i) a.update(s, 0.0f, c, 0.0f, 0.0f, 0.005f);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, a.pitchDeg());  // calls 1-3: gyro only
  a.update(s, 0.0f, c, 0.0f, 0.0f, 0.005f);             // call 4: accel blend
  TEST_ASSERT_FLOAT_WITHIN(0.01f, (1.0f - Attitude::kAlpha) * 20.0f, a.pitchDeg());
  a.reset(0, 0);
  a.update(0.0f, 0.0f, 1.0f, 0.0f, -10.0f, 0.005f);  // gy -10 dps = nose up, integrated at once
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.05f, a.pitchDeg());
}

void test_yaw_integrates_and_wraps() {
  YawIntegrator y;
  for (int i = 0; i < 200; ++i) y.update(90.0f, 0.005f, false);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 90.0f, y.yawDeg());
  for (int i = 0; i < 400; ++i) y.update(-90.0f, 0.005f, false);  // -180 → net -90
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 270.0f, y.yawDeg());
  TEST_ASSERT_FLOAT_WITHIN(0.5f, -90.0f, y.totalDeg());
}

void test_yaw_ignores_small_rate_when_stationary() {
  YawIntegrator y;
  for (int i = 0; i < 2000; ++i) y.update(0.3f, 0.005f, true);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, y.yawDeg());
  for (int i = 0; i < 200; ++i) y.update(0.3f, 0.005f, false);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.3f, y.yawDeg());
}

void test_bump_detects_front_hit_once() {
  BumpDetector b;
  int16_t mg = 0, dir = 0;
  for (uint32_t t = 0; t < 100; t += 5) TEST_ASSERT_FALSE(b.update(0.0f, 0.0f, 1500, t, mg, dir));
  TEST_ASSERT_TRUE(b.update(-2.0f, 0.0f, 1500, 100, mg, dir));  // deceleration = hit at the front
  TEST_ASSERT_EQUAL(0, dir);
  TEST_ASSERT_TRUE(mg >= 1900);
  TEST_ASSERT_FALSE(b.update(-2.0f, 0.0f, 1500, 200, mg, dir));  // refractory 500 ms
  TEST_ASSERT_TRUE(b.update(2.0f, 0.0f, 1500, 700, mg, dir));
  TEST_ASSERT_EQUAL(1, dir);
}

void test_lift_and_land() {
  LiftDetector l;
  uint32_t t = 0;
  for (; t < 200; t += 5) TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kNone),
                                            static_cast<int>(l.update(1.0f, 0, 0, 45, t)));
  LiftDetector::Change c = LiftDetector::Change::kNone;
  for (; t < 300; t += 5) {
    const LiftDetector::Change x = l.update(0.4f, 0, 0, 45, t);
    if (x != LiftDetector::Change::kNone) c = x;
  }
  TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kLifted), static_cast<int>(c));
  TEST_ASSERT_TRUE(l.lifted());
  c = LiftDetector::Change::kNone;
  for (; t < 1000; t += 5) {
    const LiftDetector::Change x = l.update(1.0f, 0, 0, 45, t);
    if (x != LiftDetector::Change::kNone) c = x;
  }
  TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kLanded), static_cast<int>(c));
  TEST_ASSERT_FALSE(l.lifted());
}

void test_tilt_needs_300ms_hold() {
  LiftDetector l;
  // Over the limit for 295 ms only (a bump / turn artefact), then back level: never tilted.
  for (uint32_t t = 0; t < 300; t += 5)
    TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kNone),
                      static_cast<int>(l.update(1.0f, 46.0f, 0, 45, t)));
  TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kNone),
                    static_cast<int>(l.update(1.0f, 20.0f, 0, 45, 300)));
  TEST_ASSERT_FALSE(l.tilted());
  // The hold restarts: a fresh excursion needs the full 300 ms again.
  for (uint32_t t = 400; t < 700; t += 5)
    TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kNone),
                      static_cast<int>(l.update(1.0f, 46.0f, 0, 45, t)));
  TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kTilted),
                    static_cast<int>(l.update(1.0f, 46.0f, 0, 45, 700)));  // 300 ms since 400
  TEST_ASSERT_TRUE(l.tilted());
}

void test_tilt_hysteresis() {
  LiftDetector l;
  for (uint32_t t = 0; t < 300; t += 5) l.update(1.0f, 46.0f, 0, 45, t);
  TEST_ASSERT_FALSE(l.tilted());
  TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kTilted),
                    static_cast<int>(l.update(1.0f, 46.0f, 0, 45, 300)));
  TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kNone),
                    static_cast<int>(l.update(1.0f, 42.0f, 0, 45, 305)));  // inside hysteresis
  TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kLevel),
                    static_cast<int>(l.update(1.0f, 39.0f, 0, 45, 310)));  // release is immediate (no hold)
}

void test_accel_variance_window() {
  AccelVariance v;
  for (uint32_t t = 0; t <= 1000; t += 5) v.add((t / 5) % 2 ? 1.1f : 0.9f, t);
  TEST_ASSERT_UINT16_WITHIN(1000, 10000, v.value());  // ±100 mg → 10000 mg²
}

void test_imu_id_accepts_mpu6050_family() {
  TEST_ASSERT_TRUE(cq::imu_scale::supportedWhoAmI(0x68));   // MPU-6050
  TEST_ASSERT_TRUE(cq::imu_scale::supportedWhoAmI(0x70));   // MPU-6500 (common clone)
  TEST_ASSERT_TRUE(cq::imu_scale::supportedWhoAmI(0x71));   // MPU-9250
  TEST_ASSERT_FALSE(cq::imu_scale::supportedWhoAmI(0x00));  // bus floating low
  TEST_ASSERT_FALSE(cq::imu_scale::supportedWhoAmI(0xFF));  // bus floating high
}

// ---- GyroZeroTracker (LLD §11.8): IMU samples every 5 ms, raw gyro units (65.5 per deg/s) ----
namespace {
const int16_t kZero3[3] = {0, 0, 0};
// Feeds n samples of (x, y, z) with +-3 raw alternating noise; returns the next sample time.
uint32_t feed(GyroZeroTracker& g, int16_t x, int16_t y, int16_t z, int n, uint32_t t, bool quiet = true) {
  for (int i = 0; i < n; ++i, t += 5) {
    const int16_t d = (i & 1) ? 3 : -3;
    const int16_t raw[3] = {static_cast<int16_t>(x + d), static_cast<int16_t>(y - d), static_cast<int16_t>(z + d)};
    g.update(raw, quiet, t);
  }
  return t;
}
}  // namespace

void test_gyro_zero_starts_from_stored() {
  GyroZeroTracker g;
  const int16_t stored[3] = {-86, -57, -17};
  g.seed(stored);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -86.0f, g.zero(0));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -17.0f, g.zero(2));
  TEST_ASSERT_EQUAL(0, g.accepted());
}

// 1 s settle (200 samples) + two agreeing 2 s windows (400 each) → the zero is learned.
void test_gyro_zero_learns_after_two_agreeing_windows() {
  GyroZeroTracker g;
  g.seed(kZero3);
  uint32_t t = feed(g, 5, -3, -13, 200 + 400, 0);
  TEST_ASSERT_EQUAL(0, g.accepted());  // one window is not enough
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, g.zero(2));
  feed(g, 5, -3, -13, 400, t);
  TEST_ASSERT_EQUAL(1, g.accepted());
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 5.0f, g.zero(0));
  TEST_ASSERT_FLOAT_WITHIN(0.5f, -3.0f, g.zero(1));
  TEST_ASSERT_FLOAT_WITHIN(0.5f, -13.0f, g.zero(2));
}

// A bump (spread > 1.5 deg/s) makes the window invalid and breaks the pair.
void test_gyro_zero_ignores_a_bump() {
  GyroZeroTracker g;
  g.seed(kZero3);
  uint32_t t = feed(g, 0, 0, -13, 200 + 200, 0);
  t = feed(g, 0, 0, 200, 1, t);  // knock
  t = feed(g, 0, 0, -13, 199, t);  // end of the bumped window
  feed(g, 0, 0, -13, 400, t);      // one clean window: no pair yet
  TEST_ASSERT_EQUAL(0, g.accepted());
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, g.zero(2));
}

// Motion during a window drops it; measuring needs a new 1 s settle.
void test_gyro_zero_motion_restarts_settle() {
  GyroZeroTracker g;
  g.seed(kZero3);
  uint32_t t = feed(g, 0, 0, -13, 200 + 200, 0);
  t = feed(g, 0, 0, -13, 1, t, false);  // driven
  feed(g, 0, 0, -13, 800, t);           // settle 200 + window 400 + 200 of the next
  TEST_ASSERT_EQUAL(0, g.accepted());
}

// A slow, smooth hand rotation looks steady within a window but not between windows: not learned.
void test_gyro_zero_disagreeing_windows_not_learned() {
  GyroZeroTracker g;
  g.seed(kZero3);
  uint32_t t = feed(g, 0, 0, 100, 200 + 400, 0);
  t = feed(g, 0, 0, 140, 400, t);
  feed(g, 0, 0, 180, 400, t);
  TEST_ASSERT_EQUAL(0, g.accepted());
}

// Beyond the datasheet limit (20 deg/s = 1310 raw) the sensor is faulty or turning: not learned.
void test_gyro_zero_rejects_out_of_range() {
  GyroZeroTracker g;
  g.seed(kZero3);
  feed(g, 0, 0, 1400, 200 + 800, 0);
  TEST_ASSERT_EQUAL(0, g.accepted());
}

// applySettings() runs after every setting change: seeding the same stored zero keeps what was learned;
// a new stored zero (manual calibration) replaces it.
void test_gyro_zero_reseed_only_on_change() {
  GyroZeroTracker g;
  g.seed(kZero3);
  feed(g, 0, 0, -13, 200 + 800, 0);
  TEST_ASSERT_EQUAL(1, g.accepted());
  g.seed(kZero3);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, -13.0f, g.zero(2));
  const int16_t cal[3] = {1, 2, -15};
  g.seed(cal);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -15.0f, g.zero(2));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_attitude_level_stays_zero);
  RUN_TEST(test_attitude_converges_to_accel_tilt);
  RUN_TEST(test_attitude_accel_correction_every_fourth_call);
  RUN_TEST(test_yaw_integrates_and_wraps);
  RUN_TEST(test_yaw_ignores_small_rate_when_stationary);
  RUN_TEST(test_bump_detects_front_hit_once);
  RUN_TEST(test_lift_and_land);
  RUN_TEST(test_tilt_needs_300ms_hold);
  RUN_TEST(test_tilt_hysteresis);
  RUN_TEST(test_accel_variance_window);
  RUN_TEST(test_imu_id_accepts_mpu6050_family);
  RUN_TEST(test_gyro_zero_starts_from_stored);
  RUN_TEST(test_gyro_zero_learns_after_two_agreeing_windows);
  RUN_TEST(test_gyro_zero_ignores_a_bump);
  RUN_TEST(test_gyro_zero_motion_restarts_settle);
  RUN_TEST(test_gyro_zero_disagreeing_windows_not_learned);
  RUN_TEST(test_gyro_zero_rejects_out_of_range);
  RUN_TEST(test_gyro_zero_reseed_only_on_change);
  return UNITY_END();
}

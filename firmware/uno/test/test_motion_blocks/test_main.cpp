#include <math.h>
#include <stdio.h>
#include <unity.h>

#include "domain/motion/battery_comp.h"
#include "domain/motion/deadband.h"
#include "domain/motion/mixer.h"
#include "domain/motion/ramp.h"
#include "domain/motion/target_limiter.h"
#include "domain/util/pid.h"

using namespace cq;

void setUp() {}
void tearDown() {}

void test_pid_p_only_and_limits() {
  Pid p;
  p.setGains({2.0f, 0.0f, 0.0f});
  p.setLimits(-10.0f, 10.0f);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 6.0f, p.update(3.0f, 0.01f, false));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f, p.update(30.0f, 0.01f, false));
}

void test_pid_integrates_and_freezes() {
  Pid p;
  p.setGains({0.0f, 1.0f, 0.0f});
  p.setLimits(-100.0f, 100.0f);
  for (int i = 0; i < 100; ++i) p.update(1.0f, 0.01f, false);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, p.integrator());
  for (int i = 0; i < 100; ++i) p.update(1.0f, 0.01f, true);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, p.integrator());
}

void test_pid_anti_windup_at_saturation() {
  Pid p;
  p.setGains({1.0f, 10.0f, 0.0f});
  p.setLimits(-5.0f, 5.0f);
  for (int i = 0; i < 1000; ++i) p.update(10.0f, 0.01f, false);  // saturated the whole time
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, p.integrator());  // P alone saturates: conditional integration adds nothing
  const float out = p.update(-1.0f, 0.01f, false);  // error reverses → output leaves the limit at once
  TEST_ASSERT_TRUE(out < 5.0f);
}

void test_ramp_accel_and_faster_decel() {
  Ramp r;
  for (int i = 0; i < 10; ++i) r.step(100.0f, 0.01f, 200.0f, 400.0f);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 20.0f, r.value());
  r.step(0.0f, 0.01f, 200.0f, 400.0f);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 16.0f, r.value());
  for (int i = 0; i < 100; ++i) r.step(-50.0f, 0.01f, 200.0f, 400.0f);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -50.0f, r.value());
}

void test_deadband_kick_then_keep() {
  TEST_ASSERT_EQUAL(0, deadbandPwm(0.2f, false, 70, 55));
  TEST_ASSERT_INT_WITHIN(1, 70, deadbandPwm(0.5f, false, 70, 55));  // at rest: start value
  TEST_ASSERT_INT_WITHIN(1, 55, deadbandPwm(0.5f, true, 70, 55));   // moving: keep value
  TEST_ASSERT_EQUAL(255, deadbandPwm(100.0f, true, 70, 55));
  TEST_ASSERT_EQUAL(-155, deadbandPwm(-50.0f, true, 70, 55));
}

void test_battery_scale() {
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.75f, batteryScale(8000, 6000));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, batteryScale(5500, 6000));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, batteryScale(500, 6000));  // USB powered: no battery
}

void test_mixer_turn_authority() {
  const MixerConfig cfg{40, false, 0.0f};
  float l = 0, r = 0;
  mix(0.0f, 50.0f, 0.0f, cfg, l, r);  // spin CCW
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -50.0f, l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 50.0f, r);
  mix(100.0f, 50.0f, 0.0f, cfg, l, r);  // spec 02 §A4.1: angle mix, authority no longer scales the stick
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 100.0f, r);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 57.75f, l);
}

// design-notes §41 (2026-10-14): the turn authority (less steering at speed) applies to the DRIVER's
// steering only. The gyro's heading correction is not scaled - scaling halved it at 80 % speed.
void test_mixer_gyro_correction_not_scaled_by_authority() {
  const MixerConfig cfg{40, false, 0.0f};
  float l = 0, r = 0;
  mix(80.0f, 0.0f, 10.0f, cfg, l, r);  // gyro correction 10 % at 80 % speed: full 10
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 70.0f, l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.0f, r);
  mix(80.0f, 10.0f, 0.0f, cfg, l, r);  // driver steering 10 % at 80 %: angle mix (§A4.1)
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 69.27f, l);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 80.62f, r);  // outer = stick distance |(80, 10)|
}

// design-notes §39: w into the mixer is a physical turn (CCW +) - also in reverse. A gyro heading
// correction flipped in reverse became positive feedback and spun the tank.
void test_mixer_never_flips_w_in_reverse() {
  const MixerConfig cfg{40, false, 0.0f};
  float l = 0, r = 0;
  mix(-50.0f, 20.0f, 0.0f, cfg, l, r);
  TEST_ASSERT_TRUE(r > l);  // CCW stays CCW
}

// Car-style steering (reverse_steer 0) flips only the DRIVER's teleop steering when reversing.
void test_car_steer_flips_only_driver_input_in_reverse() {
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -20.0f, carSteer(-50.0f, 20.0f, true));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 20.0f, carSteer(50.0f, 20.0f, true));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 20.0f, carSteer(-50.0f, 20.0f, false));  // tank style
}

void test_mixer_pivot_fallback() {
  const MixerConfig pivot{100, true, 20.0f};  // full turn authority, keep threshold 20 %
  float l = 0, r = 0;
  mix(100.0f, 30.0f, 0.0f, pivot, l, r);  // angle mix (§A4.1): inner 70.75 above keep, unchanged
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 70.75f, l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 100.0f, r);
  mix(30.0f, 25.0f, 0.0f, pivot, l, r);  // inner 16.6 % < keep 20 % → 0: pivot on the left track
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, l);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 39.05f, r);
  mix(0.0f, 10.0f, 0.0f, pivot, l, r);  // spin in place has no inner track
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -10.0f, l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 10.0f, r);
  const MixerConfig off{100, false, 20.0f};
  mix(30.0f, 25.0f, 0.0f, off, l, r);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 16.55f, l);
}

// Spec 02 §A4.1 (owner 2026-10-15): stick steering by angle; pure axes and corrections keep the old mix.
void test_mixer_angle_45_forward_arc_and_reverse() {
  const MixerConfig cfg{40, false, 0.0f};
  float l = 0, r = 0;
  mix(70.71f, 70.71f, 0.0f, cfg, l, r);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 36.63f, l);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, r);
  mix(-70.71f, 70.71f, 0.0f, cfg, l, r);  // reversing: both tracks backwards
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -100.0f, l);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -36.63f, r);
}

// Review M23: driver steering cancelled exactly by a correction at standstill must give 0/0, never NaN.
void test_mixer_angle_zero_input_is_stop() {
  const MixerConfig cfg{40, false, 0.0f};
  float l = 1, r = 1;
  mix(0.0f, 10.0f, -10.0f, cfg, l, r);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, r);
}

// Deferred minor (bridge LLD R3.11): outer track = the stick distance, never above MAX (old "big + 0.375 small"
// was ~7 % over near 20°). Full stick at MAX 60 %, every angle 1..90°.
void test_mixer_angle_never_over_max() {
  const MixerConfig cfg{40, false, 0.0f};
  for (int deg = 1; deg <= 90; ++deg) {
    const float th = static_cast<float>(deg) * 3.14159265f / 180.0f;
    float l = 0, r = 0;
    mix(60.0f * cosf(th), 60.0f * sinf(th), 0.0f, cfg, l, r);
    char msg[16];
    snprintf(msg, sizeof(msg), "deg %d", deg);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 60.0f, l > r ? l : r, msg);
  }
}

void test_mixer_ir_axes_and_correction_unchanged() {
  const MixerConfig cfg{40, true, 20.0f};
  float l = 0, r = 0;
  mix(80.0f, 0.0f, 0.0f, cfg, l, r);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 80.0f, l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 80.0f, r);
  mix(0.0f, -80.0f, 0.0f, cfg, l, r);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 80.0f, l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -80.0f, r);
  mix(100.0f, 0.0f, 30.0f, cfg, l, r);  // gyro / primitive output: arcade mix with turning priority
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 40.0f, l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 100.0f, r);
}

void test_envelope_caps_targets() {
  const Limits child{20, 0xFFFF, 15, 50, true, 100};
  Limited o = limitTargets(100.0f, 100.0f, child);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 20.0f, o.l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 20.0f, o.r);
  TEST_ASSERT_TRUE(o.capped);
  o = limitTargets(100.0f, -50.0f, child);  // ratio kept: the arc shape does not change
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 20.0f, o.l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -10.0f, o.r);
  o = limitTargets(10.0f, 15.0f, child);
  TEST_ASSERT_FALSE(o.capped);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 15.0f, o.r);
}

void test_obstacle_slow_zone_scales_forward_only() {
  const Limits l{100, 325, 15, 50, true, 100};  // halfway between 15 and 50 cm
  Limited o = limitTargets(80.0f, 80.0f, l);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 40.0f, o.l);
  TEST_ASSERT_TRUE(o.slowed);
  o = limitTargets(-80.0f, -80.0f, l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -80.0f, o.l);
  const Limits off{100, 325, 15, 50, false, 100};
  o = limitTargets(80.0f, 80.0f, off);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 80.0f, o.l);
}

void test_degraded_caps() {
  const Limits us_fault{100, 0xFFFE, 15, 50, true, 50};  // brake unavailable → 50 % (LLD §9.16)
  Limited o = limitTargets(80.0f, 80.0f, us_fault);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 50.0f, o.l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 50.0f, o.r);
  TEST_ASSERT_TRUE(o.capped);
  TEST_ASSERT_FALSE(o.slowed);
  const Limits imu_fault{100, 0xFFFF, 15, 50, true, 30};  // IMU fault → 30 %
  o = limitTargets(-60.0f, 60.0f, imu_fault);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -30.0f, o.l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 30.0f, o.r);
  const Limits child_and_fault{20, 0xFFFF, 15, 50, true, 30};  // the tighter cap wins
  o = limitTargets(100.0f, 100.0f, child_and_fault);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 20.0f, o.l);
  const Limits healthy{100, 0xFFFF, 15, 50, true, 100};
  o = limitTargets(80.0f, -40.0f, healthy);
  TEST_ASSERT_FALSE(o.capped);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 80.0f, o.l);
}

void test_scale_pwm_caps_every_source() {
  // Final review Important #2: the 6 V cap applies to calibration PWM too (full 8.4 V pack).
  const float s = cq::batteryScale(8400, 6000);
  TEST_ASSERT_EQUAL(182, cq::scalePwm(255, s));
  TEST_ASSERT_EQUAL(-182, cq::scalePwm(-255, s));
  TEST_ASSERT_EQUAL(120, cq::scalePwm(120, cq::batteryScale(0, 6000)));  // USB only: unscaled
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_pid_p_only_and_limits);
  RUN_TEST(test_pid_integrates_and_freezes);
  RUN_TEST(test_pid_anti_windup_at_saturation);
  RUN_TEST(test_ramp_accel_and_faster_decel);
  RUN_TEST(test_deadband_kick_then_keep);
  RUN_TEST(test_battery_scale);
  RUN_TEST(test_mixer_turn_authority);
  RUN_TEST(test_mixer_gyro_correction_not_scaled_by_authority);
  RUN_TEST(test_mixer_never_flips_w_in_reverse);
  RUN_TEST(test_car_steer_flips_only_driver_input_in_reverse);
  RUN_TEST(test_mixer_pivot_fallback);
  RUN_TEST(test_mixer_angle_45_forward_arc_and_reverse);
  RUN_TEST(test_mixer_ir_axes_and_correction_unchanged);
  RUN_TEST(test_mixer_angle_zero_input_is_stop);
  RUN_TEST(test_mixer_angle_never_over_max);
  RUN_TEST(test_envelope_caps_targets);
  RUN_TEST(test_obstacle_slow_zone_scales_forward_only);
  RUN_TEST(test_degraded_caps);
  RUN_TEST(test_scale_pwm_caps_every_source);
  return UNITY_END();
}

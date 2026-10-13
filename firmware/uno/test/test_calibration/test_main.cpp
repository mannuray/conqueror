#include <unity.h>

#include "domain/calibration/cal_script_runner.h"
#include "domain/calibration/cal_status.h"
#include "domain/selftest/self_test.h"
#include "domain/settings/settings.h"

using namespace cq;

void setUp() {}
void tearDown() {}

namespace {
CalInputs still() {
  CalInputs in{};
  in.gz = 12;
  in.az = 8192;
  in.batt_mv = 7800;
  return in;
}
}  // namespace

void test_still_sample_streams_50hz_then_ok() {
  CalScriptRunner r;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk),
                    static_cast<int>(r.start(proto::CalScript::kStillSample, 1000, false, 0)));
  int samples = 0;
  CalOutputs out{};
  for (uint32_t t = 0; t <= 1100 && r.running(); t += 10) {
    r.tick(t, still(), out);
    TEST_ASSERT_EQUAL(0, out.pwm_l);
    if (out.sample) {
      ++samples;
      TEST_ASSERT_EQUAL(12, out.data.v[2]);
      TEST_ASSERT_EQUAL(8192, out.data.v[5]);
    }
  }
  TEST_ASSERT_INT_WITHIN(1, 50, samples);
  TEST_ASSERT_TRUE(out.finished);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(out.status));
  TEST_ASSERT_EQUAL(100, out.progress);
}

void test_pwm_ramp_up_then_down_on_selected_track() {
  CalScriptRunner r;
  const int32_t param = 0 | (100 << 8);  // track 0 = left, forward, max 100
  r.start(proto::CalScript::kPwmRamp, param, false, 0);
  CalOutputs out{};
  int16_t max_seen = 0;
  bool up = false, down = false;
  for (uint32_t t = 0; t < 10000 && r.running(); t += 10) {
    r.tick(t, still(), out);
    TEST_ASSERT_EQUAL(0, out.pwm_r);
    if (out.pwm_l > max_seen) max_seen = out.pwm_l;
    if (out.sample) {
      TEST_ASSERT_EQUAL(out.pwm_l, out.data.v[0]);  // v[0] = |pwm| of the selected track
      up = up || out.data.phase == 0;
      down = down || out.data.phase == 1;
    }
  }
  TEST_ASSERT_EQUAL(100, max_seen);
  TEST_ASSERT_TRUE(up && down);  // phase 0 ramp up, phase 1 ramp down
  TEST_ASSERT_EQUAL(0, out.pwm_l);
  TEST_ASSERT_TRUE(out.finished);
  TEST_ASSERT_EQUAL(100, out.progress);
}

void test_pwm_ramp_reverse_bit() {
  CalScriptRunner r;
  r.start(proto::CalScript::kPwmRamp, 0x01 | 0x02 | (50 << 8), false, 0);  // right, reverse, max 50
  CalOutputs out{};
  int16_t min_seen = 0;
  int16_t max_v0 = 0;
  for (uint32_t t = 0; t < 10000 && r.running(); t += 10) {
    r.tick(t, still(), out);
    TEST_ASSERT_EQUAL(0, out.pwm_l);
    if (out.pwm_r < min_seen) min_seen = out.pwm_r;
    if (out.sample && out.data.v[0] > max_v0) max_v0 = out.data.v[0];
  }
  TEST_ASSERT_EQUAL(-50, min_seen);
  TEST_ASSERT_EQUAL(50, max_v0);  // magnitude on the wire
}

void test_full_spin_needs_encoders_and_stops_at_720() {
  CalScriptRunner r;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kCalPrecondition),
                    static_cast<int>(r.start(proto::CalScript::kFullSpin, 150, false, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk),
                    static_cast<int>(r.start(proto::CalScript::kFullSpin, 150, true, 0)));
  CalInputs in = still();
  CalOutputs out{};
  uint32_t t = 0;
  for (; t < 20000 && r.running(); t += 10) {
    in.yaw_total_deg = t * 0.09f;  // 90 °/s
    r.tick(t, in, out);
    if (r.running()) {
      TEST_ASSERT_EQUAL(-150, out.pwm_l);
      TEST_ASSERT_EQUAL(150, out.pwm_r);
    }
    if (out.sample) {
      TEST_ASSERT_EQUAL(-150, out.data.v[0]);  // pwm_l pwm_r cps_l cps_r yaw_rate×10 …
      TEST_ASSERT_EQUAL(150, out.data.v[1]);
    }
  }
  TEST_ASSERT_UINT32_WITHIN(20, 8000, t);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(out.status));
}

void test_spin_and_arc_spins_360_then_arcs() {
  CalScriptRunner r;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kCalPrecondition),
                    static_cast<int>(r.start(proto::CalScript::kSpinAndArc, 0, false, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk),
                    static_cast<int>(r.start(proto::CalScript::kSpinAndArc, 0, true, 0)));
  CalInputs in = still();
  CalOutputs out{};
  float yaw = 0.0f;
  int16_t last0[3] = {0, 0, 0};
  int arc_samples = 0;
  bool arced = false;
  for (uint32_t t = 0; t < 20000 && r.running(); t += 10) {
    // toy plant: spin (l < 0 < r) turns at 90 °/s, a forward arc (0 < l < r) at 30 °/s
    in.yaw_rate_dps = out.pwm_r > out.pwm_l ? (out.pwm_l < 0 ? 90.0f : 30.0f) : 0.0f;
    yaw += in.yaw_rate_dps * 0.01f;
    in.yaw_total_deg = yaw;
    in.enc_l += out.pwm_l / 10;
    in.enc_r += out.pwm_r / 10;
    r.tick(t, in, out);
    if (!out.sample) continue;
    if (out.data.phase == 0) {
      for (uint8_t i = 0; i < 3; ++i) last0[i] = out.data.v[i];
    } else {
      ++arc_samples;
      arced = arced || (out.data.v[4] > 0 && out.data.v[5] > out.data.v[4]);  // pwm_l, pwm_r
    }
  }
  TEST_ASSERT_TRUE(last0[0] < 0 && last0[1] > 0);  // enc_l, enc_r since phase start (CCW spin)
  TEST_ASSERT_INT_WITHIN(20, 3600, last0[2]);     // yaw ×10 at the end of the spin
  TEST_ASSERT_TRUE(arced);
  TEST_ASSERT_TRUE(arc_samples > 10);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(out.status));
  TEST_ASSERT_EQUAL(0, out.pwm_l);
}

void test_rate_steps_one_phase_per_speed() {
  CalScriptRunner r;
  r.configure(CalConfig{2500, 180, 55});
  r.start(proto::CalScript::kRateSteps, 0, false, 0);  // 0 → 4 speed steps
  CalOutputs out{};
  uint8_t max_phase = 0;
  for (uint32_t t = 0; t < 10000 && r.running(); t += 10) {
    r.tick(t, still(), out);
    if (!out.sample) continue;
    if (out.data.phase > max_phase) max_phase = out.data.phase;
    TEST_ASSERT_EQUAL(1000, out.data.v[1]);  // full turn command, w % ×10
    if (out.data.v[0] == 0) TEST_ASSERT_TRUE(out.pwm_l == -255 && out.pwm_r == 255);  // spin in place
    if (out.data.v[0] == 100) TEST_ASSERT_TRUE(out.pwm_l == 0 && out.pwm_r == 255);   // pivot
  }
  TEST_ASSERT_EQUAL(3, max_phase);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(out.status));
}

void test_relay_oscillation_switches_on_measured_side() {
  CalScriptRunner r;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kCalPrecondition),
                    static_cast<int>(r.start(proto::CalScript::kRelayOscillation, 0, false, 0)));  // speed loop
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kInvalidCommand),
                    static_cast<int>(r.start(proto::CalScript::kRelayOscillation, 3, true, 0)));
  r.configure(CalConfig{2500, 180, 55});
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk),
                    static_cast<int>(r.start(proto::CalScript::kRelayOscillation, 1, false, 0)));  // rate loop
  CalInputs in = still();
  CalOutputs out{};
  in.yaw_rate_dps = 60.0f;  // below the 90 °/s setpoint → +d
  r.tick(0, in, out);
  TEST_ASSERT_TRUE(out.sample);
  TEST_ASSERT_EQUAL(900, out.data.v[0]);  // setpoint ×10
  TEST_ASSERT_EQUAL(600, out.data.v[1]);  // measured ×10
  TEST_ASSERT_EQUAL(20, out.data.v[2]);   // relay output (w %)
  TEST_ASSERT_TRUE(out.pwm_r > 0 && out.pwm_l == -out.pwm_r);
  const int16_t high = out.pwm_r;
  in.yaw_rate_dps = 120.0f;  // above → −d
  r.tick(10, in, out);
  r.tick(20, in, out);
  TEST_ASSERT_EQUAL(-20, out.data.v[2]);
  TEST_ASSERT_TRUE(out.pwm_r < high);
  uint32_t t = 30;
  for (; t < 12000 && r.running(); t += 10) r.tick(t, in, out);
  TEST_ASSERT_UINT32_WITHIN(20, 8000, t);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(out.status));
}

void test_drive_counts_corrects_heading() {
  CalScriptRunner r;
  r.start(proto::CalScript::kDriveCounts, 1000, true, 0);
  CalInputs in = still();
  CalOutputs out{};
  in.yaw_total_deg = 5.0f;  // drifted left (CCW) after start
  r.tick(0, still(), out);   // captures the start heading 0
  r.tick(10, in, out);
  TEST_ASSERT_TRUE(out.pwm_l > out.pwm_r);  // speed up left to turn back right
  in.enc_l = 1000;
  in.enc_r = 1000;
  r.tick(20, in, out);
  TEST_ASSERT_TRUE(out.finished);
  TEST_ASSERT_EQUAL(0, out.pwm_l);
}

void test_cancel_and_unavailable() {
  CalScriptRunner r;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kUnavailable),
                    static_cast<int>(r.start(proto::CalScript::kServoNudge, 0, true, 0)));  // reserved
  r.start(proto::CalScript::kIdle, 5000, false, 0);
  r.cancel();
  CalOutputs out{};
  r.tick(10, still(), out);
  TEST_ASSERT_FALSE(r.running());
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kCalCancelled), static_cast<int>(out.status));
}

void test_cal_status_bits() {
  Settings s;
  s.loadDefaults();
  uint8_t st[3];
  calStatus(s.global(), s.profile(), st);
  TEST_ASSERT_EQUAL(0, st[0] | st[1] | st[2]);
  s.set(static_cast<uint8_t>(proto::SettingId::kGyroBiasZ), -15);
  s.set(static_cast<uint8_t>(proto::SettingId::kCountsPerCm), 7400);
  calStatus(s.global(), s.profile(), st);
  TEST_ASSERT_EQUAL_HEX8(0x01, st[0] & 0x03);         // item 0 gyro bias ok
  TEST_ASSERT_EQUAL_HEX8(0x01, (st[1] >> 0) & 0x03);  // item 4 counts_per_cm ok
}

void test_self_test_mask() {
  SelfTest t;
  t.report(SelfTest::Check::kImu, true);
  t.report(SelfTest::Check::kUltrasonic, false);
  TEST_ASSERT_EQUAL_HEX8(0x02, t.failMask());
  TEST_ASSERT_FALSE(t.critical());
  t.report(SelfTest::Check::kImu, false);
  TEST_ASSERT_TRUE(t.critical());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_still_sample_streams_50hz_then_ok);
  RUN_TEST(test_pwm_ramp_up_then_down_on_selected_track);
  RUN_TEST(test_pwm_ramp_reverse_bit);
  RUN_TEST(test_full_spin_needs_encoders_and_stops_at_720);
  RUN_TEST(test_spin_and_arc_spins_360_then_arcs);
  RUN_TEST(test_rate_steps_one_phase_per_speed);
  RUN_TEST(test_relay_oscillation_switches_on_measured_side);
  RUN_TEST(test_drive_counts_corrects_heading);
  RUN_TEST(test_cancel_and_unavailable);
  RUN_TEST(test_cal_status_bits);
  RUN_TEST(test_self_test_mask);
  return UNITY_END();
}

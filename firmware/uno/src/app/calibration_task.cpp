#include "app/app.h"
#include "app/motion_api.h"
#include "app/tasks.h"
#include "domain/calibration/cal_status.h"
#include "domain/estimation/imu_scale.h"
#include "domain/util/math_util.h"
#include "ports/clock.h"
#include "ports/encoders.h"

namespace cq {
namespace app {
namespace {

void sendResult(proto::CalScript script, uint8_t stage, uint8_t progress, proto::AckStatus status) {
  g_app.cal_result.script = static_cast<uint8_t>(script);
  g_app.cal_result.stage = stage;
  g_app.cal_result.progress = progress;
  g_app.cal_result.status = static_cast<uint8_t>(status);
  g_app.cal_result_pending = true;
}

int8_t dirOf(int16_t pwm, int8_t prev) { return pwm > 0 ? 1 : (pwm < 0 ? -1 : prev); }
int8_t g_dir_l = 1;
int8_t g_dir_r = 1;
uint32_t g_last_progress_ms = 0;
uint8_t g_last_progress = 0;  // reported by calibrationAbort()

}  // namespace

void encodersTick() {
  EncoderSnapshot s{};
  port::encoders::snapshot(s);
  g_dir_l = dirOf(g_app.out_pwm_l, g_dir_l);
  g_dir_r = dirOf(g_app.out_pwm_r, g_dir_r);
  g_app.enc_l.update(static_cast<uint32_t>(s.count_l), s.period_l_us, s.last_edge_l_us, s.t_us, g_dir_l);
  g_app.enc_r.update(static_cast<uint32_t>(s.count_r), s.period_r_us, s.last_edge_r_us, s.t_us, g_dir_r);

  const ProfileSettings& ps = g_app.settings.profile();
  if (g_app.encoders_enabled) {
    // keep values by direction: deadband_keep = {left fwd, left rev, right fwd, right rev}
    const EncoderHealth::Inputs hi{g_app.out_pwm_l, g_app.out_pwm_r, g_app.req_pwm_l, g_app.req_pwm_r,
                                   g_app.enc_l.recent(), g_app.enc_r.recent(), static_cast<int16_t>(g_app.yaw_rate_dps),
                                   ps.deadband_keep[g_app.out_pwm_l < 0 ? 1 : 0],
                                   ps.deadband_keep[g_app.out_pwm_r < 0 ? 3 : 2]};
    g_app.enc_health.update(hi, port::clock::nowMs());
    const uint8_t stalls = g_app.enc_health.takeNewStalls();
    const uint8_t faults = g_app.enc_health.takeNewFaults();
    for (uint8_t sd = 0; sd < 2; ++sd) {
      if (stalls & (1u << sd)) g_app.events.push(proto::EventKind::kStall, sd);
      if (faults & (1u << sd)) g_app.events.push(proto::EventKind::kEncoderFault, sd);
    }
  }
  proto::TelemFastMsg& f = g_app.fast;
  f.enc_l = g_app.enc_l.count();
  f.enc_r = g_app.enc_r.count();
  const bool usable = g_app.encoders_enabled;
  f.spd_act_l = usable ? static_cast<int8_t>(clampf(g_app.enc_l.cps() * 100.0f / ps.max_cps_l, -127, 127)) : -128;
  f.spd_act_r = usable ? static_cast<int8_t>(clampf(g_app.enc_r.cps() * 100.0f / ps.max_cps_r, -127, 127)) : -128;
}

proto::AckStatus onCalibrate(const uint8_t* p, uint8_t len) {
  proto::CalibrateMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  const proto::CalScript script = static_cast<proto::CalScript>(m.script);
  if (m.op == static_cast<uint8_t>(proto::CalOp::kCancel)) {
    if (!g_app.cal.running()) return proto::AckStatus::kOk;
    g_app.cal.cancel();
    return proto::AckStatus::kOk;
  }
  if (script == proto::CalScript::kSelfTest) {  // LLD §10.9: re-run the self-test (no motion; exempt from the lock)
    if (g_app.moving || g_app.cal.running() || g_app.self_test_pending || g_app.safety_override.active()) {
      return proto::AckStatus::kBusy;
    }
    g_app.self_test_pending = true;
    g_app.self_test_due_ms = port::clock::nowMs() + 1500;  // result: EVENT SELFTEST (+ sound) ≈ 1.5–2.5 s later
    return proto::AckStatus::kOk;
  }
  if (g_app.self_test.locked()) return proto::AckStatus::kBlocked;  // LLD §10.9: no calibration motion while locked
  if (m.profile != 0) return proto::AckStatus::kUnavailable;  // MVP: one profile
  if (g_app.cal.running()) return proto::AckStatus::kBusy;
  if (g_app.lift.lifted() || g_app.lift.tilted() || g_app.moving || !g_app.imu_ok || g_app.envelope.blocksCalibration()) {
    return proto::AckStatus::kCalPrecondition;  // LLD §9.7: refused while an envelope is active
  }
  const proto::AckStatus own = g_app.arbitrator.beginCalibration();
  if (own != proto::AckStatus::kOk) return own;
  g_app.teleop_v = 0;  // an old DRIVE never resumes after the script
  g_app.teleop_w = 0;
  const ProfileSettings& ps = g_app.settings.profile();
  const uint8_t keep = static_cast<uint8_t>(
      (ps.deadband_keep[0] + ps.deadband_keep[1] + ps.deadband_keep[2] + ps.deadband_keep[3]) / 4);
  g_app.cal.configure(CalConfig{static_cast<uint16_t>((ps.max_cps_l + ps.max_cps_r) / 2),
                                g_app.settings.global().turn_rate_max, keep});
  const proto::AckStatus st = g_app.cal.start(script, m.param, g_app.encoders_enabled, port::clock::nowMs());
  if (st != proto::AckStatus::kOk) {
    g_app.arbitrator.endCalibration();
    return st;
  }
  sendResult(script, 0, 0, proto::AckStatus::kOk);
  return st;
}

void calibrationAbort(proto::AckStatus reason) {
  if (!g_app.cal.running()) return;
  const proto::CalScript script = g_app.cal.script();
  g_app.cal.cancel();
  CalOutputs out{};
  const CalInputs in{};
  g_app.cal.tick(port::clock::nowMs(), in, out);
  g_app.arbitrator.endCalibration();
  sendResult(script, 0, g_last_progress, reason);  // progress reached before the abort
}

bool calibrationTick(int16_t& pwm_l, int16_t& pwm_r) {
  if (!g_app.cal.running()) return false;
  if (g_app.arbitrator.owner() != proto::Owner::kCalibration) {  // IR took over
    calibrationAbort(proto::AckStatus::kCalCancelled);
    return false;
  }
  const ImuRaw& r = g_app.imu_raw;
  const CalInputs in{r.gx, r.gy, r.gz, r.ax, r.ay, r.az, imu_scale::tempCentiC(r.temp), g_app.yaw.totalDeg(), g_app.yaw_rate_dps,
                     g_app.enc_l.count(), g_app.enc_r.count(), g_app.enc_l.cps(), g_app.enc_r.cps(), g_app.batt.mv()};
  CalOutputs out{};
  const uint32_t now = port::clock::nowMs();
  const proto::CalScript script = g_app.cal.script();
  g_app.cal.tick(now, in, out);
  g_last_progress = out.progress;
  if (out.sample) {
    g_app.cal_data = out.data;
    g_app.cal_data_pending = true;  // 50 Hz × 25 B fits easily in 250 kbaud next to telemetry
  }
  if (out.finished) {
    g_app.arbitrator.endCalibration();
    sendResult(script, out.stage, out.progress, out.status);
  } else if (elapsed(now, g_last_progress_ms, 500)) {
    g_last_progress_ms = now;
    sendResult(script, out.stage, out.progress, proto::AckStatus::kOk);
  }
  pwm_l = out.pwm_l;
  pwm_r = out.pwm_r;
  return !out.finished;
}

static void publishSelfTest() {
  g_app.events.push(proto::EventKind::kSelftest, g_app.self_test.failMask(), g_app.self_test.locked() ? 1 : 0);
  const uint32_t m = 1ul << static_cast<uint8_t>(proto::SlowFlagBit::kSelftestLocked);
  g_app.slow_flags = g_app.self_test.locked() ? (g_app.slow_flags | m) : (g_app.slow_flags & ~m);
}

void selfTestTask() {
  const uint32_t now = port::clock::nowMs();
  calStatus(g_app.settings.global(), g_app.settings.profile(), g_app.slow.cal_status);
  uint8_t hw = 0;
  if (g_app.encoders_enabled) hw |= 1u << static_cast<uint8_t>(proto::HwBit::kEncodersEnabled);
  if (g_app.imu_ok && g_app.imu_fail_streak == 0) hw |= 1u << static_cast<uint8_t>(proto::HwBit::kImuOk);
  if (!g_app.us.fault()) hw |= 1u << static_cast<uint8_t>(proto::HwBit::kUsOk);
  g_app.slow.hw = hw;
  if (!g_app.self_test_pending || static_cast<int32_t>(now - g_app.self_test_due_ms) < 0) return;
  g_app.self_test_pending = false;
  g_app.self_test.begin();
  g_app.self_test.report(SelfTest::Check::kImu, g_app.imu_ok);  // critical
  g_app.self_test.report(SelfTest::Check::kUltrasonic, !g_app.us.fault() && g_app.us.distMm() != UltrasonicFilter::kFault);
  g_app.self_test.report(SelfTest::Check::kBattery, SelfTest::batteryPlausible(g_app.batt.mv()));  // critical
  g_app.self_test.report(SelfTest::Check::kSettings, g_app.settings_reset_mask == 0);  // warning: defaults loaded
  g_app.self_test.complete();  // critical → motion locked (supervisor); pass → unlocked (LLD §10.9)
  publishSelfTest();
}

// Battery-only lock (e.g. booted on USB only): once the battery reads 5-9 V for 1 s the lock clears by itself,
// with the same event / flag / sound+LED (runs edge) as a passing re-run. IMU failures never take this path.
void selfTestRecoverTask() {
  if (g_app.self_test.tickBattery(SelfTest::batteryPlausible(g_app.batt.mv()), port::clock::nowMs())) publishSelfTest();
}

}  // namespace app
}  // namespace cq

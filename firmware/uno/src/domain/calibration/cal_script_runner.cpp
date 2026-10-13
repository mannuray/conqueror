#include "domain/calibration/cal_script_runner.h"

#include "domain/util/math_util.h"
#include "domain/motion/deadband.h"

namespace cq {
namespace {

constexpr uint16_t kRampStepMs = 150;
constexpr uint8_t kRampStep = 5;
constexpr uint16_t kRateStepMs = 1500;
constexpr uint16_t kRatePauseMs = 500;
constexpr uint32_t kSpinTimeoutMs = 20000;
constexpr uint32_t kDriveTimeoutMs = 15000;
constexpr int16_t kDrivePwm = 150;
constexpr float kDriveKp = 4.0f;  // PWM per degree of heading error
// SPIN_AND_ARC: gyro turn-rate control in both phases
constexpr float kSpinRateDps = 90.0f;
constexpr float kSpinFfPwm = 140.0f;
constexpr float kSpinKp = 1.5f;  // PWM per °/s of rate error
constexpr uint16_t kSettleMs = 500;
constexpr float kArcPwm = 150.0f;  // fixed v
constexpr float kArcRateDps = 30.0f;  // fixed ω
constexpr float kArcFfDiff = 40.0f;
constexpr float kArcKp = 1.5f;
constexpr float kArcDeg = 90.0f;
// RELAY_OSCILLATION: relay ±d around the setpoint, output in the PID's own output unit
constexpr uint32_t kRelayMs = 8000;
constexpr float kRelaySpeedPct = 50.0f;   // speed loop: % of max_cps, output % correction
constexpr float kRelaySpeedD = 15.0f;
constexpr float kRelayRateDps = 90.0f;    // rate loop: °/s, output w %
constexpr float kRelayRateD = 20.0f;
constexpr float kRelayHeadingD = 60.0f;   // heading loop: setpoint 0° (start heading), output °/s

int16_t sat16(float v) { return static_cast<int16_t>(clampf(v, -32767.0f, 32767.0f)); }
int16_t sat16(int32_t v) { return static_cast<int16_t>(v > 32767 ? 32767 : (v < -32767 ? -32767 : v)); }
int32_t absi(int32_t v) { return v < 0 ? -v : v; }
int16_t rampMax(int32_t param) {
  const uint8_t raw = static_cast<uint8_t>((param >> 8) & 0xFF);
  return raw == 0 ? 255 : raw;
}
uint8_t rateSteps(int32_t param) { return param <= 0 ? 4 : static_cast<uint8_t>(param < 2 ? 2 : (param > 5 ? 5 : param)); }

}  // namespace

proto::AckStatus CalScriptRunner::start(proto::CalScript script, int32_t param, bool encoders, uint32_t now_ms) {
  if (m_running) return proto::AckStatus::kBusy;
  switch (script) {
    case proto::CalScript::kStillSample:
    case proto::CalScript::kIdle:
      m_duration_ms = param <= 0 ? 3000u : static_cast<uint32_t>(param > 30000 ? 30000 : param);
      break;
    case proto::CalScript::kPwmRamp: {
      const uint32_t steps = static_cast<uint32_t>((rampMax(param) + kRampStep - 1) / kRampStep) + 1;
      m_duration_ms = 2u * steps * kRampStepMs;  // up, then down
      break;
    }
    case proto::CalScript::kRateSteps:
      m_duration_ms = rateSteps(param) * static_cast<uint32_t>(kRateStepMs + kRatePauseMs);
      break;
    case proto::CalScript::kFullSpin:
    case proto::CalScript::kSpinAndArc:
      if (!encoders) return proto::AckStatus::kCalPrecondition;
      m_duration_ms = kSpinTimeoutMs;
      break;
    case proto::CalScript::kRelayOscillation:
      if (param < 0 || param > 2) return proto::AckStatus::kInvalidCommand;
      if (param == 0 && !encoders) return proto::AckStatus::kCalPrecondition;  // speed loop needs encoders
      m_duration_ms = kRelayMs;
      break;
    case proto::CalScript::kDriveCounts:
      if (!encoders) return proto::AckStatus::kCalPrecondition;
      m_duration_ms = kDriveTimeoutMs;
      break;
    default:
      return proto::AckStatus::kUnavailable;  // SERVO_NUDGE: reserved
  }
  m_script = script;
  m_param = param;
  m_running = true;
  m_cancel = false;
  m_primed = false;
  m_start_ms = now_ms;
  m_tick = 0;
  m_step = 0;
  return proto::AckStatus::kOk;
}

void CalScriptRunner::finish(CalOutputs& out, proto::AckStatus status) {
  m_running = false;
  out.pwm_l = 0;
  out.pwm_r = 0;
  out.finished = true;
  out.status = status;
  out.progress = status == proto::AckStatus::kOk ? 100 : out.progress;
}

void CalScriptRunner::emit(CalOutputs& out, uint32_t t_ms, const int16_t v[8]) {
  if ((m_tick++ & 1u) != 0) return;
  out.sample = true;
  out.data.script = static_cast<uint8_t>(m_script);
  out.data.phase = out.stage;
  out.data.t_ms = static_cast<uint16_t>(t_ms);  // wraps after 65.5 s; the host unwraps
  for (uint8_t i = 0; i < 8; ++i) out.data.v[i] = v[i];
}

void CalScriptRunner::setOrigin(const CalInputs& in) {
  m_yaw0 = in.yaw_total_deg;
  m_enc0_l = in.enc_l;
  m_enc0_r = in.enc_r;
}

int16_t CalScriptRunner::pctToPwm(float pct) const { return deadbandPwm(pct, true, m_cfg.keep_pwm, m_cfg.keep_pwm); }

void CalScriptRunner::tickSpinAndArc(uint32_t now_ms, const CalInputs& in, CalOutputs& out, int16_t& l, int16_t& r) {
  const float dir = (m_param & 0x01) ? -1.0f : 1.0f;  // 0 = CCW (yaw +)
  const float rate = dir * in.yaw_rate_dps;
  if (m_step == 0 && absf(in.yaw_total_deg - m_yaw0) >= 360.0f) {
    m_step = 1;
    m_step_ms = now_ms;
  }
  if (m_step == 1 && elapsed(now_ms, m_step_ms, kSettleMs)) {
    m_step = 2;
    setOrigin(in);  // phase 1 counts from the end of the settle
  }
  const float yaw = in.yaw_total_deg - m_yaw0;
  if (m_step == 2 && absf(yaw) >= kArcDeg) return finish(out, proto::AckStatus::kOk);
  if (m_step == 0) {
    const float pwm = clampf(kSpinFfPwm + kSpinKp * (kSpinRateDps - rate), 0.0f, 255.0f);
    l = static_cast<int16_t>(-dir * pwm);
    r = static_cast<int16_t>(dir * pwm);
    out.progress = static_cast<uint8_t>(absf(yaw) / 6.0f);  // 0–60 %
  } else if (m_step == 2) {
    const float diff = clampf(kArcFfDiff + kArcKp * (kArcRateDps - rate), -100.0f, 100.0f);
    l = static_cast<int16_t>(kArcPwm - dir * diff);
    r = static_cast<int16_t>(kArcPwm + dir * diff);
    out.progress = static_cast<uint8_t>(60.0f + absf(yaw) * 40.0f / kArcDeg);
  } else {
    out.progress = 60;
  }
  out.stage = m_step == 2 ? 1 : 0;
  const int16_t v[8] = {sat16(in.enc_l - m_enc0_l), sat16(in.enc_r - m_enc0_r), sat16(yaw * 10.0f),
                        sat16(in.yaw_rate_dps * 10.0f), l, r, 0, 0};
  emit(out, now_ms - m_start_ms, v);
}

void CalScriptRunner::tickRelay(const CalInputs& in, float yaw, uint32_t t, CalOutputs& out, int16_t& l, int16_t& r) {
  float sp = 0.0f, meas = 0.0f, u = 0.0f;
  if (m_param == 0) {  // speed loop: both tracks, % of max_cps
    sp = kRelaySpeedPct;
    meas = (in.cps_l + in.cps_r) * 50.0f / m_cfg.max_cps;
    u = meas < sp ? kRelaySpeedD : -kRelaySpeedD;
    l = r = pctToPwm(sp + u);
  } else if (m_param == 1) {  // rate loop: spin, w % = feed-forward + relay
    sp = kRelayRateDps;
    meas = in.yaw_rate_dps;
    u = meas < sp ? kRelayRateD : -kRelayRateD;
    r = pctToPwm(sp * 100.0f / m_cfg.turn_rate_max_dps + u);
    l = static_cast<int16_t>(-r);
  } else {  // heading loop: hold the start heading, relay on the rate target (feed-forward only)
    meas = wrap180(yaw);
    u = meas < sp ? kRelayHeadingD : -kRelayHeadingD;
    r = pctToPwm(u * 100.0f / m_cfg.turn_rate_max_dps);
    l = static_cast<int16_t>(-r);
  }
  const int16_t v[8] = {sat16(sp * 10.0f), sat16(meas * 10.0f), sat16(u), l, r, 0, 0, 0};
  emit(out, t, v);
}

void CalScriptRunner::tick(uint32_t now_ms, const CalInputs& in, CalOutputs& out) {
  out.sample = false;
  out.finished = false;
  out.stage = 0;
  if (!m_running) {
    out.pwm_l = 0;
    out.pwm_r = 0;
    return;
  }
  if (m_cancel) {
    finish(out, proto::AckStatus::kCalCancelled);
    return;
  }
  if (!m_primed) {
    m_primed = true;
    setOrigin(in);
  }
  const uint32_t t = now_ms - m_start_ms;
  out.progress = static_cast<uint8_t>(t >= m_duration_ms ? 99 : (t * 100u) / m_duration_ms);
  const float yaw = in.yaw_total_deg - m_yaw0;
  const int32_t dl = in.enc_l - m_enc0_l;
  const int32_t dr = in.enc_r - m_enc0_r;
  const int16_t batt = static_cast<int16_t>(in.batt_mv);
  int16_t l = 0, r = 0;

  switch (m_script) {
    case proto::CalScript::kStillSample:
    case proto::CalScript::kIdle: {
      if (t >= m_duration_ms) return finish(out, proto::AckStatus::kOk);
      const int16_t v[8] = {in.gx, in.gy, in.gz, in.ax, in.ay, in.az, in.temp_centi_c, batt};
      emit(out, t, v);
      break;
    }
    case proto::CalScript::kPwmRamp: {
      if (t >= m_duration_ms) return finish(out, proto::AckStatus::kOk);
      const uint32_t half = m_duration_ms / 2;
      const bool down = t >= half;
      const int16_t max_pwm = rampMax(m_param);
      int16_t mag = static_cast<int16_t>(((down ? t - half : t) / kRampStepMs) * kRampStep);
      if (mag > max_pwm) mag = max_pwm;
      if (down) mag = static_cast<int16_t>(max_pwm - mag);
      const int16_t pwm = (m_param & 0x02) ? static_cast<int16_t>(-mag) : mag;
      const bool right = (m_param & 0x01) != 0;
      l = right ? 0 : pwm;
      r = right ? pwm : 0;
      out.stage = down ? 1 : 0;
      const int16_t v[8] = {mag, sat16(in.yaw_rate_dps * 10.0f), sat16(absi(right ? dr : dl)),
                            sat16(absf(right ? in.cps_r : in.cps_l)), 0, 0, 0, 0};
      emit(out, t, v);
      break;
    }
    case proto::CalScript::kRateSteps: {
      if (t >= m_duration_ms) return finish(out, proto::AckStatus::kOk);
      const uint8_t n = rateSteps(m_param);
      const uint8_t idx = static_cast<uint8_t>(t / (kRateStepMs + kRatePauseMs));
      const bool pause = (t % (kRateStepMs + kRatePauseMs)) >= kRateStepMs;
      const int16_t speed = static_cast<int16_t>(100 * idx / (n - 1));
      float lp = speed - 100.0f;  // full CCW turn command (w = 100 %) at this forward speed
      float rp = speed + 100.0f;
      const float m = absf(lp) > absf(rp) ? absf(lp) : absf(rp);
      if (m > 100.0f) {
        lp = lp * 100.0f / m;
        rp = rp * 100.0f / m;
      }
      out.stage = idx;
      if (pause) break;  // stopped between steps, no samples
      l = pctToPwm(lp);
      r = pctToPwm(rp);
      const int16_t v[8] = {speed, 1000, sat16(in.yaw_rate_dps * 10.0f), sat16(in.cps_l), sat16(in.cps_r), l, r, 0};
      emit(out, t, v);
      break;
    }
    case proto::CalScript::kFullSpin: {
      if (absf(yaw) >= 720.0f) return finish(out, proto::AckStatus::kOk);
      if (t >= m_duration_ms) return finish(out, proto::AckStatus::kCalFailed);
      const int16_t pwm = m_param <= 0 ? 255 : static_cast<int16_t>(m_param > 255 ? 255 : m_param);
      l = static_cast<int16_t>(-pwm);
      r = pwm;
      out.progress = static_cast<uint8_t>(absf(yaw) / 7.2f);
      const int16_t v[8] = {l, r, sat16(in.cps_l), sat16(in.cps_r), sat16(in.yaw_rate_dps * 10.0f),
                            sat16(yaw * 10.0f), sat16(dl), sat16(dr)};
      emit(out, t, v);
      break;
    }
    case proto::CalScript::kSpinAndArc: {
      if (t >= m_duration_ms) return finish(out, proto::AckStatus::kCalFailed);
      tickSpinAndArc(now_ms, in, out, l, r);
      if (out.finished) return;
      break;
    }
    case proto::CalScript::kRelayOscillation: {
      if (t >= m_duration_ms) return finish(out, proto::AckStatus::kOk);
      tickRelay(in, yaw, t, out, l, r);
      break;
    }
    case proto::CalScript::kDriveCounts: {
      const int32_t target = m_param <= 0 ? 7600 : m_param;
      if ((dl + dr) / 2 >= target) return finish(out, proto::AckStatus::kOk);
      if (t >= m_duration_ms) return finish(out, proto::AckStatus::kCalFailed);
      const float corr = clampf(kDriveKp * yaw, -60.0f, 60.0f);  // yaw > 0 = drifted left → push left harder
      l = static_cast<int16_t>(kDrivePwm + corr);
      r = static_cast<int16_t>(kDrivePwm - corr);
      out.progress = static_cast<uint8_t>(((dl + dr) / 2 * 100) / target);
      const int16_t v[8] = {sat16(dl), sat16(dr), sat16(yaw * 10.0f), l, r, sat16(in.cps_l), sat16(in.cps_r), 0};
      emit(out, t, v);
      break;
    }
    default:
      return finish(out, proto::AckStatus::kInternal);
  }
  out.pwm_l = l;
  out.pwm_r = r;
}

}  // namespace cq

#include "domain/lite/stall_guard_lite.h"

namespace cq {
namespace {
int16_t absI(int16_t v) { return v < 0 ? static_cast<int16_t>(-v) : v; }
}  // namespace

StallGuardLite::Result StallGuardLite::update(const Inputs& in, uint32_t now_ms, int8_t& side) {
  uint32_t dt = m_started ? now_ms - m_last_ms : 0;
  m_started = true;
  m_last_ms = now_ms;
  if (dt > kDetectMs) dt = kDetectMs;  // a long gap (task stalled) counts at most one detection window
  if (!in.enabled) {
    reset();
    return Result::kNone;
  }
  const int16_t pwm[2] = {in.pwm_l, in.pwm_r};
  const uint8_t keep[2] = {in.keep_l, in.keep_r};
  const uint16_t cps[2] = {in.cps_l, in.cps_r};
  bool slow[2] = {false, false};
  for (uint8_t s = 0; s < 2; ++s) {
    const int16_t mag = absI(pwm[s]);
    if (faulted(s)) continue;
    if (mag == 0) {  // idle: a long rest clears the history
      m_idle[s] = static_cast<uint16_t>(m_idle[s] + dt > kIdleResetMs ? kIdleResetMs : m_idle[s] + dt);
      if (m_idle[s] >= kIdleResetMs) m_acc[s] = 0;
      continue;
    }
    m_idle[s] = 0;
    if (mag < keep[s] + kMargin || keep[s] >= 255) continue;  // not driven: pause
    const uint32_t expected = static_cast<uint32_t>(kMaxCps) * static_cast<uint32_t>(mag - keep[s]) / (255u - keep[s]);
    slow[s] = static_cast<uint32_t>(cps[s]) * 100u < expected * kSlowPct;
    if (!slow[s]) {
      m_acc[s] = 0;  // moving properly
    } else {
      m_acc[s] = static_cast<uint16_t>(m_acc[s] + dt > kDetectMs ? kDetectMs : m_acc[s] + dt);
    }
  }
  const bool straight = (in.pwm_l > 0 && in.pwm_r > 0) || (in.pwm_l < 0 && in.pwm_r < 0);
  for (uint8_t s = 0; s < 2; ++s) {
    if (!slow[s] || m_acc[s] < kDetectMs) continue;
    m_acc[0] = m_acc[1] = 0;
    side = static_cast<int8_t>(s);
    const uint8_t other = static_cast<uint8_t>(1 - s);
    const bool other_moving = !slow[other] && cps[other] > 0;
    if (cps[s] == 0 && straight && other_moving && absI(in.yaw_rate_dps) < 10) {
      m_faulted = static_cast<uint8_t>(m_faulted | (1u << s));
      return Result::kFault;
    }
    return Result::kStall;
  }
  return Result::kNone;
}

}  // namespace cq

#include "domain/lite/stall_guard_lite.h"

namespace cq {
namespace {
int16_t absI(int16_t v) { return v < 0 ? static_cast<int16_t>(-v) : v; }
}  // namespace

StallGuardLite::Result StallGuardLite::update(const Inputs& in, uint32_t now_ms, int8_t& side) {
  if (!in.enabled) {
    m_slow = 0;
    return Result::kNone;
  }
  const int16_t pwm[2] = {in.pwm_l, in.pwm_r};
  const uint8_t keep[2] = {in.keep_l, in.keep_r};
  const uint16_t cps[2] = {in.cps_l, in.cps_r};
  bool slow[2] = {false, false};
  for (uint8_t s = 0; s < 2; ++s) {
    const int16_t mag = absI(pwm[s]);
    if (faulted(s) || mag < keep[s] + kMargin || keep[s] >= 255) continue;
    const uint32_t expected = static_cast<uint32_t>(kMaxCps) * static_cast<uint32_t>(mag - keep[s]) / (255u - keep[s]);
    slow[s] = static_cast<uint32_t>(cps[s]) * 100u < expected * kSlowPct;
  }
  const bool straight = (in.pwm_l > 0 && in.pwm_r > 0) || (in.pwm_l < 0 && in.pwm_r < 0);
  for (uint8_t s = 0; s < 2; ++s) {
    const uint8_t bit = static_cast<uint8_t>(1u << s);
    if (!slow[s]) {
      m_slow = static_cast<uint8_t>(m_slow & ~bit);
      continue;
    }
    if (!(m_slow & bit)) {
      m_slow = static_cast<uint8_t>(m_slow | bit);
      m_since[s] = now_ms;
    }
    if (now_ms - m_since[s] < kDetectMs) continue;
    m_slow = 0;
    side = static_cast<int8_t>(s);
    const uint8_t other = static_cast<uint8_t>(1 - s);
    const bool other_moving = !slow[other] && cps[other] > 0;
    if (cps[s] == 0 && straight && other_moving && absI(in.yaw_rate_dps) < 10) {
      m_faulted = static_cast<uint8_t>(m_faulted | bit);
      return Result::kFault;
    }
    return Result::kStall;
  }
  return Result::kNone;
}

}  // namespace cq

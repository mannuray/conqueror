#include "domain/estimation/encoder_health.h"

#include "domain/util/math_util.h"

namespace cq {

void EncoderHealth::update(const Inputs& in, uint32_t now_ms) {
  const int16_t pwm[2] = {in.pwm_l, in.pwm_r};
  const int16_t req[2] = {in.req_l, in.req_r};
  const bool edges[2] = {in.edges_l, in.edges_r};
  const uint8_t keep[2] = {in.keep_l, in.keep_r};
  const bool straight = (in.pwm_l > 0 && in.pwm_r > 0) || (in.pwm_l < 0 && in.pwm_r < 0);
  const bool spin = (in.pwm_l > 0 && in.pwm_r < 0) || (in.pwm_l < 0 && in.pwm_r > 0);
  const float yaw = absf(in.yaw_rate_dps);

  for (uint8_t s = 0; s < 2; ++s) {
    const uint8_t bit = static_cast<uint8_t>(1u << s);
    if (m_stalled & bit) {  // held while the supervisor cuts the track (its applied PWM is 0)
      const bool reversed = (m_stall_dir[s] > 0 && req[s] < 0) || (m_stall_dir[s] < 0 && req[s] > 0);
      if (reversed || elapsed(now_ms, m_stall_ms[s], kStallHoldMs)) {
        m_stalled = static_cast<uint8_t>(m_stalled & ~bit);
        m_silent[s] = false;  // a new stall needs another kDetectMs of drive
      }
      continue;
    }
    const int16_t mag = pwm[s] < 0 ? static_cast<int16_t>(-pwm[s]) : pwm[s];
    const bool driven = mag >= keep[s] + kMargin;
    const bool silent = driven && !edges[s];
    if (!silent) {
      m_silent[s] = false;
      continue;
    }
    if (!m_silent[s]) {
      m_silent[s] = true;
      m_since_ms[s] = now_ms;
    }
    if (!elapsed(now_ms, m_since_ms[s], kDetectMs) || (m_faulted & bit)) continue;
    const uint8_t other = static_cast<uint8_t>(1 - s);
    const bool moving_evidence = (straight && edges[other] && yaw < 10.0f) || (spin && !edges[other] && yaw > 20.0f);
    if (moving_evidence) {
      m_faulted = static_cast<uint8_t>(m_faulted | bit);
      m_new_faults = static_cast<uint8_t>(m_new_faults | bit);
    } else {
      m_stalled = static_cast<uint8_t>(m_stalled | bit);
      m_new_stalls = static_cast<uint8_t>(m_new_stalls | bit);
      m_stall_ms[s] = now_ms;
      m_stall_dir[s] = pwm[s] > 0 ? 1 : -1;
    }
  }
}

}  // namespace cq

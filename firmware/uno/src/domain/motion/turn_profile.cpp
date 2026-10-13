#include "domain/motion/turn_profile.h"

#include <math.h>

#include "domain/util/math_util.h"

namespace cq {

void TurnProfile::start(const TurnParams& p, float yaw_total_deg, uint32_t now_ms) {
  m_p = p;
  m_target_total = yaw_total_deg + p.angle_deg;
  m_rate_cmd = 0.0f;
  m_start_ms = now_ms;
  m_timeout_ms = static_cast<uint32_t>(absf(p.angle_deg) / p.max_rate_dps * 2000.0f) + 3000u;
  m_slow = false;
  m_progress = 0;
}

ProfileState TurnProfile::tick(float yaw_total_deg, float yaw_rate_dps, float dt_s, uint32_t now_ms,
                               MotionSetpoint& sp) {
  sp = MotionSetpoint{HeadingMode::kRate, 0.0f, 0.0f, 0.0f};
  const float remaining = m_target_total - yaw_total_deg;
  const float total = absf(m_p.angle_deg);
  m_progress = total > 0.0f ? static_cast<uint8_t>(clampf((1.0f - absf(remaining) / total) * 100.0f, 0.0f, 100.0f)) : 100;
  if (absf(remaining) < kToleranceDeg) {
    m_progress = 100;
    return ProfileState::kDone;
  }
  if (elapsed(now_ms, m_start_ms, m_timeout_ms)) return ProfileState::kTimeout;

  const float decel_limit = sqrtf(2.0f * m_p.accel_dps2 * absf(remaining));
  float rate = m_rate_cmd + m_p.accel_dps2 * dt_s;
  if (rate > m_p.max_rate_dps) rate = m_p.max_rate_dps;
  if (rate > decel_limit) rate = decel_limit;
  if (rate < kMinRateDps) rate = kMinRateDps;
  m_rate_cmd = rate;

  if (absf(yaw_rate_dps) < 3.0f) {  // commanded ≥ 15 °/s but nothing turns
    if (!m_slow) {
      m_slow = true;
      m_slow_since_ms = now_ms;
    } else if (elapsed(now_ms, m_slow_since_ms, 1000)) {
      return ProfileState::kStalled;
    }
  } else {
    m_slow = false;
  }
  sp.rate_dps = remaining > 0.0f ? rate : -rate;
  sp.v_pct = m_p.arc_v_pct * (rate / m_p.max_rate_dps);
  return ProfileState::kRunning;
}

}  // namespace cq

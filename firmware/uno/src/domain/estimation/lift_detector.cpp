#include "domain/estimation/lift_detector.h"

#include "domain/util/math_util.h"

namespace cq {

LiftDetector::Change LiftDetector::update(float az_g, float pitch_deg, float roll_deg, uint8_t tilt_limit_deg,
                                          uint32_t now_ms) {
  const float tilt = absf(pitch_deg) > absf(roll_deg) ? absf(pitch_deg) : absf(roll_deg);
  if (!m_tilted) {
    if (tilt > tilt_limit_deg) {
      if (!m_over) {
        m_over = true;
        m_over_since_ms = now_ms;
      }
      if (elapsed(now_ms, m_over_since_ms, kTiltHoldMs)) {
        m_tilted = true;
        m_over = false;
        return Change::kTilted;
      }
    } else {
      m_over = false;  // dipped back under the limit: the hold restarts
    }
  }
  if (m_tilted && tilt < static_cast<float>(tilt_limit_deg) - 5.0f) {
    m_tilted = false;
    return Change::kLevel;
  }
  const bool cond = m_lifted ? (az_g > 0.85f && az_g < 1.15f) : (absf(az_g) < 0.6f);
  if (cond != m_cond) {
    m_cond = cond;
    m_cond_since_ms = now_ms;
  }
  if (!cond) return Change::kNone;
  const uint32_t hold_ms = m_lifted ? 500u : 60u;
  if (!elapsed(now_ms, m_cond_since_ms, hold_ms)) return Change::kNone;
  m_lifted = !m_lifted;
  m_cond = false;
  return m_lifted ? Change::kLifted : Change::kLanded;
}

}  // namespace cq

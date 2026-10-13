#include "domain/motion/move_profile.h"

#include <math.h>

#include "domain/util/math_util.h"

namespace cq {

void MoveProfile::start(const MoveParams& p, int32_t enc_l, int32_t enc_r, uint32_t now_ms) {
  m_p = p;
  m_l0 = enc_l;
  m_r0 = enc_r;
  m_v_cmd = 0.0f;
  m_start_ms = now_ms;
  const float v_max = p.max_v_pct / 100.0f * p.v_full_mm_s;
  m_timeout_ms = static_cast<uint32_t>(absf(p.dist_mm) / (v_max > 1.0f ? v_max : 1.0f) * 2000.0f) + 3000u;
  m_progress = 0;
}

ProfileState MoveProfile::tick(int32_t enc_l, int32_t enc_r, bool stalled, float dt_s, uint32_t now_ms,
                               MotionSetpoint& sp) {
  sp = MotionSetpoint{HeadingMode::kHold, 0.0f, 0.0f, m_p.heading_deg};
  const float travelled = (static_cast<float>(enc_l - m_l0) + static_cast<float>(enc_r - m_r0)) * 0.5f / m_p.counts_per_mm;
  const float dir = m_p.dist_mm < 0.0f ? -1.0f : 1.0f;
  const float remaining = (m_p.dist_mm - travelled) * dir;  // > 0 while short of the target
  const float total = absf(m_p.dist_mm);
  m_progress = static_cast<uint8_t>(clampf((1.0f - remaining / (total > 0.0f ? total : 1.0f)) * 100.0f, 0.0f, 100.0f));
  if (remaining < kToleranceMm) {
    m_progress = 100;
    return ProfileState::kDone;
  }
  if (stalled) return ProfileState::kStalled;
  if (elapsed(now_ms, m_start_ms, m_timeout_ms)) return ProfileState::kTimeout;

  const float v_max = m_p.max_v_pct / 100.0f * m_p.v_full_mm_s;
  float v = m_v_cmd + m_p.accel_mm_s2 * dt_s;
  if (v > v_max) v = v_max;
  const float decel_limit = sqrtf(2.0f * m_p.accel_mm_s2 * remaining);
  if (v > decel_limit) v = decel_limit;
  if (v < kMinSpeedMmS) v = kMinSpeedMmS;
  m_v_cmd = v;
  sp.v_pct = dir * v / m_p.v_full_mm_s * 100.0f;
  return ProfileState::kRunning;
}

}  // namespace cq

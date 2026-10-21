#include "core/rate_loop.h"

#include "core/motion_config.h"

namespace cq {
namespace core {

void RateLoop::reset() {
  m_integral = 0.0f;
  m_last_target = 0;
}

int8_t RateLoop::step(int16_t target_ddps, int16_t rate_ddps, uint16_t dt_ms, int8_t limit_pct) {
  // A sign change or a zero target starts afresh (review M2: no leftover w at target 0).
  if (target_ddps == 0 || (target_ddps > 0 && m_last_target < 0) || (target_ddps < 0 && m_last_target > 0)) {
    m_integral = 0.0f;
  }
  m_last_target = target_ddps;
  const float target = target_ddps / 10.0f, err = (target_ddps - rate_ddps) / 10.0f;
  const float pre = m_ff * target + motion::kRateKpPctPerDps * err;
  const float candidate = m_integral + err * (dt_ms / 1000.0f);
  float w = pre + motion::kRateKiPctPerDeg * candidate;
  const float lim = static_cast<float>(limit_pct);
  if (w > lim || w < -lim) {
    w = w > 0.0f ? lim : -lim;  // saturated: keep the old integral (anti-windup)
  } else {
    m_integral = candidate;
  }
  return static_cast<int8_t>(w >= 0.0f ? w + 0.5f : w - 0.5f);
}

}  // namespace core
}  // namespace cq

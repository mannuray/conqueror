#include "core/rate_loop.h"

#include "core/motion_config.h"

namespace cq {
namespace core {

void RateLoop::reset() {
  m_integral = 0.0f;
  m_last_target = 0;
}

int8_t RateLoop::step(int16_t target_ddps, int16_t rate_ddps, uint16_t dt_ms) {
  if ((target_ddps > 0 && m_last_target < 0) || (target_ddps < 0 && m_last_target > 0)) m_integral = 0.0f;
  m_last_target = target_ddps;
  const float target = target_ddps / 10.0f, err = (target_ddps - rate_ddps) / 10.0f;
  const float pre = motion::kRateFfPctPerDps * target + motion::kRateKpPctPerDps * err;
  const float candidate = m_integral + err * (dt_ms / 1000.0f);
  float w = pre + motion::kRateKiPctPerDeg * candidate;
  if (w > 100.0f || w < -100.0f) {
    w = w > 0.0f ? 100.0f : -100.0f;  // saturated: keep the old integral (anti-windup)
  } else {
    m_integral = candidate;
  }
  return static_cast<int8_t>(w >= 0.0f ? w + 0.5f : w - 0.5f);
}

}  // namespace core
}  // namespace cq

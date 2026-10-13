#include "domain/estimation/accel_variance.h"

#include "domain/util/math_util.h"

namespace cq {

void AccelVariance::add(float mag_g, uint32_t now_ms) {
  const float x = mag_g * 1000.0f;
  ++m_n;
  const float d = x - m_mean;
  m_mean += d / m_n;
  m_m2 += d * (x - m_mean);
  if (elapsed(now_ms, m_window_start_ms, 1000) && m_n > 1) {
    const float var = m_m2 / (m_n - 1);
    m_value = var > 65535.0f ? 65535 : static_cast<uint16_t>(var);
    m_n = 0;
    m_mean = 0.0f;
    m_m2 = 0.0f;
    m_window_start_ms = now_ms;
  }
}

}  // namespace cq

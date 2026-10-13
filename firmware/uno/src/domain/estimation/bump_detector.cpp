#include "domain/estimation/bump_detector.h"

#include <math.h>

#include "domain/util/math_util.h"

namespace cq {

bool BumpDetector::update(float ax_g, float ay_g, uint16_t threshold_mg, uint32_t now_ms, int16_t& mg,
                          int16_t& dir) {
  const float dx = ax_g - m_base_x;
  const float dy = ay_g - m_base_y;
  m_base_x += 0.02f * dx;
  m_base_y += 0.02f * dy;
  const float mag_mg = sqrtf(dx * dx + dy * dy) * 1000.0f;
  if (mag_mg < threshold_mg) return false;
  if (m_fired && !elapsed(now_ms, m_last_ms, 500)) return false;
  m_fired = true;
  m_last_ms = now_ms;
  mg = mag_mg > 32767.0f ? 32767 : static_cast<int16_t>(mag_mg);
  dir = dx < 0.0f ? 0 : 1;  // sudden deceleration = front hit
  return true;
}

}  // namespace cq

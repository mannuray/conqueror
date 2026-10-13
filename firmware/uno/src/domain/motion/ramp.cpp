#include "domain/motion/ramp.h"

#include "domain/util/math_util.h"

namespace cq {

float Ramp::step(float target, float dt_s, float accel_per_s, float decel_per_s) {
  // From rest (value 0) any target is an acceleration in its own direction.
  const bool same_sign = m_value == 0.0f || (target >= 0.0f) == (m_value > 0.0f);
  const bool accelerating = same_sign && absf(target) > absf(m_value);
  const float max_step = (accelerating ? accel_per_s : decel_per_s) * dt_s;
  const float diff = target - m_value;
  m_value += clampf(diff, -max_step, max_step);
  return m_value;
}

}  // namespace cq

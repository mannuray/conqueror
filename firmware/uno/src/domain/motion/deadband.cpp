#include "domain/motion/deadband.h"

#include "domain/util/math_util.h"

namespace cq {

int16_t deadbandPwm(float pct, bool moving, uint8_t start_pwm, uint8_t keep_pwm) {
  const float mag = absf(pct);
  if (mag < 0.5f) return 0;
  const float base = moving ? keep_pwm : start_pwm;
  const float pwm = clampf(base + (mag / 100.0f) * (255.0f - base), 0.0f, 255.0f);
  const int16_t v = static_cast<int16_t>(pwm + 0.5f);
  return pct < 0.0f ? static_cast<int16_t>(-v) : v;
}

}  // namespace cq

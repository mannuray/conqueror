#pragma once
#include <stdint.h>

namespace cq {

// PWM scale so the 6 V motors never see more than motor_v_max from a fresh 2S pack. No pack (USB): 1.
inline float batteryScale(uint16_t batt_mv, uint16_t motor_v_max_mv) {
  if (batt_mv < 3000 || batt_mv <= motor_v_max_mv) return 1.0f;
  const float s = static_cast<float>(motor_v_max_mv) / static_cast<float>(batt_mv);
  return s < 0.5f ? 0.5f : s;
}

}  // namespace cq

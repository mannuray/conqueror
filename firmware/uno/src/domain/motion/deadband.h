#pragma once
#include <stdint.h>

namespace cq {

// Maps a track target in % to PWM, compensating static friction: from rest the first non-zero
// command starts at `start_pwm`, while moving it starts at `keep_pwm`. |pct| < 0.5 → 0.
int16_t deadbandPwm(float pct, bool moving, uint8_t start_pwm, uint8_t keep_pwm);

}  // namespace cq

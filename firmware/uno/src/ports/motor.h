#pragma once
#include "types/pwm_output.h"

namespace cq {
namespace port {
namespace motor {
void begin();                       // outputs low, driver in standby
void apply(const PwmOutput& out);   // sign = direction, |value| = duty
}  // namespace motor
}  // namespace port
}  // namespace cq

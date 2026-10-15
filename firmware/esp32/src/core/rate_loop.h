// DRIVE_RATE on the bridge (bridge LLD R4.4): w = ff * r + Kp * err + Ki * integral(err), |w| <= 100,
// integration frozen while saturated (anti-windup), integral reset when the target changes sign.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

class RateLoop {
 public:
  void reset();
  int8_t step(int16_t target_ddps, int16_t rate_ddps, uint16_t dt_ms);  // 0.1 °/s in, % out (+ = CCW / left)

 private:
  float m_integral = 0.0f;  // °/s · s
  int16_t m_last_target = 0;
};

}  // namespace core
}  // namespace cq

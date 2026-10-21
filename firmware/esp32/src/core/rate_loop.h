// DRIVE_RATE on the bridge (bridge LLD R4.4): w = ff * r + Kp * err + Ki * integral(err), |w| <= 100,
// integration frozen while saturated (anti-windup), integral reset when the target changes sign or is zero.
#pragma once

#include <stdint.h>

#include "core/motion_config.h"

namespace cq {
namespace core {

class RateLoop {
 public:
  void reset();
  void setFeedForward(float pct_per_dps) { m_ff = pct_per_dps; }  // Plan L: arcs need their own (kept by reset)
  // 0.1 °/s in, % out (+ = CCW / left); |w| <= limit_pct, the integral frozen while at the limit (R18 review).
  int8_t step(int16_t target_ddps, int16_t rate_ddps, uint16_t dt_ms, int8_t limit_pct = 100);

 private:
  float m_integral = 0.0f;  // °/s · s
  float m_ff = motion::kRateFfPctPerDps;  // % per °/s: spins 0.45, arcs kArcFfPctPerDps
  int16_t m_last_target = 0;
};

}  // namespace core
}  // namespace cq

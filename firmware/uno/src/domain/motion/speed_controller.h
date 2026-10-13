#pragma once
#include <stdint.h>

#include "domain/util/pid.h"

namespace cq {

struct SpeedConfig {
  uint8_t start_pwm;
  uint8_t keep_pwm;
  uint16_t max_cps;
  PidGains gains;   // % correction per % speed error
  bool closed_loop; // encoders enabled and healthy
};

// PWM limit while the envelope caps speed: feed-forward PWM of the cap plus 20 headroom.
int16_t pwmLimitForCap(uint8_t cap_pct, uint8_t keep_pwm);

// One track: feed-forward through the deadband map + PID on measured speed (when closed loop).
// The integrator freezes while the TargetLimiter caps the target (`capped`, LLD §9.5) and, as extra
// anti-windup, while the output sits on the PWM limit (envelope cap or 255).
class SpeedController {
 public:
  void configure(const SpeedConfig& cfg);
  int16_t update(float target_pct, float actual_cps, float dt_s, int16_t pwm_limit, bool moving, bool capped);
  void reset() {
    m_pid.reset();
    m_saturated = false;
  }
  bool saturated() const { return m_saturated; }
  float integrator() const { return m_pid.integrator(); }

 private:
  SpeedConfig m_cfg{70, 55, 2500, {0, 0, 0}, false};
  Pid m_pid;
  bool m_saturated = false;
};

}  // namespace cq

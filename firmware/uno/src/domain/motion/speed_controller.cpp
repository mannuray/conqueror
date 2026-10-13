#include "domain/motion/speed_controller.h"

#include "domain/motion/deadband.h"
#include "domain/util/math_util.h"

namespace cq {

int16_t pwmLimitForCap(uint8_t cap_pct, uint8_t keep_pwm) {
  if (cap_pct >= 100) return 255;
  const int16_t v = static_cast<int16_t>(deadbandPwm(cap_pct, true, keep_pwm, keep_pwm) + 20);
  return v > 255 ? 255 : v;
}

void SpeedController::configure(const SpeedConfig& cfg) {
  m_cfg = cfg;
  m_pid.setGains(cfg.gains);
  m_pid.setLimits(-100.0f, 100.0f);
}

int16_t SpeedController::update(float target_pct, float actual_cps, float dt_s, int16_t pwm_limit, bool moving,
                                bool capped) {
  if (absf(target_pct) < 0.5f) {
    reset();
    return 0;
  }
  float cmd_pct = target_pct;
  if (m_cfg.closed_loop && m_cfg.max_cps > 0) {
    const float actual_pct = actual_cps * 100.0f / m_cfg.max_cps;
    cmd_pct = clampf(target_pct + m_pid.update(target_pct - actual_pct, dt_s, m_saturated || capped), -100.0f, 100.0f);
  }
  int16_t pwm = deadbandPwm(cmd_pct, moving, m_cfg.start_pwm, m_cfg.keep_pwm);
  m_saturated = pwm >= pwm_limit || pwm <= -pwm_limit;
  if (pwm > pwm_limit) pwm = pwm_limit;
  if (pwm < -pwm_limit) pwm = static_cast<int16_t>(-pwm_limit);
  return pwm;
}

}  // namespace cq

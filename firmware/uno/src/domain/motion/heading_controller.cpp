#include "domain/motion/heading_controller.h"

#include "domain/util/math_util.h"

namespace cq {

void HeadingController::configure(const HeadingConfig& cfg) {
  m_cfg = cfg;
  m_rate.setGains(cfg.rate);
  m_rate.setLimits(-100.0f, 100.0f);
  m_heading.setGains(cfg.heading);
  m_heading.setLimits(-static_cast<float>(cfg.turn_rate_max_dps), static_cast<float>(cfg.turn_rate_max_dps));
}

void HeadingController::reset() {
  m_rate.reset();
  m_heading.reset();
  m_holding = false;
  m_settle_s = 0.0f;
}

float HeadingController::rateLoop(float rate_target_dps, float yaw_rate_dps, float dt_s, bool freeze) {
  const float ff = rate_target_dps * 100.0f / m_cfg.turn_rate_max_dps;
  return clampf(ff + m_rate.update(rate_target_dps - yaw_rate_dps, dt_s, freeze), -100.0f, 100.0f);
}

float HeadingController::update(HeadingMode mode, float w_cmd_pct, float rate_target_dps, float heading_target_deg,
                                 float yaw_deg, float yaw_rate_dps, float dt_s, bool freeze) {
  switch (mode) {
    case HeadingMode::kOff:
      return w_cmd_pct;
    case HeadingMode::kRate:
      return rateLoop(rate_target_dps, yaw_rate_dps, dt_s, freeze);
    case HeadingMode::kHold: {
      const float err = wrap180(heading_target_deg - yaw_deg);
      const float rate_target = m_heading.update(err, dt_s, freeze);
      return rateLoop(rate_target, yaw_rate_dps, dt_s, freeze);
    }
  }
  return 0.0f;
}

float HeadingController::teleop(float v_pct, float w_pct, float yaw_deg, float yaw_rate_dps, float dt_s, bool freeze) {
  if (absf(v_pct) < 0.5f && absf(w_pct) < 0.5f) {
    reset();
    return 0.0f;
  }
  if (absf(w_pct) >= 1.0f) {
    // Steering is open-loop (design-notes §41): the gyro only keeps the tank straight. A rate loop
    // here fought the tracks' grip and made turns step.
    m_holding = false;
    m_heading.reset();
    m_rate.reset();
    m_settle_s = 0.0f;
    return w_pct;
  }
  if (!m_holding) {
    if (absf(yaw_rate_dps) > kCaptureRateDps && m_settle_s < kSettleS) {  // let the turn settle (no braking)
      m_settle_s += dt_s;
      return 0.0f;
    }
    m_holding = true;
    m_held_deg = yaw_deg;
    m_heading.reset();
  }
  return update(HeadingMode::kHold, 0.0f, 0.0f, m_held_deg, yaw_deg, yaw_rate_dps, dt_s, freeze);
}

}  // namespace cq

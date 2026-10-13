#pragma once
#include <stdint.h>

#include "domain/motion/motion_setpoint.h"

namespace cq {

struct TurnParams {
  float angle_deg;     // relative, CCW +
  float max_rate_dps;
  float accel_dps2;
  float arc_v_pct;     // forward speed at max rate (0 = spin in place)
};

// Trapezoidal yaw-rate profile on the unwrapped gyro yaw. Done within 1°.
class TurnProfile {
 public:
  static constexpr float kToleranceDeg = 1.0f;
  static constexpr float kMinRateDps = 15.0f;
  void start(const TurnParams& p, float yaw_total_deg, uint32_t now_ms);
  ProfileState tick(float yaw_total_deg, float yaw_rate_dps, float dt_s, uint32_t now_ms, MotionSetpoint& sp);
  uint8_t progressPct() const { return m_progress; }

 private:
  TurnParams m_p{0, 0, 0, 0};
  float m_target_total = 0.0f;
  float m_rate_cmd = 0.0f;
  uint32_t m_start_ms = 0;
  uint32_t m_timeout_ms = 0;
  uint32_t m_slow_since_ms = 0;
  bool m_slow = false;
  uint8_t m_progress = 0;
};

}  // namespace cq

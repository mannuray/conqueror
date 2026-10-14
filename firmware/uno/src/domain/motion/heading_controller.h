#pragma once
#include <stdint.h>

#include "domain/util/pid.h"

namespace cq {

enum class HeadingMode : uint8_t { kOff, kRate, kHold };

struct HeadingConfig {
  PidGains rate;     // w % per °/s error
  PidGains heading;  // °/s per ° error
  uint16_t turn_rate_max_dps;
};

// Gyro steering. Rate: w = feed-forward + PID(rate error). Hold: heading PID → rate target → rate loop.
class HeadingController {
 public:
  static constexpr float kCaptureRateDps = 5.0f;
  // Longest wait for the rotation to settle after steering; then the heading is captured anyway, so a
  // track that pulls the tank round cannot keep heading hold off (design-notes §41).
  static constexpr float kSettleS = 0.3f;
  void configure(const HeadingConfig& cfg);
  float update(HeadingMode mode, float w_cmd_pct, float rate_target_dps, float heading_target_deg, float yaw_deg,
               float yaw_rate_dps, float dt_s, bool freeze);
  float teleop(float v_pct, float w_pct, float yaw_deg, float yaw_rate_dps, float dt_s, bool freeze);
  void reset();
  float heldHeading() const { return m_held_deg; }
  bool holding() const { return m_holding; }

 private:
  float rateLoop(float rate_target_dps, float yaw_rate_dps, float dt_s, bool freeze);
  HeadingConfig m_cfg{{0, 0, 0}, {0, 0, 0}, 180};
  Pid m_rate;
  Pid m_heading;
  bool m_holding = false;
  float m_held_deg = 0.0f;
  float m_settle_s = 0.0f;  // time spent waiting to capture
};

}  // namespace cq

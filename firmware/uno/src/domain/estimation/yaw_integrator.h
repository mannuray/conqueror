#pragma once

namespace cq {

// Integrates bias-corrected gyro z (CCW +). While stationary, |rate| below the clamp is treated as 0.
class YawIntegrator {
 public:
  static constexpr float kStationaryClampDps = 0.5f;
  void update(float rate_dps, float dt_s, bool stationary);
  float yawDeg() const { return m_yaw_deg; }
  float rateDps() const { return m_rate_dps; }
  float totalDeg() const { return m_total_deg; }
  void reset() {
    m_yaw_deg = 0.0f;
    m_total_deg = 0.0f;
  }

 private:
  float m_yaw_deg = 0.0f;
  float m_total_deg = 0.0f;
  float m_rate_dps = 0.0f;
};

}  // namespace cq

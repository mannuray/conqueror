#pragma once

namespace cq {

// Trapezoidal velocity profile for one servo axis (degrees).
class ServoProfile {
 public:
  void reset(float pos_deg) {
    m_pos = pos_deg;
    m_target = pos_deg;
    m_vel = 0.0f;
  }
  void setTarget(float deg) { m_target = deg; }
  void setLimits(float v_max_dps, float a_max_dps2) {
    m_v_max = v_max_dps;
    m_a_max = a_max_dps2;
  }
  float step(float dt_s);
  float position() const { return m_pos; }
  float target() const { return m_target; }
  bool moving() const { return m_pos != m_target || m_vel != 0.0f; }

 private:
  float m_pos = 90.0f;
  float m_target = 90.0f;
  float m_vel = 0.0f;
  float m_v_max = 120.0f;
  float m_a_max = 400.0f;
};

}  // namespace cq

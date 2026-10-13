#pragma once
#include <stdint.h>

namespace cq {

struct PidGains {
  float kp;
  float ki;
  float kd;
};

inline PidGains gainsFromMilli(int32_t kp, int32_t ki, int32_t kd) {
  return PidGains{kp * 0.001f, ki * 0.001f, kd * 0.001f};
}

// PID with conditional integration (no integration while saturated in the error direction or frozen),
// integrator clamped to the output range, derivative on error with a light low-pass.
class Pid {
 public:
  void setGains(const PidGains& g) { m_g = g; }
  void setLimits(float out_min, float out_max) {
    m_min = out_min;
    m_max = out_max;
  }
  float update(float error, float dt_s, bool freeze_integrator);
  void reset() {
    m_i = 0.0f;
    m_prev_err = 0.0f;
    m_d = 0.0f;
    m_primed = false;
  }
  float integrator() const { return m_i; }

 private:
  PidGains m_g{0.0f, 0.0f, 0.0f};
  float m_min = -1.0f;
  float m_max = 1.0f;
  float m_i = 0.0f;
  float m_prev_err = 0.0f;
  float m_d = 0.0f;
  bool m_primed = false;
};

}  // namespace cq

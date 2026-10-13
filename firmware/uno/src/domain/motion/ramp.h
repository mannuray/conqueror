#pragma once

namespace cq {

// Rate limiter: accelerating (|target| above |value|, same sign) uses accel, everything else decel.
class Ramp {
 public:
  float step(float target, float dt_s, float accel_per_s, float decel_per_s);
  float value() const { return m_value; }
  void reset(float v = 0.0f) { m_value = v; }

 private:
  float m_value = 0.0f;
};

}  // namespace cq

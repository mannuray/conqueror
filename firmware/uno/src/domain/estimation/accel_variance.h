#pragma once
#include <stdint.h>

namespace cq {

// Variance of |a| (mg²) over 1 s windows (Welford); used as a surface roughness / vibration metric.
class AccelVariance {
 public:
  void add(float mag_g, uint32_t now_ms);
  uint16_t value() const { return m_value; }

 private:
  uint32_t m_window_start_ms = 0;
  uint16_t m_n = 0;
  float m_mean = 0.0f;
  float m_m2 = 0.0f;
  uint16_t m_value = 0;
};

}  // namespace cq

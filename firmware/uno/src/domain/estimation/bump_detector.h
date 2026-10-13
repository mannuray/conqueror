#pragma once
#include <stdint.h>

namespace cq {

// Horizontal acceleration spike above a slow baseline; 500 ms refractory.
class BumpDetector {
 public:
  bool update(float ax_g, float ay_g, uint16_t threshold_mg, uint32_t now_ms, int16_t& mg, int16_t& dir);

 private:
  float m_base_x = 0.0f;
  float m_base_y = 0.0f;
  uint32_t m_last_ms = 0;
  bool m_fired = false;
};

}  // namespace cq

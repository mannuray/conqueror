#pragma once
#include <stdint.h>

namespace cq {

// Single-channel encoder: direction comes from the commanded PWM sign (`dir`, ±1).
class EncoderTrack {
 public:
  static constexpr uint32_t kTimeoutUs = 100000;
  void update(uint32_t edges, uint32_t period_us, uint32_t last_edge_us, uint32_t now_us, int8_t dir);
  int32_t count() const { return m_count; }
  float cps() const { return m_cps; }
  bool recent() const { return m_recent; }

 private:
  uint32_t m_prev_edges = 0;
  bool m_primed = false;
  int32_t m_count = 0;
  float m_cps = 0.0f;
  bool m_recent = false;
};

}  // namespace cq

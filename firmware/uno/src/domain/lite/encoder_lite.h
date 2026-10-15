#pragma once
#include <stdint.h>

namespace cq {

// Single-channel encoder (rising edges only): speed from the period between the last two edges; stopped when
// no edge for 100 ms. Direction comes from the motor command (no quadrature yet).
class EncoderLite {
 public:
  static constexpr uint32_t kStaleUs = 100000;
  void update(int32_t count, uint32_t period_us, uint32_t last_edge_us, uint32_t now_us);
  int32_t count() const { return m_count; }
  uint16_t cps() const { return m_cps; }
  bool moving() const { return m_cps != 0; }

 private:
  int32_t m_count = 0;
  uint16_t m_cps = 0;
};

// Telemetry spd_act: cps as % of max_cps with the command's sign; −128 = unknown (encoders off).
int8_t speedPctLite(uint16_t cps, int16_t pwm_sign_source, uint16_t max_cps, bool enabled);

}  // namespace cq

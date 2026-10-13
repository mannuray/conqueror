#pragma once
#include <stdint.h>

namespace cq {

class SafetySupervisor;

// The only value the motor port accepts. Anything but zero() must come from SafetySupervisor.
class PwmOutput {
 public:
  static PwmOutput zero() { return PwmOutput(0, 0, true); }
  int16_t left() const { return m_left; }
  int16_t right() const { return m_right; }
  bool enabled() const { return m_enabled; }  // false = driver standby (tracks coast)

 private:
  PwmOutput(int16_t left, int16_t right, bool enabled) : m_left(left), m_right(right), m_enabled(enabled) {}
  int16_t m_left;
  int16_t m_right;
  bool m_enabled;
  friend class SafetySupervisor;
};

}  // namespace cq

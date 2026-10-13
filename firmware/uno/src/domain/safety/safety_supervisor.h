#pragma once
#include <stdint.h>

#include "types/pwm_output.h"

namespace cq {

struct SafetyInputs {
  bool lifted;
  bool tilted;
  bool batt_critical;
  bool brake_on;       // obstacle brake enabled (envelope may force it)
  uint16_t dist_mm;    // 0xFFFF clear, 0xFFFE sensor fault (brake unavailable, never blocks)
  uint8_t stop_cm;
  uint8_t stall_mask;  // bit 0 left, bit 1 right: stalled track, output cut (LLD §9.6)
  bool selftest_locked;  // critical self-test failure: all motion locked (LLD §10.9); Override never lifts it
};

struct SafetyStatus {
  bool blocked;
  bool halted;  // also set while selftest-locked (primitives are cancelled like on any halt)
  bool coast;
  bool locked;  // selftest_locked applied
};

// Last gate before the motor port. Owns PwmOutput construction.
class SafetySupervisor {
 public:
  PwmOutput apply(int16_t pwm_l, int16_t pwm_r, const SafetyInputs& in, SafetyStatus& st) const;
};

}  // namespace cq

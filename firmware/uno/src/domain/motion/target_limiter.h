#pragma once
#include <stdint.h>

namespace cq {

struct Limits {
  uint8_t speed_cap_pct;  // envelope
  uint16_t dist_mm;       // ultrasonic (0xFFFF clear, 0xFFFE fault)
  uint8_t stop_cm;
  uint8_t slow_cm;
  bool brake_on;
  uint8_t degraded_cap_pct;  // 100 none; 50 ultrasonic fault, 30 IMU fault (min of both) — LLD §9.5
};

struct Limited {
  float l;
  float r;
  bool capped;
  bool slowed;
};

// Applied in % BEFORE the speed controllers so a 20 % envelope means 20 % track speed. Cap = min(envelope,
// degraded). Both tracks are scaled by the same factor (arc shape kept). Forward motion fades to 0
// between slow_cm and stop_cm.
Limited limitTargets(float l_pct, float r_pct, const Limits& lim);

}  // namespace cq

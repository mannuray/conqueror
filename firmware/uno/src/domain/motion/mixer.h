#pragma once
#include <stdint.h>

namespace cq {

struct MixerConfig {
  uint8_t turn_full_speed_pct;  // turning authority left at full speed (profile setting)
  bool car_reverse;             // reverse_steer == 0: steering inverted when reversing
  bool pivot_fallback;          // setting pivot_fallback: inner track below keep_pct → 0
  float keep_pct;               // track speed % at the deadband keep PWM (≈ keep_pwm × 100 / 255)
};

// Arcade mix v (forward %) + w (CCW %) → track targets in % (LLD §3.4). Turning priority: when a track
// would exceed ±100 %, |v| is reduced so the difference L − R (the turn) is kept. Pivot fallback: a
// non-zero inner target below keep_pct becomes 0 (the tank pivots instead of crawling that track).
void mix(float v_pct, float w_pct, const MixerConfig& cfg, float& l_pct, float& r_pct);

}  // namespace cq

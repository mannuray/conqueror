#pragma once
#include <stdint.h>

namespace cq {

struct MixerConfig {
  uint8_t turn_full_speed_pct;  // profile setting; no longer scales the stick (spec 02 §A4.1), kept for compatibility
  bool pivot_fallback;          // setting pivot_fallback: inner track below keep_pct → 0
  float keep_pct;               // track speed % at the deadband keep PWM (≈ keep_pwm × 100 / 255)
};

// Arcade mix v (forward %) + w (CCW %) → track targets in % (LLD §3.4). Turning priority: when a track
// would exceed ±100 %, |v| is reduced so the difference L − R (the turn) is kept. Pivot fallback: a
// non-zero inner target below keep_pct becomes 0 (the tank pivots instead of crawling that track).
// Car-style steering (reverse_steer 0): when reversing, the DRIVER's steering is inverted, like a car.
// Apply it to the teleop input only - never to a gyro correction or a primitive's rate, which are
// physical turns (design-notes §39: a flipped heading-hold correction spun the tank in reverse).
inline float carSteer(float v_pct, float w_pct, bool car_reverse) {
  return (car_reverse && v_pct < 0.0f) ? -w_pct : w_pct;
}

// w is a physical turn (CCW +) in every direction; mix() never flips it. Two turn inputs:
// w_steer_pct = the driver's open-loop steering → angle mix (spec 02 §A4.1, owner 2026-10-15);
// w_ctrl_pct  = a closed-loop output (gyro heading hold, rate loop, primitive) - never reduced, the
// controller already sets its size (design-notes §41: scaling halved the heading hold at 80 %).
void mix(float v_pct, float w_steer_pct, float w_ctrl_pct, const MixerConfig& cfg, float& l_pct, float& r_pct);

}  // namespace cq

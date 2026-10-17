#pragma once
#include <stdint.h>

namespace cq {

struct Tracks {
  int16_t l;  // %
  int16_t r;
};

// DRIVE v/w arrive as int8 (−128..127); everything downstream assumes ±100 (−(−128) is still −128 in int8).
inline int8_t clampDriveLite(int8_t x) { return x < -100 ? static_cast<int8_t>(-100) : (x > 100 ? static_cast<int8_t>(100) : x); }

// Car-style reverse steering (reverse_steer 0): reversing flips the DRIVER's stick, like a car. Never the bridge
// motion service's output (DRIVE src AUTO) - that w is a physical turn; flipped, it becomes positive feedback
// (design-notes §39, Plan H review C1).
inline int8_t steerLite(int8_t v, int8_t w, bool car_reverse, bool auto_src) {
  return (car_reverse && v < 0 && !auto_src) ? static_cast<int8_t>(-w) : w;
}

// Track mix (CCW +), spec 02 §A4.1: the driver's steering (w_steer != 0) uses the angle mix (forward arc at 45°,
// spin only near sideways); straight driving with the gyro's w_ctrl uses the arcade mix with turning priority
// (L − R kept when a track would exceed 100 %). turn_full_pct is no longer used (kept for the setting).
Tracks mixLite(int8_t v, int8_t w_steer, int8_t w_ctrl, uint8_t turn_full_pct);
// Spec 02 §F1.4: the bridge motion service's frames (src AUTO) are a controller's output - plain mix; the driver's
// stick keeps the angle mix (§A4.1).
Tracks liteMix(int8_t v, int8_t w_steer, int8_t w_ctrl, bool auto_src, uint8_t turn_full_pct);
// Envelope / degraded speed cap (ratio kept) and the obstacle slow zone (forward only, brake on, valid range).
Tracks capLite(Tracks t, uint8_t cap_pct, uint16_t dist_mm, uint8_t stop_cm, uint8_t slow_cm, bool brake_on,
               bool& capped);

// Acceleration limit per 10 ms tick, value in 0.1 % (decel twice as fast, as the full firmware).
class RampLite {
 public:
  int16_t step(int16_t target_pct, uint16_t accel_pct_s);
  void reset() { m_pm = 0; }
  int16_t value() const { return m_pm; }

 private:
  int16_t m_pm = 0;
};

// Deadband compensation: |0.1 %| < 5 → 0; else base + |pm| × (255 − base) / 1000, base = keep when moving.
int16_t toPwmLite(int16_t permille, bool moving, uint8_t start_pwm, uint8_t keep_pwm);

}  // namespace cq

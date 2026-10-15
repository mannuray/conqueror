#pragma once
#include <stdint.h>

namespace cq {

// Lift and tilt from the accelerometer only (Lite has no attitude filter). Accel is low-passed (≈ 40 ms) and
// scaled to 128 per g. Tilted: ax² + ay² > az² × tan²(limit) (or az ≤ 0) for 300 ms; level again below limit − 5°.
// Lifted: |az| < 0.6 g for 60 ms; landed: 0.85..1.15 g for 500 ms (full LiftDetector thresholds).
class TiltLite {
 public:
  enum class Change : uint8_t { kNone, kLifted, kLanded, kTilted, kLevel };
  static constexpr uint32_t kTiltHoldMs = 300;
  Change update(int16_t ax, int16_t ay, int16_t az, uint8_t limit_deg, uint32_t now_ms);
  bool lifted() const { return m_lifted; }
  bool tilted() const { return m_tilted; }

 private:
  bool over(uint8_t deg) const;
  int16_t m_fx = 0, m_fy = 0, m_fz = 8192;  // filtered raw accel
  bool m_lifted = false;
  bool m_tilted = false;
  bool m_over = false;
  uint32_t m_over_since_ms = 0;
  bool m_cond = false;
  uint32_t m_cond_since_ms = 0;
};

}  // namespace cq

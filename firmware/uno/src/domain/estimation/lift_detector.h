#pragma once
#include <stdint.h>

namespace cq {

// Lifted: |az| < 0.6 g for 60 ms. Landed: 0.85..1.15 g for 500 ms.
// Tilted: |pitch| or |roll| over the limit (default 45°) continuously for kTiltHoldMs = 300 ms (filters bumps and
// turn artefacts). Level: below limit - 5° (hysteresis), immediately, no hold.
class LiftDetector {
 public:
  enum class Change : uint8_t { kNone, kLifted, kLanded, kTilted, kLevel };
  static constexpr uint32_t kTiltHoldMs = 300;
  Change update(float az_g, float pitch_deg, float roll_deg, uint8_t tilt_limit_deg, uint32_t now_ms);
  bool lifted() const { return m_lifted; }
  bool tilted() const { return m_tilted; }

 private:
  bool m_lifted = false;
  bool m_tilted = false;
  bool m_over = false;  // tilt currently over the limit (not yet confirmed)
  uint32_t m_over_since_ms = 0;
  bool m_cond = false;
  uint32_t m_cond_since_ms = 0;
};

}  // namespace cq

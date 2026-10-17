#pragma once
#include <stdint.h>

namespace cq {

// Spec 02 §F1.3 / LLD uno-lite §19.1: the heading hold made things worse - |error| above 30° and larger than when the
// correction saturated, with the correction saturated for 0.5 s (50 control ticks). A reversed motor (design-notes
// §60) turns the hold into positive feedback; this stops it.
class RunawayGuardLite {
 public:
  static constexpr int32_t kErrCdeg = 3000;
  static constexpr uint8_t kTicks = 50;
  bool step(bool holding, int32_t err_cdeg, int8_t w_ctrl) {
    const int32_t e = err_cdeg < 0 ? -err_cdeg : err_cdeg;
    const bool saturated = w_ctrl >= 100 || w_ctrl <= -100;
    if (!holding || !saturated || e <= kErrCdeg) {
      m_ticks = 0;
      return false;
    }
    if (m_ticks == 0) m_start = e;
    if (++m_ticks < kTicks) return false;
    const bool grew = e > m_start;
    m_ticks = 0;
    return grew;
  }
  void reset() { m_ticks = 0; }

 private:
  uint8_t m_ticks = 0;
  int32_t m_start = 0;
};

}  // namespace cq

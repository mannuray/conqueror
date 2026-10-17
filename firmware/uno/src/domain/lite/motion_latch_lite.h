#pragma once
#include <stdint.h>

namespace cq {

// Spec 02 §F1 / LLD uno-lite §19: one latch for stall, encoder fault and heading runaway. Owner (2026-10-17): stop
// BOTH tracks until STOP or until the driver's command returns to v = 0, w = 0; the first trip's reason is kept.
class MotionLatchLite {
 public:
  enum class Reason : uint8_t { kNone, kStall, kEncoderFault, kHeading };
  void trip(Reason r, int8_t side) {
    if (m_reason != Reason::kNone) return;
    m_reason = r;
    m_side = side;
  }
  void clear() {
    m_reason = Reason::kNone;
    m_side = -1;
  }
  void clearIfReleased(int8_t v, int8_t w) {
    if (v == 0 && w == 0) clear();
  }
  bool latched() const { return m_reason != Reason::kNone; }
  Reason reason() const { return m_reason; }
  int8_t side() const { return m_side; }
  uint8_t stallMask() const { return latched() ? 0x03 : 0x00; }  // SafetyInputs.stall_mask: both tracks cut

 private:
  Reason m_reason = Reason::kNone;
  int8_t m_side = -1;
};

}  // namespace cq

// Pushing but not moving (bridge LLD R21): during a straight drive, tracks that really move at different speeds turn
// the tank - (v_right - v_left) / track width. When the gyro does not see that turn for kSlipHoldMs, a track is
// slipping (e.g. against a table leg the distance sensor cannot see) and the tank is held. Pure, host-tested.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

class SlipWatch {
 public:
  void reset() {
    m_started = false;
    m_slipping = false;
  }
  // Each TELEM_FAST: the raw counts (they rise in both directions), the gyro, counts per metre, the drive's
  // direction (+1 forward, -1 reverse). True once held.
  bool step(int32_t enc_l, int32_t enc_r, int16_t rate_ddps, uint16_t cpm, int8_t dir, uint32_t now_ms);

 private:
  void startWindow(int32_t enc_l, int32_t enc_r, uint32_t now_ms);
  bool m_started = false;
  uint32_t m_win_ms = 0;
  int32_t m_l0 = 0, m_r0 = 0;
  int32_t m_rate_sum = 0;
  int32_t m_rate_n = 0;
  bool m_slipping = false;
  uint32_t m_slip_since = 0;
};

}  // namespace core
}  // namespace cq

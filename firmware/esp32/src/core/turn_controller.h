// TURN (spin) on the bridge (bridge LLD R4.3): proportional to the angle still to go, latency-compensated, with a
// minimum command, a heading-only done window with hysteresis (w = 0 inside), stall and timeout. Whole numbers only.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

// Relative angle to turn (0.01°, + = CCW / left): `by` as given, or `to` the shortest way from the current yaw.
int32_t turnDelta(uint16_t yaw_cdeg, bool has_to, int32_t by_cdeg, uint16_t to_cdeg);
int32_t wrapDeltaCdeg(int32_t d);  // to -18000..17999

struct TurnStep {
  int8_t w = 0;  // % for DRIVE (v = 0)
  bool done = false;
  bool timeout = false;
  bool stalled = false;
};

class TurnController {
 public:
  void begin(uint16_t start_yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, uint32_t now_ms);
  TurnStep step(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms);
  uint8_t progressPct() const;

 private:
  int8_t command(int32_t error_cdeg, int16_t rate_ddps) const;
  bool inWindow(int32_t error_cdeg, uint32_t now_ms);
  void remember(uint32_t now_ms);
  int32_t avgRateDdps(uint32_t now_ms) const;  // from the heading over ~kAvgRateMs (a shaking gyro averages out)
  bool settled(uint32_t now_ms);
  bool stalledFor(int8_t w, int16_t rate_ddps, uint32_t now_ms);

  int32_t m_delta = 0;      // target, relative to the start (0.01°)
  int32_t m_pos = 0;        // turned so far, unwrapped (0.01°)
  uint16_t m_last_yaw = 0;
  uint8_t m_speed = 40;
  uint32_t m_start_ms = 0;
  uint32_t m_timeout_ms = 0;
  bool m_in_window = false;
  uint32_t m_window_since = 0;
  int32_t m_window_pos = 0;  // m_pos when the current hold began
  static constexpr uint8_t kHist = 16;  // >= kAvgRateMs at 50 Hz
  int32_t m_hist_pos[kHist] = {};
  uint32_t m_hist_ms[kHist] = {};
  uint8_t m_hist_n = 0;
  uint8_t m_hist_head = 0;
  bool m_stalling = false;
  uint32_t m_stall_since = 0;
};

}  // namespace core
}  // namespace cq

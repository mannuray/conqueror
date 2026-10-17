// Arc turn on the bridge (Plan L, spec 02 §H4, bridge LLD R8.3): forward at `speed`; the turn rate is gyro-closed on
// ω = v_meas / R with v_meas from the encoders, through the drive_rate loop; ends at the target heading.
#pragma once

#include <stdint.h>

#include "core/move_controller.h"
#include "core/rate_loop.h"

namespace cq {
namespace core {

struct ArcStep {
  int8_t v = 0;
  int8_t w = 0;
  bool done = false;
  bool stalled = false;
  bool blocked = false;
  bool timeout = false;
  bool diverged = false;
};

class ArcController {
 public:
  void begin(const MotionTelem& t, int32_t delta_cdeg, uint16_t radius_cm, uint8_t speed_pct, uint16_t cpm,
             uint32_t now_ms);
  ArcStep step(const MotionTelem& t, uint32_t now_ms);
  uint8_t progressPct() const;
  int16_t targetDdps() const { return m_target_ddps; }  // last ω asked of the rate loop (0.1 °/s)

 private:
  int32_t speedCmS10(int32_t counts, uint32_t now_ms);  // measured centre speed, 0.1 cm/s, never negative

  RateLoop m_rate;
  int32_t m_delta = 0;  // target, 0.01°
  int32_t m_pos = 0;    // turned, unwrapped, 0.01°
  int8_t m_dir = 1;
  uint16_t m_last_yaw = 0;
  uint16_t m_radius = 1;
  uint16_t m_cpm = 7560;
  uint8_t m_speed = 40;
  int16_t m_target_ddps = 0;
  bool m_finished = false;  // done: progress 100 % (the stop leads the lag by a little)
  uint32_t m_last_ms = 0;
  uint32_t m_deadline = 0;
  static constexpr uint8_t kHist = 8;
  int32_t m_hist_counts[kHist] = {};
  uint32_t m_hist_ms[kHist] = {};
  uint8_t m_hist_n = 0;
  uint8_t m_hist_head = 0;
  int32_t m_ref_counts = 0;
  int32_t m_ref_pos = 0;
  uint32_t m_ref_ms = 0;
  int32_t m_stall_counts = 1;
  bool m_blocked = false;
  uint32_t m_blocked_since = 0;
  bool m_wrong = false;
  uint32_t m_wrong_since = 0;
};

}  // namespace core
}  // namespace cq

// MOVE by distance on the bridge (Plan K, bridge LLD R7.3): straight (w = 0, the Uno holds the heading), mean
// encoder distance, cruise then a linear slow-down to a crawl over the last 10 cm, stall / blocked / timeout ends.
// Whole numbers only.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

// The TELEM_FAST fields the motion service uses (0.01°, 0.1 °/s, counts rising in both directions, FastFlagBit).
struct MotionTelem {
  uint16_t yaw_cdeg = 0;
  int16_t rate_ddps = 0;
  int32_t enc_l = 0;
  int32_t enc_r = 0;
  uint32_t flags = 0;
};

struct MoveStep {
  int8_t v = 0;  // % for DRIVE (w = 0)
  bool done = false;
  bool stalled = false;
  bool blocked = false;
  bool timeout = false;
};

class MoveController {
 public:
  void begin(int32_t enc_l, int32_t enc_r, int16_t dist_cm, uint8_t speed_pct, uint16_t cpm, uint32_t now_ms);
  MoveStep step(const MotionTelem& t, uint32_t now_ms);
  uint8_t progressPct() const;
  int32_t movedMm() const;  // signed like dist_cm

 private:
  int32_t m_start_l = 0;
  int32_t m_start_r = 0;
  int32_t m_target = 1;       // counts, > 0
  int32_t m_slow = 1;         // slow zone, counts
  int32_t m_stall_counts = 1;  // progress that counts as moving
  int32_t m_done = 0;         // travelled, counts (rise in both directions; m_dir gives v / moved signs)
  int32_t m_ref = 0;          // m_done at the last progress
  int8_t m_dir = 1;
  uint8_t m_speed = 40;
  uint16_t m_cpm = 7560;
  uint32_t m_ref_ms = 0;
  uint32_t m_deadline = 0;
  bool m_blocked = false;
  uint32_t m_blocked_since = 0;
};

}  // namespace core
}  // namespace cq

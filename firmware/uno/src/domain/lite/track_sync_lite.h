#pragma once
#include <stdint.h>

#include "domain/lite/drive_mix_lite.h"

namespace cq {

// Spec 02 §F1.1 / LLD uno-lite §19.1: while driving straight keep both tracks' counted distance equal (fast inner
// loop); the gyro heading hold keeps running on top (slow outer loop, slip). Counts always rise (channel A rising
// edges), so the correction works on magnitudes: the faster side is slowed, the slower one sped up.
class TrackSyncLite {
 public:
  static constexpr int16_t kSyncCountsPerPct = 20;  // 20 counts ahead -> 1 %
  static constexpr int16_t kSyncCpsPerPct = 50;     // 50 counts/s faster -> 1 %
  static constexpr int16_t kSyncMaxPct = 15;
  static constexpr int32_t kSyncMaxCounts = 300;    // anti-windup on the distance term
  void reset() {
    m_active = false;
    m_c = 0;
  }
  Tracks apply(Tracks t, bool active, int32_t count_l, int32_t count_r, uint16_t cps_l, uint16_t cps_r);
  int16_t correctionPct() const { return m_c; }

 private:
  bool m_active = false;
  int32_t m_base_l = 0;
  int32_t m_base_r = 0;
  int16_t m_c = 0;
};

}  // namespace cq

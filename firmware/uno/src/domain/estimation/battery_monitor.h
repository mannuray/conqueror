#pragma once
#include <stdint.h>

namespace cq {

// Pack voltage = average of the last 4 samples (one per 10 Hz tick, LLD §9.13); low/critical need
// 2 s below the threshold; leaving critical or low needs that threshold + hyst_mv (setting batt_hyst, 200 mV) for 2 s
// (LLD §10.6: critical → low above crit + hyst, low → ok above low + hyst).
// Below 3 V the board is USB powered (no pack): level kUsb, never an alarm.
class BatteryMonitor {
 public:
  enum class Level : uint8_t { kUsb, kOk, kLow, kCritical };
  static constexpr uint8_t kAvgSamples = 4;
  Level update(uint16_t adc, uint16_t mv_per_count_x100, uint16_t low_mv, uint16_t crit_mv, uint16_t hyst_mv,
               uint32_t now_ms);
  bool changed() const { return m_changed; }
  uint16_t mv() const { return m_mv; }
  Level level() const { return m_level; }

 private:
  uint16_t m_samples_mv[kAvgSamples] = {};
  uint8_t m_idx = 0;
  uint8_t m_count = 0;  // samples so far, up to kAvgSamples (the average primes on the first one)
  uint16_t m_mv = 0;
  Level m_level = Level::kOk;
  Level m_candidate = Level::kOk;
  uint32_t m_candidate_since_ms = 0;
  bool m_changed = false;
};

}  // namespace cq

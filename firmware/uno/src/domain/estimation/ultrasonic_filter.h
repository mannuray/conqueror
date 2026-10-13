#pragma once
#include <stdint.h>

#include "types/hw_types.h"

namespace cq {

// Median of the last 3 readings, offset, clear (> 4 m or echo high > 25 ms: nothing in range) and
// fault (LLD §9.16): 3 consecutive failed measurements. A failed measurement is kNoEcho (no rising edge
// within kMaxRiseUs of the trigger) or a kPending beyond kMaxPending ticks (echo line stuck high > 250 ms:
// clones hold it up to 200 ms; an unplugged sensor reads high through the pull-up). kClear never faults.
class UltrasonicFilter {
 public:
  static constexpr uint16_t kClear = 0xFFFF;
  static constexpr uint16_t kFault = 0xFFFE;
  static constexpr uint16_t kMaxMm = 4000;
  static constexpr uint8_t kFaultAfter = 3;
  static constexpr uint8_t kMaxPending = 5;  // 5 × 50 ms = 250 ms of skipped pings
  // Echo must rise within 10 ms of the trigger (LLD §11.6): clones answer late (bench module 2.2 ms);
  // 10 ms + the 25 ms clear limit still fits the 50 ms ping period.
  static constexpr uint32_t kMaxRiseUs = 10000;
  static constexpr uint32_t kClearUs = 25000;
  enum class Change : uint8_t { kNone, kFault, kRecovered };

  // One ping's edges → its state; echo_us is set only for kReady.
  static EchoState classify(bool rose, uint32_t rise_delay_us, bool fell, uint32_t width_us, uint16_t& echo_us);

  Change onReading(EchoState st, uint16_t echo_us, uint8_t offset_mm, uint32_t now_ms);
  uint16_t distMm() const { return m_fault ? kFault : m_dist_mm; }
  int16_t closingCmS() const { return static_cast<int16_t>(m_closing_cm_s); }
  bool fault() const { return m_fault; }
  bool valid() const { return !m_fault && m_dist_mm != kClear; }

 private:
  Change countFailure();
  uint16_t m_hist[3] = {kClear, kClear, kClear};
  uint8_t m_idx = 0;
  uint16_t m_dist_mm = kClear;
  uint16_t m_prev_mm = kClear;
  uint32_t m_prev_ms = 0;
  float m_closing_cm_s = 0.0f;
  uint8_t m_failures = 0;  // consecutive failed measurements
  uint8_t m_pending = 0;   // consecutive ticks without a ping (echo still high)
  bool m_fault = false;
};

}  // namespace cq

#pragma once
#include <stdint.h>

#include "types/hw_types.h"

namespace cq {

// Integer copy of the full UltrasonicFilter without the closing speed: median of 3, offset, clear (> 4 m),
// fault after 3 failed measurements or > 5 skipped pings (echo stuck high); kClear never faults (LLD §9.16).
class RangeLite {
 public:
  static constexpr uint16_t kClear = 0xFFFF;
  static constexpr uint16_t kFault = 0xFFFE;
  static constexpr uint16_t kMaxMm = 4000;
  enum class Change : uint8_t { kNone, kFault, kRecovered };
  Change onReading(EchoState st, uint16_t echo_us, uint8_t offset_mm);
  uint16_t distMm() const { return m_fault ? kFault : m_dist_mm; }
  bool fault() const { return m_fault; }

 private:
  Change countFailure();
  uint16_t m_hist[3] = {kClear, kClear, kClear};
  uint8_t m_idx = 0;
  uint16_t m_dist_mm = kClear;
  uint8_t m_failures = 0;
  uint8_t m_pending = 0;
  bool m_fault = false;
};

}  // namespace cq

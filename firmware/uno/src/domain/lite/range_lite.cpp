#include "domain/lite/range_lite.h"

namespace cq {

namespace {
uint16_t median3(uint16_t a, uint16_t b, uint16_t c) {
  if (a > b) {
    const uint16_t t = a;
    a = b;
    b = t;
  }
  if (b > c) b = c;
  return a > b ? a : b;
}
}  // namespace

RangeLite::Change RangeLite::countFailure() {
  if (m_failures < 255) ++m_failures;
  if (m_failures == 3 && !m_fault) {
    m_fault = true;
    return Change::kFault;
  }
  return Change::kNone;
}

RangeLite::Change RangeLite::onReading(EchoState st, uint16_t echo_us, uint8_t offset_mm) {
  if (st == EchoState::kPending) {
    if (m_pending < 255) ++m_pending;
    return m_pending > 5 ? countFailure() : Change::kNone;
  }
  m_pending = 0;
  if (st == EchoState::kNoEcho) return countFailure();
  m_failures = 0;
  Change change = Change::kNone;
  if (m_fault) {
    m_fault = false;
    change = Change::kRecovered;
  }
  uint16_t mm = kClear;
  if (st == EchoState::kReady) {
    const uint32_t raw = static_cast<uint32_t>(echo_us) * 343u / 2000u;
    if (raw <= kMaxMm) mm = raw > offset_mm ? static_cast<uint16_t>(raw - offset_mm) : 0;
  }
  m_hist[m_idx] = mm;
  m_idx = static_cast<uint8_t>((m_idx + 1) % 3);
  m_dist_mm = median3(m_hist[0], m_hist[1], m_hist[2]);
  return change;
}

}  // namespace cq

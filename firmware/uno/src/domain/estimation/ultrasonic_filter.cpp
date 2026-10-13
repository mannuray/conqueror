#include "domain/estimation/ultrasonic_filter.h"

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

UltrasonicFilter::Change UltrasonicFilter::countFailure() {
  if (m_failures < 255) ++m_failures;
  if (m_failures == kFaultAfter && !m_fault) {
    m_fault = true;
    return Change::kFault;
  }
  return Change::kNone;
}

UltrasonicFilter::Change UltrasonicFilter::onReading(EchoState st, uint16_t echo_us, uint8_t offset_mm,
                                                     uint32_t now_ms) {
  Change change = Change::kNone;
  if (st == EchoState::kPending) {  // no ping sent: echo still high from the previous one
    if (m_pending < 255) ++m_pending;
    return m_pending > kMaxPending ? countFailure() : change;
  }
  m_pending = 0;
  if (st == EchoState::kNoEcho) return countFailure();
  m_failures = 0;  // the sensor answered (kReady or kClear)
  if (m_fault) {
    m_fault = false;
    change = Change::kRecovered;
  }
  uint16_t mm = kClear;
  if (st == EchoState::kReady) {
    const uint32_t raw = (static_cast<uint32_t>(echo_us) * 343u) / 2000u;  // speed of sound 343 m/s
    if (raw <= kMaxMm) mm = raw > offset_mm ? static_cast<uint16_t>(raw - offset_mm) : 0;
  }
  m_hist[m_idx] = mm;
  m_idx = static_cast<uint8_t>((m_idx + 1) % 3);
  m_dist_mm = median3(m_hist[0], m_hist[1], m_hist[2]);

  if (m_dist_mm != kClear && m_prev_mm != kClear && now_ms != m_prev_ms) {
    const float v = (static_cast<float>(m_prev_mm) - m_dist_mm) * 100.0f / static_cast<float>(now_ms - m_prev_ms);
    m_closing_cm_s += 0.3f * (v - m_closing_cm_s);  // mm/ms × 100 = cm/s
  } else if (m_dist_mm == kClear) {
    m_closing_cm_s = 0.0f;
  }
  m_prev_mm = m_dist_mm;
  m_prev_ms = now_ms;
  return change;
}

EchoState UltrasonicFilter::classify(bool rose, uint32_t rise_delay_us, bool fell, uint32_t width_us,
                                     uint16_t& echo_us) {
  if (!rose || rise_delay_us > kMaxRiseUs) return EchoState::kNoEcho;  // the sensor did not answer
  if (!fell || width_us > kClearUs) return EchoState::kClear;           // nothing in range
  echo_us = static_cast<uint16_t>(width_us);
  return EchoState::kReady;
}

}  // namespace cq

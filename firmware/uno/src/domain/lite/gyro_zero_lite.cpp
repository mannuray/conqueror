#include "domain/lite/gyro_zero_lite.h"

namespace cq {

namespace {
int16_t absI(int16_t v) { return v < 0 ? static_cast<int16_t>(-v) : v; }
}  // namespace

void GyroZeroLite::seed(int16_t stored_raw) {
  if (m_seeded_valid && m_seeded == stored_raw) return;
  m_seeded = stored_raw;
  m_seeded_valid = true;
  m_zero = stored_raw;
  m_state = State::kWaitQuiet;
  m_have_prev = false;
  m_accepted = 0;
}

void GyroZeroLite::startWindow() {
  m_n = 0;
  m_sum = 0;
  m_min = INT16_MAX;
  m_max = INT16_MIN;
}

void GyroZeroLite::update(int16_t raw, bool quiet, uint32_t now_ms) {
  if (!quiet) {
    m_state = State::kWaitQuiet;
    return;
  }
  if (m_state == State::kWaitQuiet) {
    m_quiet_since_ms = now_ms;
    m_state = State::kSettle;
  }
  if (m_state == State::kSettle) {
    if (static_cast<uint32_t>(now_ms - m_quiet_since_ms) < kSettleMs) return;
    startWindow();
    m_state = State::kCollect;
  }
  m_sum += raw;
  if (raw < m_min) m_min = raw;
  if (raw > m_max) m_max = raw;
  if (++m_n < kWindowSamples) return;
  judge();
  startWindow();
}

void GyroZeroLite::judge() {
  const int32_t half = m_n / 2;
  const int16_t mean = static_cast<int16_t>((m_sum >= 0 ? m_sum + half : m_sum - half) / m_n);
  const bool valid = static_cast<int32_t>(m_max) - m_min <= kMaxSpreadRaw && absI(mean) <= kMaxZeroRaw;
  if (!valid) {
    m_have_prev = false;
    return;
  }
  if (m_have_prev && absI(static_cast<int16_t>(mean - m_prev)) <= kAgreeRaw) {
    m_zero = static_cast<int16_t>((static_cast<int32_t>(mean) + m_prev) / 2);
    ++m_accepted;
  }
  m_prev = mean;
  m_have_prev = true;
}

}  // namespace cq

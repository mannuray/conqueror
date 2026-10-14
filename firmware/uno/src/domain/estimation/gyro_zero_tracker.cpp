#include "domain/estimation/gyro_zero_tracker.h"

#include "domain/util/math_util.h"

namespace cq {

void GyroZeroTracker::seed(const int16_t stored_raw[3]) {
  bool same = m_seeded_valid;
  for (uint8_t i = 0; i < 3; ++i) same = same && m_seeded[i] == stored_raw[i];
  if (same) return;
  for (uint8_t i = 0; i < 3; ++i) {
    m_seeded[i] = stored_raw[i];
    m_zero[i] = stored_raw[i];
  }
  m_seeded_valid = true;
  m_state = State::kWaitQuiet;
  m_have_prev = false;
  m_accepted = 0;
}

void GyroZeroTracker::startWindow() {
  m_n = 0;
  for (uint8_t i = 0; i < 3; ++i) {
    m_sum[i] = 0;
    m_min[i] = INT16_MAX;
    m_max[i] = INT16_MIN;
  }
}

void GyroZeroTracker::update(const int16_t raw[3], bool quiet, uint32_t now_ms) {
  if (!quiet) {
    m_state = State::kWaitQuiet;  // the window in progress is dropped
    return;
  }
  if (m_state == State::kWaitQuiet) {
    m_quiet_since_ms = now_ms;
    m_state = State::kSettle;
  }
  if (m_state == State::kSettle) {
    if (!elapsed(now_ms, m_quiet_since_ms, kSettleMs)) return;
    startWindow();
    m_state = State::kCollect;
  }
  for (uint8_t i = 0; i < 3; ++i) {
    m_sum[i] += raw[i];
    if (raw[i] < m_min[i]) m_min[i] = raw[i];
    if (raw[i] > m_max[i]) m_max[i] = raw[i];
  }
  if (++m_n < kWindowSamples) return;
  judge();
  startWindow();
}

void GyroZeroTracker::judge() {
  float mean[3];
  bool valid = true;
  bool agree = m_have_prev;
  for (uint8_t i = 0; i < 3; ++i) {
    mean[i] = static_cast<float>(m_sum[i]) / static_cast<float>(m_n);
    const int32_t spread = static_cast<int32_t>(m_max[i]) - m_min[i];
    valid = valid && spread <= kMaxSpreadRaw && absf(mean[i]) <= kMaxZeroRaw;
    agree = agree && absf(mean[i] - m_prev[i]) <= kAgreeRaw;
  }
  if (!valid) {
    m_have_prev = false;
    return;
  }
  if (agree) {
    for (uint8_t i = 0; i < 3; ++i) m_zero[i] = 0.5f * (mean[i] + m_prev[i]);
    ++m_accepted;
  }
  for (uint8_t i = 0; i < 3; ++i) m_prev[i] = mean[i];
  m_have_prev = true;
}

}  // namespace cq

#include "core/turn_controller.h"

#include "core/motion_config.h"

namespace cq {
namespace core {
namespace {
int32_t absI(int32_t v) { return v < 0 ? -v : v; }
}  // namespace

int32_t wrapDeltaCdeg(int32_t d) {
  d %= 36000;
  if (d >= 18000) d -= 36000;
  if (d < -18000) d += 36000;
  return d;
}

int32_t turnDelta(uint16_t yaw_cdeg, bool has_to, int32_t by_cdeg, uint16_t to_cdeg) {
  if (!has_to) return by_cdeg;
  const int32_t d = wrapDeltaCdeg(static_cast<int32_t>(to_cdeg) - yaw_cdeg);
  return d == -18000 ? 18000 : d;  // exactly opposite: turn left (+180°)
}

void TurnController::begin(uint16_t start_yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, uint32_t now_ms) {
  m_delta = delta_cdeg;
  m_pos = 0;
  m_last_yaw = start_yaw_cdeg;
  m_speed = speed_pct;
  m_start_ms = now_ms;
  m_timeout_ms = static_cast<uint32_t>(absI(delta_cdeg)) / 2u + motion::kTimeoutBaseMs;  // |angle| / 20 °/s + 3 s
  m_in_window = false;
  m_stalling = false;
  m_hist_n = 0;
  m_hist_head = 0;
  remember(now_ms);
}

void TurnController::remember(uint32_t now_ms) {
  m_hist_pos[m_hist_head] = m_pos;
  m_hist_ms[m_hist_head] = now_ms;
  m_hist_head = static_cast<uint8_t>((m_hist_head + 1) % kHist);
  if (m_hist_n < kHist) ++m_hist_n;
}

int32_t TurnController::avgRateDdps(uint32_t now_ms) const {
  // Against the newest sample at least kAvgRateMs old (else the oldest kept). 0.01° per ms * 100 = 0.1 °/s.
  uint8_t pick = static_cast<uint8_t>((m_hist_head + kHist - m_hist_n) % kHist);  // oldest
  for (uint8_t k = 1; k <= m_hist_n; ++k) {  // newest first
    const uint8_t i = static_cast<uint8_t>((m_hist_head + kHist - k) % kHist);
    if (now_ms - m_hist_ms[i] >= motion::kAvgRateMs) {
      pick = i;
      break;
    }
  }
  const uint32_t dt = now_ms - m_hist_ms[pick];
  return dt == 0 ? 0 : (m_pos - m_hist_pos[pick]) * 100 / static_cast<int32_t>(dt);
}

int8_t TurnController::command(int32_t error_cdeg, int16_t rate_ddps) const {
  const int32_t ep = error_cdeg - static_cast<int32_t>(rate_ddps) * motion::kLatencyDdpsToCdeg / 10;
  const int32_t e = ep != 0 ? ep : error_cdeg;
  int32_t mag = absI(e) * motion::kTurnKpPctPerDeg / 100;
  if (mag < motion::kTurnWMin) mag = motion::kTurnWMin;
  if (mag > m_speed) mag = m_speed;
  return static_cast<int8_t>(e < 0 ? -mag : mag);
}

bool TurnController::inWindow(int32_t error_cdeg, uint32_t now_ms) {
  // Enter: close and not sweeping through (heading-average rate); stay: heading only - the momentary gyro rate is
  // unusable while the tank shakes on target.
  const bool in = m_in_window ? absI(error_cdeg) <= motion::kWindowOutCdeg
                              : absI(error_cdeg) <= motion::kWindowInCdeg &&
                                    absI(avgRateDdps(now_ms)) < motion::kWindowEnterRateDdps;
  if (in && !m_in_window) {
    m_window_since = now_ms;
    m_window_pos = m_pos;
  }
  m_in_window = in;
  return in;
}

// Held for kWindowHoldMs with < kSettleMoveCdeg net movement; a real drift restarts the hold instead.
bool TurnController::settled(uint32_t now_ms) {
  if (now_ms - m_window_since < motion::kWindowHoldMs) return false;
  if (absI(m_pos - m_window_pos) < motion::kSettleMoveCdeg) return true;
  m_window_since = now_ms;
  m_window_pos = m_pos;
  return false;
}

bool TurnController::stalledFor(int8_t w, int16_t rate_ddps, uint32_t now_ms) {
  const int16_t r = rate_ddps < 0 ? static_cast<int16_t>(-rate_ddps) : rate_ddps;
  const int8_t aw = w < 0 ? static_cast<int8_t>(-w) : w;
  if (aw < motion::kTurnWMin || r >= motion::kStallRateDdps) {
    m_stalling = false;
    return false;
  }
  if (!m_stalling) {
    m_stalling = true;
    m_stall_since = now_ms;
  }
  return now_ms - m_stall_since >= motion::kStallMs;
}

TurnStep TurnController::step(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms) {
  m_pos += wrapDeltaCdeg(static_cast<int32_t>(yaw_cdeg) - m_last_yaw);
  m_last_yaw = yaw_cdeg;
  remember(now_ms);
  const int32_t error = m_delta - m_pos;
  TurnStep s;
  s.timeout = now_ms - m_start_ms >= m_timeout_ms;
  if (inWindow(error, now_ms)) {
    s.done = settled(now_ms);
    m_stalling = false;
    return s;  // w = 0: let the tank settle, no corrections from a shaking gyro
  }
  s.w = command(error, rate_ddps);
  s.stalled = stalledFor(s.w, rate_ddps, now_ms);
  return s;
}

uint8_t TurnController::progressPct() const {
  if (m_delta == 0) return 100;
  int32_t p = m_pos * 100 / m_delta;
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return static_cast<uint8_t>(p);
}

}  // namespace core
}  // namespace cq

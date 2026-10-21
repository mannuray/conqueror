#include "core/turn_controller.h"

#include "core/motion_config.h"

namespace cq {
namespace core {
namespace {
int32_t absI(int32_t v) { return v < 0 ? -v : v; }
}  // namespace

uint16_t coastAverage(uint16_t old_ms, uint16_t reading_ms) {
  int32_t v = (3 * static_cast<int32_t>(old_ms) + reading_ms + 2) / 4;
  if (v < motion::kCoastMinMs) v = motion::kCoastMinMs;
  if (v > motion::kCoastMaxMs) v = motion::kCoastMaxMs;
  return static_cast<uint16_t>(v);
}

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

void TurnController::begin(uint16_t start_yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, uint32_t now_ms,
                           uint16_t coast_ms) {
  m_delta = delta_cdeg;
  m_pos = 0;
  m_last_yaw = start_yaw_cdeg;
  m_speed = speed_pct;
  m_start_ms = m_last_ms = now_ms;
  m_timeout_ms = static_cast<uint32_t>(absI(delta_cdeg)) / 2u + motion::kTimeoutBaseMs;  // |angle| / 20 °/s + 3 s
  m_coast_ms = coast_ms;
  m_phase = absI(delta_cdeg) <= motion::kTurnSlowZoneCdeg ? TurnPhase::kSlow : TurnPhase::kApproach;
  m_dir = delta_cdeg < 0 ? -1 : 1;
  m_rate.reset();
  m_cut_pos = 0;
  m_cut_rate = 0;
  m_stalling = false;
}

int8_t TurnController::command(int32_t error_cdeg, int16_t rate_ddps) const {
  const int32_t ep = error_cdeg - static_cast<int32_t>(rate_ddps) * motion::kLatencyDdpsToCdeg / 10;
  const int32_t e = ep != 0 ? ep : error_cdeg;
  int32_t mag = absI(e) * motion::kTurnKpPctPerDeg / 100;
  if (mag < motion::kTurnWMin) mag = motion::kTurnWMin;
  if (mag > m_speed) mag = m_speed;
  return static_cast<int8_t>(e < 0 ? -mag : mag);
}

// R18: w = 0 once the tank would stop on (or past) the target if the power were cut now - along the way it
// drives (m_dir), so a nudge back after an overshoot cuts on its own crossing. Within 3° and not turning it is
// already there: the slowest spin would glide past by more than that.
bool TurnController::cutNow(int32_t error_cdeg, int16_t rate_ddps) const {
  const int16_t r = rate_ddps < 0 ? static_cast<int16_t>(-rate_ddps) : rate_ddps;
  if (absI(error_cdeg) <= motion::kTurnDoneCdeg && r < motion::kCoastMinRateDdps) return true;
  const int32_t left = error_cdeg - static_cast<int32_t>(rate_ddps) * m_coast_ms / 100;  // 0.1 °/s x ms -> 0.01°
  return m_dir > 0 ? left <= 0 : left >= 0;
}

void TurnController::cut(int16_t rate_ddps, uint32_t now_ms) {
  if (m_phase != TurnPhase::kNudge) {  // the first cut is the one the coast is learned from
    m_cut_pos = m_pos;
    m_cut_rate = rate_ddps;
  }
  m_phase = m_phase == TurnPhase::kNudge ? TurnPhase::kNudgeCoast : TurnPhase::kCoast;
  m_still_since = now_ms;
  m_still_pos = m_pos;
}

// Stopped: held for kWindowHoldMs with < kSettleMoveCdeg net movement (the heading, not the shaking gyro rate).
bool TurnController::stopped(uint32_t now_ms) {
  if (now_ms - m_still_since < motion::kWindowHoldMs) return false;
  if (absI(m_pos - m_still_pos) < motion::kSettleMoveCdeg) return true;
  m_still_since = now_ms;
  m_still_pos = m_pos;
  return false;
}

// T_coast (ms) seen on this turn: how far it went on after the first cut, over the rate at the cut.
uint16_t TurnController::coastReading() const {
  const int32_t rate = m_cut_rate < 0 ? -m_cut_rate : m_cut_rate;
  if (rate < motion::kCoastMinRateDdps) return 0;
  const int32_t coast = (m_delta >= 0 ? m_pos - m_cut_pos : m_cut_pos - m_pos);  // along the turn
  const int32_t ms = coast * 100 / rate;
  return ms >= motion::kCoastMinMs && ms <= motion::kCoastMaxMs ? static_cast<uint16_t>(ms) : 0;
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

void TurnController::driven(TurnStep& s, int32_t error, int16_t rate_ddps, uint32_t now_ms) {
  if (m_phase == TurnPhase::kApproach && absI(error) <= motion::kTurnSlowZoneCdeg) {
    m_phase = TurnPhase::kSlow;  // the last 25°: rate control at the slowest smooth spin
    m_rate.reset();
  }
  if (m_phase != TurnPhase::kApproach && cutNow(error, rate_ddps)) {
    cut(rate_ddps, now_ms);
    return;  // w = 0: glide onto the target
  }
  if (m_phase == TurnPhase::kApproach) {
    s.w = command(error, rate_ddps);
  } else {
    const int16_t target = static_cast<int16_t>(m_dir * motion::kSpinMinDdps);
    const uint32_t dt = now_ms - m_last_ms;
    // kept to the asked speed like the approach, never below the smallest command that turns (R18 review)
    const uint8_t top = m_speed > 100 ? 100 : m_speed;
    const int8_t cap = static_cast<int8_t>(top < motion::kTurnWMin ? motion::kTurnWMin : top);
    s.w = m_rate.step(target, rate_ddps, static_cast<uint16_t>(dt > 100 ? 100 : dt), cap);
  }
  s.stalled = stalledFor(s.w, rate_ddps, now_ms);
}

void TurnController::coasting(TurnStep& s, int32_t error, uint32_t now_ms) {
  m_stalling = false;
  if (!stopped(now_ms)) return;  // w = 0 while it glides
  if (m_phase == TurnPhase::kCoast) {
    s.coast_ms = coastReading();
    if (absI(error) > motion::kTurnDoneCdeg) {
      m_phase = TurnPhase::kNudge;  // stopped too far off: one nudge, never a second
      m_dir = error < 0 ? -1 : 1;
      m_rate.reset();
      return;
    }
  }
  s.done = true;
}

TurnStep TurnController::step(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms) {
  m_pos += wrapDeltaCdeg(static_cast<int32_t>(yaw_cdeg) - m_last_yaw);
  m_last_yaw = yaw_cdeg;
  const int32_t error = m_delta - m_pos;
  TurnStep s;
  s.timeout = now_ms - m_start_ms >= m_timeout_ms;
  if (m_phase == TurnPhase::kCoast || m_phase == TurnPhase::kNudgeCoast) {
    coasting(s, error, now_ms);
  } else {
    driven(s, error, rate_ddps, now_ms);
  }
  m_last_ms = now_ms;
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

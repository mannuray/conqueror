#include "core/move_controller.h"

#include "core/motion_config.h"
#include "cq_protocol.h"

namespace cq {
namespace core {

void MoveController::begin(int32_t enc_l, int32_t enc_r, int16_t dist_cm, uint8_t speed_pct, uint16_t cpm,
                           uint32_t now_ms, uint16_t roll_mm) {
  const int32_t cm = dist_cm < 0 ? -dist_cm : dist_cm;
  m_start_l = enc_l;
  m_start_r = enc_r;
  m_dir = dist_cm < 0 ? -1 : 1;
  m_cpm = cpm;
  m_target = cm * cpm / 100;
  if (m_target < 1) m_target = 1;
  m_stop_at = m_target - static_cast<int32_t>(roll_mm) * cpm / 1000;  // the tracks roll on into the target
  if (m_stop_at < m_target / 2) m_stop_at = m_target / 2;
  if (m_stop_at < 1) m_stop_at = 1;
  m_slow = motion::kMoveSlowCm * cpm / 100;
  m_stall_counts = motion::kMoveStallCm * cpm / 100;
  m_speed = speed_pct;
  m_done = 0;
  m_ref = 0;
  m_ref_ms = now_ms;
  m_deadline = now_ms + static_cast<uint32_t>(cm) * 1000u / motion::kMoveTimeoutCmPerS + motion::kMoveTimeoutBaseMs;
  m_blocked = false;
  m_slip.reset();
}

// The obstacle brake has held the move for kMoveBlockedMs.
bool MoveController::blockedTooLong(const MotionTelem& t, uint32_t now_ms) {
  if (((t.flags >> static_cast<unsigned>(proto::FastFlagBit::kBlocked)) & 1u) == 0) {
    m_blocked = false;
    return false;
  }
  if (!m_blocked) m_blocked_since = now_ms;
  m_blocked = true;
  m_ref_ms = now_ms;  // an obstacle ends as blocked, never as stalled
  return now_ms - m_blocked_since >= motion::kMoveBlockedMs;
}

// No progress for kMoveStallMs: stalled, or blocked when something is close ahead.
bool MoveController::stuck(const MotionTelem& t, uint32_t now_ms, MoveStep& s) {
  if (m_done - m_ref >= m_stall_counts) {
    m_ref = m_done;
    m_ref_ms = now_ms;
    return false;
  }
  if (now_ms - m_ref_ms < motion::kMoveStallMs) return false;
  s.blocked = m_dir > 0 && t.dist_mm < motion::kMoveObstacleMm;  // the sensor looks forward
  s.stalled = !s.blocked;
  return true;
}

int8_t MoveController::rampedV() const {  // slow down toward the stop point (remaining > 0 here)
  const int32_t crawl = m_speed < motion::kMoveCrawlPct ? m_speed : motion::kMoveCrawlPct;
  const int32_t remaining = m_stop_at - m_done;
  int32_t mag = m_speed;
  if (remaining < m_slow) mag = crawl + (m_speed - crawl) * remaining / m_slow;
  return static_cast<int8_t>(m_dir * mag);
}

MoveStep MoveController::step(const MotionTelem& t, uint32_t now_ms) {
  MoveStep s;
  // The Uno counts rising edges only: the counts rise forward and reverse (final review C1), so the distance is the
  // raw mean delta - never its absolute value: after an Uno restart the counts drop and the move must stall, not
  // report a false done.
  m_done = ((t.enc_l - m_start_l) + (t.enc_r - m_start_r)) / 2;
  if (m_done >= m_stop_at) {
    s.done = true;
    return s;
  }
  if (blockedTooLong(t, now_ms) || m_slip.step(t.enc_l, t.enc_r, t.rate_ddps, m_cpm, m_dir, now_ms)) {
    s.blocked = true;  // the obstacle brake, or a slipping track the gyro does not see (R21)
    return s;
  }
  if (stuck(t, now_ms, s)) return s;
  if (static_cast<int32_t>(now_ms - m_deadline) >= 0) {
    s.timeout = true;
    return s;
  }
  s.v = rampedV();
  return s;
}

uint8_t MoveController::progressPct() const {
  if (m_done <= 0) return 0;
  if (m_done >= m_target) return 100;
  return static_cast<uint8_t>(m_done * 100 / m_target);
}

int32_t MoveController::movedMm() const { return m_dir * m_done * 1000 / m_cpm; }

}  // namespace core
}  // namespace cq

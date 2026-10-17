#include "core/move_controller.h"

#include "core/motion_config.h"
#include "cq_protocol.h"

namespace cq {
namespace core {

void MoveController::begin(int32_t enc_l, int32_t enc_r, int16_t dist_cm, uint8_t speed_pct, uint16_t cpm,
                           uint32_t now_ms) {
  const int32_t cm = dist_cm < 0 ? -dist_cm : dist_cm;
  m_start_l = enc_l;
  m_start_r = enc_r;
  m_dir = dist_cm < 0 ? -1 : 1;
  m_cpm = cpm;
  m_target = cm * cpm / 100;
  if (m_target < 1) m_target = 1;
  m_slow = motion::kMoveSlowCm * cpm / 100;
  m_stall_counts = motion::kMoveStallCm * cpm / 100;
  m_speed = speed_pct;
  m_done = 0;
  m_ref = 0;
  m_ref_ms = now_ms;
  m_deadline = now_ms + static_cast<uint32_t>(cm) * 1000u / motion::kMoveTimeoutCmPerS + motion::kMoveTimeoutBaseMs;
  m_blocked = false;
}

MoveStep MoveController::step(const MotionTelem& t, uint32_t now_ms) {
  MoveStep s;
  m_done = m_dir * ((t.enc_l - m_start_l) + (t.enc_r - m_start_r)) / 2;
  if (m_done >= m_target) {
    s.done = true;
    return s;
  }
  const bool blocked = ((t.flags >> static_cast<unsigned>(proto::FastFlagBit::kBlocked)) & 1u) != 0;
  if (blocked) {
    if (!m_blocked) m_blocked_since = now_ms;
    m_blocked = true;
    m_ref_ms = now_ms;  // an obstacle ends as blocked, never as stalled
    if (now_ms - m_blocked_since >= motion::kMoveBlockedMs) {
      s.blocked = true;
      return s;
    }
  } else {
    m_blocked = false;
  }
  if (m_done - m_ref >= m_stall_counts) {
    m_ref = m_done;
    m_ref_ms = now_ms;
  } else if (now_ms - m_ref_ms >= motion::kMoveStallMs) {
    s.stalled = true;
    return s;
  }
  if (static_cast<int32_t>(now_ms - m_deadline) >= 0) {
    s.timeout = true;
    return s;
  }
  const int32_t crawl = m_speed < motion::kMoveCrawlPct ? m_speed : motion::kMoveCrawlPct;
  const int32_t remaining = m_target - m_done;  // > 0 here
  int32_t mag = m_speed;
  if (remaining < m_slow) mag = crawl + (m_speed - crawl) * remaining / m_slow;
  s.v = static_cast<int8_t>(m_dir * mag);
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

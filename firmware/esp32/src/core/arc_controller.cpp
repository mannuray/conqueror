#include "core/arc_controller.h"

#include "core/motion_config.h"
#include "core/turn_controller.h"
#include "cq_protocol.h"

namespace cq {
namespace core {

void ArcController::begin(const MotionTelem& t, int32_t delta_cdeg, uint16_t radius_cm, uint8_t speed_pct,
                          uint16_t cpm, uint32_t now_ms) {
  m_rate.reset();
  m_rate.setFeedForward(motion::kArcFfPctPerDps);
  m_delta = delta_cdeg;
  m_pos = 0;
  m_dir = delta_cdeg < 0 ? -1 : 1;
  m_last_yaw = t.yaw_cdeg;
  m_radius = radius_cm == 0 ? 1 : radius_cm;
  m_cpm = cpm;
  m_speed = speed_pct;
  m_target_ddps = 0;
  m_finished = false;
  m_last_ms = now_ms;
  m_hist_n = 0;
  m_hist_head = 0;
  const int32_t counts = (t.enc_l + t.enc_r) / 2;
  m_ref_counts = counts;
  m_ref_pos = 0;
  m_ref_ms = now_ms;
  m_stall_counts = cpm / 100 > 0 ? cpm / 100 : 1;  // 1 cm
  m_blocked = false;
  m_wrong = false;
  // timeout: arc length R·|θ| / 5 cm/s + 3 s, |θ| = |delta| / 100 · π / 180
  const int64_t a = delta_cdeg < 0 ? -static_cast<int64_t>(delta_cdeg) : delta_cdeg;
  const uint32_t len_cm = static_cast<uint32_t>(static_cast<int64_t>(m_radius) * a * 31416 / 180000000);
  m_deadline = now_ms + len_cm * 1000u / motion::kArcTimeoutCmPerS + motion::kArcTimeoutBaseMs;
  speedCmS10(counts, now_ms);  // first history entry
}

int32_t ArcController::speedCmS10(int32_t counts, uint32_t now_ms) {
  m_hist_counts[m_hist_head] = counts;
  m_hist_ms[m_hist_head] = now_ms;
  m_hist_head = static_cast<uint8_t>((m_hist_head + 1) % kHist);
  if (m_hist_n < kHist) ++m_hist_n;
  uint8_t pick = static_cast<uint8_t>((m_hist_head + kHist - m_hist_n) % kHist);  // oldest
  for (uint8_t i = 1; i < m_hist_n; ++i) {  // newest entry at least the window back
    const uint8_t j = static_cast<uint8_t>((m_hist_head + kHist - 1 - i) % kHist);
    if (now_ms - m_hist_ms[j] >= motion::kArcSpeedWindowMs) {
      pick = j;
      break;
    }
  }
  const uint32_t dt = now_ms - m_hist_ms[pick];
  const int64_t d = static_cast<int64_t>(counts) - m_hist_counts[pick];
  if (dt == 0 || d <= 0) return 0;  // counts never fall while driving: a drop is an Uno restart (review focus 1)
  return static_cast<int32_t>(d * 1000000 / (static_cast<int64_t>(dt) * m_cpm));
}

ArcStep ArcController::step(const MotionTelem& t, uint32_t now_ms) {
  ArcStep s;
  m_pos += wrapDeltaCdeg(static_cast<int32_t>(t.yaw_cdeg) - static_cast<int32_t>(m_last_yaw));
  m_last_yaw = t.yaw_cdeg;
  const int32_t to_go = m_dir * (m_delta - m_pos);
  const int32_t rate = t.rate_ddps < 0 ? -t.rate_ddps : t.rate_ddps;
  if (to_go <= rate * motion::kLatencyDdpsToCdeg / 10) {  // the heading still to go is covered by the lag
    s.done = true;
    m_finished = true;
    return s;
  }
  const int32_t counts = (t.enc_l + t.enc_r) / 2;
  const int32_t v10 = speedCmS10(counts, now_ms);
  const bool blocked_flag = ((t.flags >> static_cast<unsigned>(proto::FastFlagBit::kBlocked)) & 1u) != 0;
  if (blocked_flag) {
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
  const int32_t dpos = m_pos - m_ref_pos;
  if (counts - m_ref_counts >= m_stall_counts || dpos >= motion::kArcStallCdeg || dpos <= -motion::kArcStallCdeg) {
    m_ref_counts = counts;
    m_ref_pos = m_pos;
    m_ref_ms = now_ms;
  } else if (now_ms - m_ref_ms >= motion::kArcStallMs) {
    if (t.dist_mm < motion::kMoveObstacleMm) {
      s.blocked = true;
    } else {
      s.stalled = true;
    }
    return s;
  }
  if (static_cast<int32_t>(now_ms - m_deadline) >= 0) {
    s.timeout = true;
    return s;
  }
  uint32_t dt = now_ms - m_last_ms;
  if (dt == 0 || dt > 100) dt = 20;
  m_last_ms = now_ms;
  m_target_ddps = static_cast<int16_t>(m_dir * 573 * v10 / (10 * static_cast<int32_t>(m_radius)));  // 57.3 v / R
  s.w = m_rate.step(m_target_ddps, t.rate_ddps, static_cast<uint16_t>(dt));
  s.v = static_cast<int8_t>(m_speed);
  const bool saturated = s.w >= motion::kRateSaturatedPct || s.w <= -motion::kRateSaturatedPct;
  const bool opposite = (m_dir > 0 && t.rate_ddps < -motion::kRateWrongWayDdps) ||
                        (m_dir < 0 && t.rate_ddps > motion::kRateWrongWayDdps);
  if (saturated && opposite) {
    if (!m_wrong) m_wrong_since = now_ms;
    m_wrong = true;
    if (now_ms - m_wrong_since >= motion::kRateWrongWayMs) {
      s.diverged = true;
      s.v = 0;
      s.w = 0;
    }
  } else {
    m_wrong = false;
  }
  return s;
}

uint8_t ArcController::progressPct() const {
  if (m_delta == 0 || m_finished) return 100;
  const int32_t p = m_pos * 100 / m_delta;
  return static_cast<uint8_t>(p < 0 ? 0 : (p > 100 ? 100 : p));
}

}  // namespace core
}  // namespace cq

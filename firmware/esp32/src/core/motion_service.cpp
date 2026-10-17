#include "core/motion_service.h"

#include "core/motion_config.h"

namespace cq {
namespace core {

void MotionService::startTurn(int fd, uint16_t yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, int32_t id,
                              uint32_t now_ms) {
  m_kind = PrimKind::kTurn;
  m_fd = fd;
  m_id = id;
  m_turn.begin(yaw_cdeg, delta_cdeg, speed_pct, now_ms);
  m_last_telem_ms = now_ms;
}

void MotionService::driveRate(int fd, int8_t v, int16_t rate_ddps, uint32_t now_ms) {
  if (m_kind != PrimKind::kRate || m_fd != fd) {
    m_kind = PrimKind::kRate;
    m_fd = fd;
    m_id = -1;
    m_rate.reset();
    m_wrong_way = false;
    m_rate_stalling = false;
    m_last_telem_ms = now_ms;
  }
  // Plan L (spec 02 §H6): a spin slower than 65 °/s cannot run smoothly - clamp it and say so.
  const int16_t mag = rate_ddps < 0 ? static_cast<int16_t>(-rate_ddps) : rate_ddps;
  m_rate_limited = v == 0 && mag > 0 && mag < motion::kSpinMinDdps;
  if (m_rate_limited) rate_ddps = rate_ddps < 0 ? static_cast<int16_t>(-motion::kSpinMinDdps) : motion::kSpinMinDdps;
  m_rate_v = v;
  m_rate_target = rate_ddps;
  m_rate_update_ms = now_ms;
}

void MotionService::startMove(int fd, const MotionTelem& t, int16_t dist_cm, uint8_t speed_pct, uint16_t cpm,
                              int32_t id, uint32_t now_ms, uint16_t roll_mm) {
  m_kind = PrimKind::kMove;
  m_fd = fd;
  m_id = id;
  m_move.begin(t.enc_l, t.enc_r, dist_cm, speed_pct, cpm, now_ms, roll_mm);
  m_last_telem_ms = now_ms;
}

void MotionService::startArc(int fd, const MotionTelem& t, int32_t delta_cdeg, uint16_t radius_cm, uint8_t speed_pct,
                             uint16_t cpm, int32_t id, uint32_t now_ms) {
  m_kind = PrimKind::kArc;
  m_fd = fd;
  m_id = id;
  m_arc.begin(t, delta_cdeg, radius_cm, speed_pct, cpm, now_ms);
  m_last_telem_ms = now_ms;
}

void MotionService::startPivot(int fd, uint16_t yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, int32_t id,
                               uint32_t now_ms) {
  m_kind = PrimKind::kPivot;
  m_fd = fd;
  m_id = id;
  // outside track = 2|w| <= speed; the spin's minimum w still applies (12 % -> outside 24 %)
  uint8_t half = static_cast<uint8_t>(speed_pct / 2);
  if (half < motion::kTurnWMin) half = static_cast<uint8_t>(motion::kTurnWMin);
  m_turn.begin(yaw_cdeg, delta_cdeg, half, now_ms);
  m_last_telem_ms = now_ms;
}

MotionOut MotionService::stepArc(const MotionTelem& t, uint32_t now_ms) {
  const ArcStep s = m_arc.step(t, now_ms);
  if (s.done) return finish(EndReason::kDone);
  if (s.stalled) return finish(EndReason::kStalled);
  if (s.blocked) return finish(EndReason::kBlocked);
  if (s.timeout) return finish(EndReason::kTimeout);
  if (s.diverged) return finish(EndReason::kDiverged);
  MotionOut o;
  o.send = true;
  o.v = s.v;
  o.w = s.w;
  return o;
}

MotionOut MotionService::stepPivot(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms) {
  const TurnStep s = m_turn.step(yaw_cdeg, rate_ddps, now_ms);
  if (s.done) return finish(EndReason::kDone);
  if (s.stalled) return finish(EndReason::kStalled);
  if (s.timeout) return finish(EndReason::kTimeout);
  MotionOut o;
  o.send = true;
  o.w = s.w;
  o.v = static_cast<int8_t>(s.w < 0 ? -s.w : s.w);  // plain mix: the inside track gets v - |w| = 0
  return o;
}

MotionOut MotionService::finish(EndReason why) {
  MotionOut o;
  o.send = true;  // v = 0, w = 0
  o.ended = true;
  o.end.kind = m_kind;
  o.end.ok = why == EndReason::kDone;
  o.end.reason = why;
  o.end.id = m_id;
  if (m_kind == PrimKind::kMove) {
    o.end.has_moved = true;
    o.end.moved_mm = m_move.movedMm();
  }
  m_kind = PrimKind::kNone;
  m_fd = -1;
  m_id = -1;
  return o;
}

MotionOut MotionService::cancel(EndReason why) { return active() ? finish(why) : MotionOut{}; }

MotionOut MotionService::stepTurn(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms) {
  const TurnStep s = m_turn.step(yaw_cdeg, rate_ddps, now_ms);
  if (s.done) return finish(EndReason::kDone);
  if (s.stalled) return finish(EndReason::kStalled);
  if (s.timeout) return finish(EndReason::kTimeout);
  MotionOut o;
  o.send = true;
  o.w = s.w;
  return o;
}

MotionOut MotionService::stepRate(int16_t rate_ddps, uint32_t now_ms) {
  if (now_ms - m_rate_update_ms > motion::kRateDeadmanMs) return finish(EndReason::kDeadman);
  uint32_t dt = now_ms - m_last_telem_ms;
  if (dt == 0 || dt > 100) dt = 20;  // first frame / after a gap: one nominal period
  MotionOut o;
  o.send = true;
  o.v = m_rate_v;
  o.w = m_rate.step(m_rate_target, rate_ddps, static_cast<uint16_t>(dt));
  // Review C1 guard: full power while the gyro sees the opposite turn = the loop runs away (e.g. a body that
  // flips w when reversing) - end it instead of spinning faster and faster.
  const bool saturated = o.w >= motion::kRateSaturatedPct || o.w <= -motion::kRateSaturatedPct;
  const bool opposite = (m_rate_target > 0 && rate_ddps < -motion::kRateWrongWayDdps) ||
                        (m_rate_target < 0 && rate_ddps > motion::kRateWrongWayDdps);
  if (saturated && opposite) {
    if (!m_wrong_way) m_wrong_since = now_ms;
    m_wrong_way = true;
    if (now_ms - m_wrong_since >= motion::kRateWrongWayMs) return finish(EndReason::kDiverged);
  } else {
    m_wrong_way = false;
  }
  // Final review I1: full power while the gyro sees no turn (jammed track, or the Uno's safety gate cutting the
  // PWM) - end "stalled" as TURN does, never hold a stalled track (design-notes §56).
  const int16_t r = rate_ddps < 0 ? static_cast<int16_t>(-rate_ddps) : rate_ddps;
  if (saturated && r < motion::kStallRateDdps) {
    if (!m_rate_stalling) m_rate_stall_since = now_ms;
    m_rate_stalling = true;
    if (now_ms - m_rate_stall_since >= motion::kStallMs) return finish(EndReason::kStalled);
  } else {
    m_rate_stalling = false;
  }
  return o;
}

MotionOut MotionService::stepMove(const MotionTelem& t, uint32_t now_ms) {
  const MoveStep s = m_move.step(t, now_ms);
  if (s.done) return finish(EndReason::kDone);
  if (s.stalled) return finish(EndReason::kStalled);
  if (s.blocked) return finish(EndReason::kBlocked);
  if (s.timeout) return finish(EndReason::kTimeout);
  MotionOut o;
  o.send = true;
  o.v = s.v;  // w = 0: the Uno's heading hold keeps it straight
  return o;
}

MotionOut MotionService::onTelemetry(const MotionTelem& t, uint32_t now_ms) {
  if (!active()) return MotionOut{};
  MotionOut o;
  switch (m_kind) {
    case PrimKind::kTurn: o = stepTurn(t.yaw_cdeg, t.rate_ddps, now_ms); break;
    case PrimKind::kMove: o = stepMove(t, now_ms); break;
    case PrimKind::kArc: o = stepArc(t, now_ms); break;
    case PrimKind::kPivot: o = stepPivot(t.yaw_cdeg, t.rate_ddps, now_ms); break;
    default: o = stepRate(t.rate_ddps, now_ms); break;
  }
  m_last_telem_ms = now_ms;
  return o;
}

MotionOut MotionService::tick(uint32_t now_ms) {
  if (!active()) return MotionOut{};
  if (now_ms - m_last_telem_ms > motion::kTelemGapMs) return finish(EndReason::kLink);
  if (m_kind == PrimKind::kRate && now_ms - m_rate_update_ms > motion::kRateDeadmanMs) {
    return finish(EndReason::kDeadman);
  }
  return MotionOut{};
}

uint8_t MotionService::progressPct() const {
  if (m_kind == PrimKind::kTurn) return m_turn.progressPct();
  if (m_kind == PrimKind::kMove) return m_move.progressPct();
  if (m_kind == PrimKind::kArc) return m_arc.progressPct();
  if (m_kind == PrimKind::kPivot) return m_turn.progressPct();
  return 0;
}

}  // namespace core
}  // namespace cq

#include "core/motion_service.h"

#include "core/motion_config.h"
#include "cq_protocol.h"

namespace cq {
namespace core {
namespace {
constexpr int32_t kGoProgressCounts = motion::kCpmDefault / 100;  // 1 cm (R20)
constexpr uint16_t kGoProgressMm = 10;                            // 1 cm closer
}  // namespace

void MotionService::startTurn(int fd, uint16_t yaw_cdeg, int32_t delta_cdeg, uint8_t speed_pct, int32_t id,
                              uint32_t now_ms, uint16_t coast_ms) {
  m_kind = PrimKind::kTurn;
  m_fd = fd;
  m_id = id;
  m_coast_reading = 0;
  m_turn.begin(yaw_cdeg, delta_cdeg, speed_pct, now_ms, coast_ms);
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
                               uint32_t now_ms, uint16_t coast_ms) {
  m_kind = PrimKind::kPivot;
  m_fd = fd;
  m_id = id;
  m_pivot_dir = delta_cdeg < 0 ? -1 : 1;
  // outside track = 2|w| <= speed; the spin's minimum w still applies (12 % -> outside 24 %)
  uint8_t half = static_cast<uint8_t>(speed_pct / 2);
  if (half < motion::kTurnWMin) half = static_cast<uint8_t>(motion::kTurnWMin);
  m_coast_reading = 0;
  m_turn.begin(yaw_cdeg, delta_cdeg, half, now_ms, coast_ms);
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
  if (s.coast_ms != 0) m_coast_reading = s.coast_ms;
  if (s.done) return finish(EndReason::kDone);
  if (s.stalled) return finish(EndReason::kStalled);
  if (s.timeout) return finish(EndReason::kTimeout);
  MotionOut o;
  o.send = true;
  o.w = s.w;
  // Plain mix L = v - w, R = v + w: a left pivot holds L = 0 (v = w), a right pivot R = 0 (v = -w) - also while
  // correcting back past the target, when the outside track reverses (floor 2026-10-18, review M5).
  o.v = static_cast<int8_t>(m_pivot_dir > 0 ? s.w : -s.w);
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
  if (m_kind == PrimKind::kTurn || m_kind == PrimKind::kPivot) o.end.coast_ms = m_coast_reading;
  m_kind = PrimKind::kNone;
  m_fd = -1;
  m_id = -1;
  return o;
}

MotionOut MotionService::cancel(EndReason why) { return active() ? finish(why) : MotionOut{}; }

void MotionService::startGo(int fd, uint8_t speed_pct, int32_t id, uint32_t now_ms, uint16_t cpm) {
  if (m_kind != PrimKind::kGo || m_fd != fd) {
    m_kind = PrimKind::kGo;
    m_fd = fd;
    m_id = id;
    m_last_telem_ms = now_ms;
    m_go_ref_set = false;
    m_go_slip.reset();
  }
  m_go_cpm = cpm;
  m_go_v = static_cast<int8_t>(speed_pct > 100 ? 100 : speed_pct);
  m_go_update_ms = now_ms;
}

MotionOut MotionService::hold() { return m_kind == PrimKind::kGo ? finish(EndReason::kDone) : MotionOut{}; }

// R20: no progress - neither the counts (1 cm) nor the distance ahead (1 cm) - for kMoveStallMs.
bool MotionService::goStopped(const MotionTelem& t, uint32_t now_ms) {
  const int32_t counts = (t.enc_l + t.enc_r) / 2;
  const bool seen = t.dist_mm < 65534;  // 65535 no echo, 65534 sensor fault
  const int32_t moved = counts - m_go_ref_counts;
  const bool progress = !m_go_ref_set || (moved < 0 ? -moved : moved) >= kGoProgressCounts ||
                        (seen && m_go_ref_dist >= t.dist_mm + kGoProgressMm);
  if (progress) {
    m_go_ref_set = true;
    m_go_ref_counts = counts;
    m_go_ref_dist = t.dist_mm;
    m_go_ref_ms = now_ms;
    return false;
  }
  return now_ms - m_go_ref_ms >= motion::kMoveStallMs;
}

// R20: no target - ends when something is in the way, as a MOVE does: at once on the Uno's obstacle-brake flag,
// or after kMoveStallMs without progress with an obstacle closer than kMoveObstacleMm (the Uno's slow-down stops
// the tank without its flag, floor 2026-10-17; final review C1).
MotionOut MotionService::stepGo(const MotionTelem& t, uint32_t now_ms) {
  if (now_ms - m_go_update_ms > motion::kRateDeadmanMs) return finish(EndReason::kDeadman);
  const bool first = !m_go_ref_set;  // its flag may still be the last primitive's (final review M1)
  if (!first && ((t.flags >> static_cast<unsigned>(proto::FastFlagBit::kBlocked)) & 1u) != 0) {
    return finish(EndReason::kBlocked);
  }
  if (goStopped(t, now_ms) && t.dist_mm < motion::kMoveObstacleMm) return finish(EndReason::kBlocked);
  if (m_go_slip.step(t.enc_l, t.enc_r, t.rate_ddps, m_go_cpm, 1, now_ms)) return finish(EndReason::kBlocked);  // R21
  MotionOut o;
  o.send = true;
  o.v = m_go_v;  // w = 0: the Uno's heading hold keeps it straight
  return o;
}

MotionOut MotionService::stepTurn(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms) {
  const TurnStep s = m_turn.step(yaw_cdeg, rate_ddps, now_ms);
  if (s.coast_ms != 0) m_coast_reading = s.coast_ms;
  if (s.done) return finish(EndReason::kDone);
  if (s.stalled) return finish(EndReason::kStalled);
  if (s.timeout) return finish(EndReason::kTimeout);
  MotionOut o;
  o.send = true;
  o.w = s.w;
  return o;
}

// Review C1 guard: full power while the gyro sees the opposite turn = the loop runs away (e.g. a body that flips w
// when reversing) - true once that lasted kRateWrongWayMs.
bool MotionService::rateDiverged(bool saturated, int16_t rate_ddps, uint32_t now_ms) {
  const bool opposite = (m_rate_target > 0 && rate_ddps < -motion::kRateWrongWayDdps) ||
                        (m_rate_target < 0 && rate_ddps > motion::kRateWrongWayDdps);
  if (!(saturated && opposite)) {
    m_wrong_way = false;
    return false;
  }
  if (!m_wrong_way) m_wrong_since = now_ms;
  m_wrong_way = true;
  return now_ms - m_wrong_since >= motion::kRateWrongWayMs;
}

// Final review I1: full power while the gyro sees no turn (jammed track, or the Uno's safety gate cutting the PWM) -
// true once that lasted kStallMs (design-notes §56).
bool MotionService::rateStalled(bool saturated, int16_t rate_ddps, uint32_t now_ms) {
  const int16_t r = rate_ddps < 0 ? static_cast<int16_t>(-rate_ddps) : rate_ddps;
  if (!(saturated && r < motion::kStallRateDdps)) {
    m_rate_stalling = false;
    return false;
  }
  if (!m_rate_stalling) m_rate_stall_since = now_ms;
  m_rate_stalling = true;
  return now_ms - m_rate_stall_since >= motion::kStallMs;
}

MotionOut MotionService::stepRate(int16_t rate_ddps, uint32_t now_ms) {
  if (now_ms - m_rate_update_ms > motion::kRateDeadmanMs) return finish(EndReason::kDeadman);
  uint32_t dt = now_ms - m_last_telem_ms;
  if (dt == 0 || dt > 100) dt = 20;  // first frame / after a gap: one nominal period
  MotionOut o;
  o.send = true;
  o.v = m_rate_v;
  o.w = m_rate.step(m_rate_target, rate_ddps, static_cast<uint16_t>(dt));
  const bool saturated = o.w >= motion::kRateSaturatedPct || o.w <= -motion::kRateSaturatedPct;
  if (rateDiverged(saturated, rate_ddps, now_ms)) return finish(EndReason::kDiverged);
  if (rateStalled(saturated, rate_ddps, now_ms)) return finish(EndReason::kStalled);
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
    case PrimKind::kGo: o = stepGo(t, now_ms); break;
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
  if (m_kind == PrimKind::kGo && now_ms - m_go_update_ms > motion::kRateDeadmanMs) return finish(EndReason::kDeadman);
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

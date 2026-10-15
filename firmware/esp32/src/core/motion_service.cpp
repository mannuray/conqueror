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
    m_last_telem_ms = now_ms;
  }
  m_rate_v = v;
  m_rate_target = rate_ddps;
  m_rate_update_ms = now_ms;
}

MotionOut MotionService::finish(EndReason why) {
  MotionOut o;
  o.send = true;  // v = 0, w = 0
  o.ended = true;
  o.end.kind = m_kind;
  o.end.ok = why == EndReason::kDone;
  o.end.reason = why;
  o.end.id = m_id;
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
  return o;
}

MotionOut MotionService::onTelemetry(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms) {
  if (!active()) return MotionOut{};
  const MotionOut o =
      m_kind == PrimKind::kTurn ? stepTurn(yaw_cdeg, rate_ddps, now_ms) : stepRate(rate_ddps, now_ms);
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

uint8_t MotionService::progressPct() const { return m_kind == PrimKind::kTurn ? m_turn.progressPct() : 0; }

}  // namespace core
}  // namespace cq

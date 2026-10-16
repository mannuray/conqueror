#include "core/motion_link.h"

namespace cq {
namespace core {
namespace {
EndReason reasonFor(Decision d) { return d == Decision::kNackBodyOffline ? EndReason::kLink : EndReason::kNotOwner; }

LinkOut eventOnly(const MotionOut& o) {
  LinkOut out;
  out.event = o.ended;
  out.end = o.end;
  return out;
}
}  // namespace

LinkOut MotionLink::finished(const MotionOut& o, int fd, uint32_t now_ms) {
  LinkOut out = eventOnly(o);
  out.stop = m_owner.keepDriving(fd, now_ms) == Decision::kSend;  // STOP only while still the owner (review M3)
  return out;
}

LinkOut MotionLink::onTelemetry(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms) {
  if (!m_svc.active()) return LinkOut{};
  const int fd = m_svc.ownerFd();
  const MotionOut o = m_svc.onTelemetry(yaw_cdeg, rate_ddps, now_ms);
  if (o.ended) return finished(o, fd, now_ms);
  if (!o.send) return LinkOut{};
  const Decision d = m_owner.keepDriving(fd, now_ms);  // never takes ownership (review I1)
  if (d != Decision::kSend) return eventOnly(m_svc.cancel(reasonFor(d)));  // no frames: someone else drives
  LinkOut out;
  out.drive = true;
  out.v = o.v;
  out.w = o.w;
  return out;
}

LinkOut MotionLink::tick(uint32_t now_ms) {
  if (!m_svc.active()) return LinkOut{};
  const int fd = m_svc.ownerFd();
  const MotionOut o = m_svc.tick(now_ms);
  return o.ended ? finished(o, fd, now_ms) : LinkOut{};
}

LinkOut MotionLink::onEvent(MotionEvent e, uint32_t now_ms) {
  if (!m_svc.active()) return LinkOut{};
  const int fd = m_svc.ownerFd();
  const EndReason why = e == MotionEvent::kStop          ? EndReason::kStop
                        : e == MotionEvent::kBodyOffline ? EndReason::kLink
                                                         : EndReason::kCancelled;
  if (e == MotionEvent::kCancel) return finished(m_svc.cancel(why), fd, now_ms);
  return eventOnly(m_svc.cancel(why));  // the STOP is already on its way, or the owner's own frames follow
}

}  // namespace core
}  // namespace cq

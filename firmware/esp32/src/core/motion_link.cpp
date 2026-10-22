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

LinkOut MotionLink::onTelemetry(const MotionTelem& t, uint32_t now_ms) {
  if (!m_svc.active()) return LinkOut{};
  const int fd = m_svc.ownerFd();
  const MotionOut o = m_svc.onTelemetry(t, now_ms);
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

namespace {
EndReason reasonOf(MotionEvent e) {
  if (e == MotionEvent::kStop) return EndReason::kStop;
  return e == MotionEvent::kBodyOffline ? EndReason::kLink : EndReason::kCancelled;
}
}  // namespace

LinkOut MotionLink::onEvent(MotionEvent e, uint32_t now_ms) {
  if (!m_svc.active()) return LinkOut{};
  const int fd = m_svc.ownerFd();
  if (e == MotionEvent::kBodyStall || e == MotionEvent::kBodyEncoderFault) {
    const PrimKind k = m_svc.kind();
    if (k != PrimKind::kMove && k != PrimKind::kArc && k != PrimKind::kGo) return LinkOut{};  // spin / pivot / rate: own checks
    const EndReason r = e == MotionEvent::kBodyStall ? EndReason::kStalled : EndReason::kEncoderFault;
    return finished(m_svc.cancel(r), fd, now_ms);  // the Uno already stopped both tracks; STOP while owner
  }
  if (e == MotionEvent::kCancel) return finished(m_svc.cancel(reasonOf(e)), fd, now_ms);
  if (e == MotionEvent::kTxFull) return finished(m_svc.cancel(EndReason::kLink), fd, now_ms);  // review minor 12
  return eventOnly(m_svc.cancel(reasonOf(e)));  // the STOP is already on its way, or the owner's own frames follow
}

LinkOut MotionLink::hold(uint32_t now_ms) {
  if (m_svc.kind() != PrimKind::kGo) return LinkOut{};
  const int fd = m_svc.ownerFd();
  return finished(m_svc.hold(), fd, now_ms);
}

}  // namespace core
}  // namespace cq

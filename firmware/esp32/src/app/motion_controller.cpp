#include "app/motion_controller.h"

#include <stdio.h>

#include "core/arc_limits.h"
#include "core/motion_config.h"
#include "core/motion_router.h"
#include "core/turn_controller.h"
#include "ports/log_out.h"
#include "ports/motion_store.h"

namespace cq {
namespace app {
namespace {

// Plan K: the TELEM_FAST fields the motion service uses.
core::MotionTelem telemOf(const core::TankSnapshot& s) {
  core::MotionTelem t;
  t.yaw_cdeg = s.fast.yaw;
  t.rate_ddps = s.fast.yaw_rate;
  t.enc_l = s.fast.enc_l;
  t.enc_r = s.fast.enc_r;
  t.flags = s.fast.flags;
  t.dist_mm = s.fast.dist_mm;
  return t;
}

}  // namespace

void MotionController::begin() {
  m_cpm = ports::loadCpm();  // Plan K
  m_roll_mm = ports::loadRoll();  // spec 02 §G6
  m_coast_ms = ports::loadCoast();  // bridge LLD R18.2
}

bool MotionController::encoderFault() const {
  return ((m_link.snapshot().fast.flags >> static_cast<unsigned>(proto::FastFlagBit::kEncoderFault)) & 1u) != 0;
}

bool MotionController::ownedBy(int fd) const {  // Plan N3: a running Logo program counts as its page's primitive
  if (m_h.logo_fd != nullptr && m_h.logo_fd() == fd && fd >= 0) return true;
  return active() && m_motion.service().ownerFd() == fd;
}

core::PrimitiveView MotionController::view() const {
  const core::MotionService& svc = m_motion.service();
  return core::PrimitiveView{svc.active(), svc.kind(), svc.progressPct(), svc.limited()};
}

// Plan H (bridge LLD R4.6): execute a MotionLink decision - DRIVE src AUTO, STOP, primitive_done.
void MotionController::run(const core::LinkOut& o, uint32_t now) {
  if (o.drive && !m_link.sendDrive(o.v, o.w, proto::Source::kAuto)) {
    // Body gone: no frame can reach it. Queue full while synced: the STOP still goes (urgent slot, review minor 12).
    const core::MotionEvent e = m_link.synced() ? core::MotionEvent::kTxFull : core::MotionEvent::kBodyOffline;
    run(m_motion.onEvent(e, now), now);
    return;
  }
  if (o.stop) m_link.sendStop(now);  // STOP purges queued motion frames: no DRIVE 0/0 needed (review M11)
  if (!o.event) return;
  m_out.all(core::jsonPrimitiveDone(m_out.buf(), m_out.cap(), m_tel.nextEventSeq(), m_tel.tankMs(), o.end));
  if (o.end.id == kLogoPrimitive && m_h.logo_end != nullptr) m_h.logo_end(o.end, now);
  if (o.end.reason != core::EndReason::kDone) {  // Plan O: why a move / turn ended early
    m_log.add(core::LogKind::kPrimitive, static_cast<int32_t>(o.end.reason), static_cast<int32_t>(o.end.kind), "",
              m_tel.tankMs(), now);
  } else {
    learnFrom(o.end, now);
  }
}

// A done move: watch its roll-on (§G6). A done turn / pivot with a coast reading: average it in and save it (R18.2).
void MotionController::learnFrom(const core::PrimitiveEnd& e, uint32_t now) {
  if (e.kind == core::PrimKind::kMove) {
    const core::TankSnapshot& s = m_link.snapshot();
    m_roll.begin((s.fast.enc_l + s.fast.enc_r) / 2, now);
    ++m_roll_begun;
    return;
  }
  if (e.coast_ms == 0) return;
  const uint16_t next = core::coastAverage(m_coast_ms, e.coast_ms);
  ports::logLine("motion: turn coast %u ms -> average %u ms", static_cast<unsigned>(e.coast_ms),
                 static_cast<unsigned>(next));
  if (next != m_coast_ms && ports::saveCoast(next)) m_coast_ms = next;
}

void MotionController::end(core::MotionEvent e, uint32_t now) {
  run(m_motion.onEvent(e, now), now);
  if (m_h.logo_event != nullptr) m_h.logo_event(e, now);  // Plan N3: also between the program's motions
}

// Why a Logo motion cannot start now (true + why), or false.
bool MotionController::logoRefused(const logo::Action& a, core::BridgeReason& why) const {
  const bool counted = a.kind == logo::ActKind::kMove || a.kind == logo::ActKind::kArc || a.kind == logo::ActKind::kPivot;
  if (m_h.ota()) {
    why = core::BridgeReason::kBusy;
  } else if (!m_link.synced() || !m_link.snapshot().fast_valid) {
    why = core::BridgeReason::kBodyOffline;
  } else if (counted && (core::routeMove(m_link.hello().caps) != core::Route::kLocal || encoderFault())) {
    why = core::BridgeReason::kUnavailable;
  } else {
    return false;
  }
  return true;
}

bool MotionController::startLogo(int fd, const logo::Action& a, uint32_t now, core::BridgeReason& why) {
  if (logoRefused(a, why)) return false;
  m_roll.cancel();  // §G6: a new primitive
  const core::TankSnapshot& snap = m_link.snapshot();
  core::MotionService& svc = m_motion.service();
  const uint8_t spin = a.speed_pct < core::motion::kTurnWMin ? core::motion::kTurnWMin : a.speed_pct;
  switch (a.kind) {  // VARIANT POINT: Logo motions (Tank Logo LLD §10)
    case logo::ActKind::kMove:
      svc.startMove(fd, telemOf(snap), a.dist_cm, a.speed_pct, m_cpm, kLogoPrimitive, now, m_roll_mm);
      return true;
    case logo::ActKind::kArc:
      svc.startArc(fd, telemOf(snap), a.delta_cdeg, a.radius_cm, a.speed_pct, m_cpm, kLogoPrimitive, now);
      return true;
    case logo::ActKind::kPivot: svc.startPivot(fd, snap.fast.yaw, a.delta_cdeg, spin, kLogoPrimitive, now, m_coast_ms); return true;
    default: svc.startTurn(fd, snap.fast.yaw, a.delta_cdeg, spin, kLogoPrimitive, now, m_coast_ms); return true;  // turn, HOME
  }
}

bool MotionController::claimed(int fd, int32_t id, uint32_t now) {
  const core::Decision d = m_h.claim(fd, now);
  if (d != core::Decision::kSend) m_out.nack(fd, id, reasonOf(d));
  return d == core::Decision::kSend;
}

// Plan L: arcs need trustworthy counts and a radius / speed the tracks can drive.
bool MotionController::arcAllowed(int fd, int32_t id, const core::WsCommand& c) {
  if (encoderFault()) {
    m_out.nack(fd, id, core::BridgeReason::kUnavailable);
    return false;
  }
  const core::ArcBand b = core::arcBand(c.radius_cm);
  if (b.possible && c.speed >= b.lo && c.speed <= b.hi) return true;
  char why[96];
  core::arcRefusal(why, sizeof(why), c.radius_cm);
  m_out.to(fd, core::jsonNackMessage(m_out.buf(), m_out.cap(), id, core::BridgeReason::kInvalidCommand, why));
  return false;
}

bool MotionController::turnAllowed(int fd, int32_t id, const core::WsCommand& c, uint32_t now) {
  if (m_h.ota()) {
    m_out.nack(fd, id, core::BridgeReason::kBusy);
    return false;
  }
  if (core::routeTurn(m_link.hello().caps, c.radius_cm, c.pivot) != core::Route::kLocal) {
    m_out.nack(fd, id, core::BridgeReason::kUnavailable);  // arc / pivot need encoders; Forward: no full-firmware body
    return false;
  }
  if (!m_link.synced() || !m_link.snapshot().fast_valid) {
    m_out.nack(fd, id, core::BridgeReason::kBodyOffline);
    return false;
  }
  if (c.radius_cm > 0 && !arcAllowed(fd, id, c)) return false;
  return claimed(fd, id, now);
}

// turn (LLD R4.2 / R4.6 / R8): spin, arc or pivot - Local on a body without TURN / MOVE.
void MotionController::onTurn(int fd, int32_t id, const core::WsCommand& c, uint32_t now) {
  if (!turnAllowed(fd, id, c, now)) return;
  end(core::MotionEvent::kOwnerCommand, now);  // a new turn replaces the old primitive
  m_roll.cancel();  // §G6: a new primitive - the last move's roll-on is no longer measurable
  const core::TankSnapshot& snap = m_link.snapshot();
  const int32_t delta = core::turnDelta(snap.fast.yaw, c.turn_has_to, c.turn_by_cdeg, c.turn_to_cdeg);
  core::MotionService& svc = m_motion.service();
  if (c.radius_cm > 0) {
    svc.startArc(fd, telemOf(snap), delta, c.radius_cm, c.speed, m_cpm, id, now);
  } else if (c.pivot) {
    svc.startPivot(fd, snap.fast.yaw, delta, c.speed, id, now, m_coast_ms);
  } else {
    svc.startTurn(fd, snap.fast.yaw, delta, c.speed, id, now, m_coast_ms);
  }
  m_out.ack(fd, id);
}

// Why a drive_rate stream may not run now (true + why), or false when it may (ownership then claimed).
bool MotionController::rateRefused(int fd, uint32_t now, core::BridgeReason& why) {
  if (m_h.ota()) {
    why = core::BridgeReason::kBusy;
  } else if (core::routeDriveRate(m_link.hello().caps) != core::Route::kLocal) {
    why = core::BridgeReason::kUnavailable;  // Forward: no full-firmware body yet
  } else if (!m_link.synced()) {
    why = core::BridgeReason::kBodyOffline;
  } else {
    const core::Decision d = m_h.claim(fd, now);
    if (d == core::Decision::kSend) return false;
    why = reasonOf(d);
  }
  return true;
}

// drive_rate (LLD R4.4): a stream like drive; the DRIVE frames come from the service on each TELEM_FAST.
void MotionController::onDriveRate(int fd, const core::WsCommand& c, uint32_t now) {
  core::BridgeReason why = core::BridgeReason::kBusy;
  if (rateRefused(fd, now, why)) return m_out.streamReject(fd, why);  // reported once per stream
  const core::MotionService& svc = m_motion.service();
  if (svc.active() && (svc.kind() != core::PrimKind::kRate || svc.ownerFd() != fd)) {
    end(core::MotionEvent::kOwnerCommand, now);
  }
  m_roll.cancel();  // §G6: a new primitive - the last move's roll-on is no longer measurable
  m_motion.service().driveRate(fd, c.v, c.rate_ddps, now);
  m_out.clients().streamAccepted(fd);
}

// Why a move may not start now (true + why), or false.
bool MotionController::moveRefused(core::BridgeReason& why) const {
  if (m_h.ota()) {
    why = core::BridgeReason::kBusy;
  } else if (core::routeMove(m_link.hello().caps) != core::Route::kLocal) {
    why = core::BridgeReason::kUnavailable;  // encoders off
  } else if (!m_link.synced() || !m_link.snapshot().fast_valid) {
    why = core::BridgeReason::kBodyOffline;
  } else if (encoderFault()) {
    why = core::BridgeReason::kUnavailable;  // an encoder is switched off until the Uno restarts
  } else {
    return false;
  }
  return true;
}

bool MotionController::moveAllowed(int fd, int32_t id, uint32_t now) {
  core::BridgeReason why = core::BridgeReason::kBusy;
  if (moveRefused(why)) {
    m_out.nack(fd, id, why);
    return false;
  }
  return claimed(fd, id, now);
}

// move (Plan K, LLD R7.6): Local for an Uno with encoders and no encoder fault.
void MotionController::onMove(int fd, int32_t id, const core::WsCommand& c, uint32_t now) {
  if (!moveAllowed(fd, id, now)) return;
  end(core::MotionEvent::kOwnerCommand, now);  // a new move replaces the old primitive
  m_roll.cancel();  // a new move: the last one's roll-on is no longer measurable
  m_motion.service().startMove(fd, telemOf(m_link.snapshot()), c.move_cm, c.speed, m_cpm, id, now, m_roll_mm);
  m_out.ack(fd, id);
}

// cancel: the owner ends its primitive; acked also when nothing runs.
void MotionController::onCancel(int fd, int32_t id, uint32_t now) {
  if (active() && m_motion.service().ownerFd() != fd) {
    m_out.nack(fd, id, core::BridgeReason::kNotOwner);
    return;
  }
  end(core::MotionEvent::kCancel, now);
  m_out.ack(fd, id);
}

// §G6: learn the roll-on of the last move from the frames after its STOP.
void MotionController::observeRoll(uint32_t now) {
  const core::TankSnapshot& s = m_link.snapshot();
  int32_t roll = 0;
  if (!m_roll.watching() ||
      !m_roll.observe((s.fast.enc_l + s.fast.enc_r) / 2, s.fast.pwm_l != 0 || s.fast.pwm_r != 0, m_cpm, now, roll)) {
    return;
  }
  const uint16_t next = core::rollAverage(m_roll_mm, roll);
  m_roll_last = roll;
  ++m_roll_read;
  ports::logLine("motion: roll-on %ld mm -> average %u mm", static_cast<long>(roll), static_cast<unsigned>(next));
  if (next != m_roll_mm && ports::saveRoll(next)) m_roll_mm = next;
}

void MotionController::onTelemetry(uint32_t now) {
  observeRoll(now);
  run(m_motion.onTelemetry(telemOf(m_link.snapshot()), now), now);  // 50 Hz
}

void MotionController::tick(uint32_t now) { run(m_motion.tick(now), now); }

void MotionController::sendCalib(int fd) {
  char* b = m_out.buf();
  size_t n = core::jsonCalib(b, m_out.cap() - 64, m_cpm, m_roll_mm);
  if (n > 1) {  // + roll-on learner diagnostics (spec 02 §G6 floor check): replace the closing brace
    const int k = snprintf(b + n - 1, m_out.cap() - n + 1, ",\"roll_last\":%ld,\"roll_watched\":%u,\"roll_read\":%u}",
                           static_cast<long>(m_roll_last), static_cast<unsigned>(m_roll_begun),
                           static_cast<unsigned>(m_roll_read));
    if (k > 0) n = n - 1 + static_cast<size_t>(k);
  }
  m_out.to(fd, n);
}

}  // namespace app
}  // namespace cq

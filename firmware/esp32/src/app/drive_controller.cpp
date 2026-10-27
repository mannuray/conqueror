#include "app/drive_controller.h"

#include <stdio.h>

#include "ports/client_out.h"
#include "ports/log_out.h"

namespace cq {
namespace app {
namespace {

// Streaming commands (one rejection reported per stream) and every motion command (dropped behind a STOP).
bool isStream(core::WsCmdKind k) {
  return k == core::WsCmdKind::kDrive || k == core::WsCmdKind::kCamJoy || k == core::WsCmdKind::kDriveRate;
}
bool isMotion(core::WsCmdKind k) {  // posted before a STOP that was handled first: dropped (logo_run too, Plan N3)
  return isStream(k) || k == core::WsCmdKind::kTurn || k == core::WsCmdKind::kMove || k == core::WsCmdKind::kLogoRun;
}

core::AckedOp requestOf(int fd, int32_t id, const core::WsCommand& c) {
  core::AckedOp op;
  op.fd = fd;
  op.id = id;
  op.kind = core::AckedOp::kCamLock;
  op.a = c.on ? 1 : 0;
  if (c.kind == core::WsCmdKind::kCamHome) op.kind = core::AckedOp::kCamHome;
  if (c.kind == core::WsCmdKind::kSound) {
    op.kind = core::AckedOp::kSound;
    op.a = c.sound_id;
  }
  if (c.kind == core::WsCmdKind::kAudioMode) {
    op.kind = core::AckedOp::kAudioMode;
    op.a = c.audio_mode;
  }
  return op;
}

}  // namespace

void DriveController::begin(uint32_t now) { applyOwner(m_owner.begin(), now); }

// Executes LinkOwner actions in the documented order.
void DriveController::applyOwner(const core::Actions& a, uint32_t now) {
  core::BodyLink& link = m_uno.link();
  if (a.has(core::Actions::kSendStop)) link.sendStop(now);
  if (a.has(core::Actions::kHintNone)) link.setOwnerHint(proto::OwnerHintValue::kNone);
  if (a.has(core::Actions::kHintHuman)) link.setOwnerHint(proto::OwnerHintValue::kHuman);
  if (a.has(core::Actions::kStartMaster)) link.startMaster(now);
  if (!a.has(core::Actions::kOwnerChanged)) return;
  m_out.all(core::jsonControlChanged(m_out.buf(), m_out.cap(), m_tel.nextEventSeq(), m_tel.tankMs(), ownerView()));
  m_tel.sendTs();
}

core::Decision DriveController::claim(int fd, uint32_t now, bool moving) {
  core::Actions a;
  const core::Decision d = m_owner.onDrive(fd, now, a, moving);
  applyOwner(a, now);
  return d;
}

core::OwnerView DriveController::ownerView() const {
  if (m_owner.irActive()) return {"ir", ""};
  if (m_owner.driverFd() != core::LinkOwner::kNoFd) return {"human", m_out.clients().name(m_owner.driverFd())};
  return {"none", ""};
}

bool DriveController::admitted(int fd, core::WsCmdKind kind, int32_t id, uint32_t post_seq, uint32_t now) {
  if (isMotion(kind) && m_barrier.stale(post_seq)) return false;  // posted before a STOP handled first: dropped
  if (m_out.clients().admit(fd, kind, now)) return true;
  if (isStream(kind)) {
    m_out.streamReject(fd, core::BridgeReason::kRateLimited);
  } else {
    m_out.nack(fd, id, core::BridgeReason::kRateLimited);
  }
  return false;
}

void DriveController::onStop(int fd, int32_t id, uint32_t post_seq, uint32_t now) {
  core::WsClients& ws = m_out.clients();
  m_barrier.onStop(post_seq);
  if (m_owner.state() == core::LinkState::kWebDriving && m_owner.driverFd() != fd) {  // Plan O: §72 stop hold
    m_log.add(core::LogKind::kStopHold, 0, 0, ws.contains(fd) ? ws.name(fd) : "?", m_tel.tankMs(), now);
  }
  applyOwner(m_owner.onStop(fd), now);  // urgent slot; queued DRIVE / CAM_JOY frames are purged
  m_motion.end(core::MotionEvent::kStop, now);  // Plan H: a STOP from anyone ends a primitive
  if (!ws.contains(fd)) return;
  ws.admit(fd, core::WsCmdKind::kStop, now);  // never limited; counts as alive
  m_uno.waitStop(fd, id);  // answered when the Uno acks the STOP
}

void DriveController::onDrive(int fd, const core::WsCommand& c, uint32_t now) {
  if (m_ota) return m_out.streamReject(fd, core::BridgeReason::kBusy);
  const bool moving = c.v != 0 || c.w != 0;
  core::Actions a;
  const core::Decision d = m_owner.onDrive(fd, now, a, moving);  // §72: a held stick stays stopped
  applyOwner(a, now);
  if (d != core::Decision::kSend) return m_out.streamReject(fd, reasonOf(d));
  if (m_motion.ownedBy(fd)) m_motion.end(core::MotionEvent::kOwnerCommand, now);
  if (moving) m_motion.cancelRollOn();  // §G6: driving again, no roll-on
  if (m_uno.link().sendDrive(c.v, c.w)) {  // 1:1 - the bridge never repeats a DRIVE
    m_out.clients().streamAccepted(fd);
  } else {
    m_out.streamReject(fd, core::BridgeReason::kBodyOffline);
  }
}

void DriveController::onCamJoy(int fd, const core::WsCommand& c) {
  const core::Decision d = m_owner.onCamera(fd);
  if (d != core::Decision::kSend || m_ota) {
    m_out.streamReject(fd, m_ota ? core::BridgeReason::kBusy : reasonOf(d));
  } else if (m_uno.link().sendCamJoy(c.x, c.y, c.active)) {
    m_out.clients().streamAccepted(fd);
  }
}

// True (+ why) when a camera / sound / audio request is refused: not the owner, or OTA (cam_home: owner only, as before).
bool DriveController::requestRefused(const core::WsCommand& c, core::Decision d, core::BridgeReason& why) const {
  if (d != core::Decision::kSend) {
    why = reasonOf(d);
    return true;
  }
  why = core::BridgeReason::kBusy;
  return m_ota && c.kind != core::WsCmdKind::kCamHome;
}

// CAM_HOME / SOUND / AUDIO_MODE / CAM_LOCK: owner or nobody (as the camera); acked by the Uno through UnoChannel.
void DriveController::onUnoRequest(int fd, int32_t id, const core::WsCommand& c) {
  core::BridgeReason why = core::BridgeReason::kBusy;
  if (requestRefused(c, m_owner.onCamera(fd), why)) return m_out.nack(fd, id, why);
  if (!m_uno.request(requestOf(fd, id, c))) m_out.nack(fd, id, core::BridgeReason::kBusy);  // R15.5: BUSY only when full
}

// R22: the camera rule (as cam_home), never ownership; OUT_OF_RANGE first, so the sender learns the limits.
void DriveController::onCamSet(int fd, int32_t id, const core::WsCommand& c) {
  if (!c.cam_in_range) return m_out.nack(fd, id, core::BridgeReason::kOutOfRange);
  core::BridgeReason why = core::BridgeReason::kBusy;
  if (requestRefused(c, m_owner.onCamera(fd), why)) return m_out.nack(fd, id, why);
  core::AckedOp op;
  op.kind = core::AckedOp::kCamSet;
  op.fd = fd;
  op.id = id;
  op.pan_dd = c.cam_pan_dd;
  op.tilt_dd = c.cam_tilt_dd;
  if (!m_uno.request(op)) m_out.nack(fd, id, core::BridgeReason::kBusy);
}

void DriveController::onRelease(int fd, int32_t id, uint32_t now) {
  applyOwner(m_owner.onRelease(fd), now);
  if (m_motion.ownedBy(fd)) m_motion.end(core::MotionEvent::kRelease, now);  // review I1: release ends a primitive
  m_out.ack(fd, id);
}

// LLD R5: opt-in debug stream for this client only (any role: read-only data).
void DriveController::onDebug(int fd, int32_t id, const core::WsCommand& c) {
  m_out.clients().setDebug(fd, c.on ? static_cast<uint8_t>(c.debug_hz == 50 ? 1 : 5) : 0);
  m_out.ack(fd, id);
}

void DriveController::onClientOpen(int fd, uint32_t now) {
  core::WsClients& ws = m_out.clients();
  if (m_ota || !ws.add(fd, now)) {
    ports::clientClose(fd);  // full (4 clients) or updating
    return;
  }
  ports::logLine("ws: %s connected (%u clients)", ws.name(fd), static_cast<unsigned>(ws.count()));
  applyOwner(m_owner.onWsOpen(fd), now);
}

void DriveController::onClientClose(int fd, uint32_t now) {
  core::WsClients& ws = m_out.clients();
  if (!ws.contains(fd)) return;  // a plain HTTP socket
  char name[12];
  snprintf(name, sizeof(name), "%s", ws.name(fd));
  if (m_motion.ownedBy(fd)) m_motion.end(core::MotionEvent::kOwnerClosed, now);  // onWsClose STOPs its driver
  if (fd == m_owner.driverFd()) m_log.add(core::LogKind::kClientLost, 0, 0, name, m_tel.tankMs(), now);  // Plan O
  const core::Actions a = m_owner.onWsClose(fd);
  m_settings.onClientClosed(fd);  // Plan M: a waiting change of that client is dropped (review focus 4)
  ws.remove(fd);
  ports::logLine("ws: %s closed (%u clients)", name, static_cast<unsigned>(ws.count()));
  applyOwner(a, now);
}

void DriveController::onUnoOwner(uint8_t owner, uint32_t now) {
  applyOwner(m_owner.onUnoOwner(static_cast<proto::Owner>(owner)), now);
}

void DriveController::onSynced(bool synced, uint32_t now) { applyOwner(m_owner.onSynced(synced), now); }

void DriveController::startOta(uint32_t now) {
  m_ota = true;
  m_log.add(core::LogKind::kOta, 0, 0, "", m_tel.tankMs(), now);
  m_motion.end(core::MotionEvent::kOta, now);  // the STOP below stops it
  m_uno.link().sendStop(now);  // resent until the Uno's ACK
  m_uno.stopForOta();  // heartbeats stop: Uno link-loss stop at standstill is harmless (design-notes §95)
  core::WsClients& ws = m_out.clients();
  for (size_t i = 0; i < ws.count(); ++i) ports::clientClose(ws.fdAt(i));  // kWsClose follows
}

void DriveController::reapSilent(uint32_t now) {
  int fds[core::kMaxWsClients];
  const size_t n = m_out.clients().silent(now, fds, core::kMaxWsClients);
  for (size_t i = 0; i < n; ++i) {
    ports::logLine("ws: %s silent for 3 s - closing", m_out.clients().name(fds[i]));
    if (!ports::clientClose(fds[i])) onClientClose(fds[i], now);  // review M6: a ghost (session gone) - remove it here
  }
}

void DriveController::tick(uint32_t now) {
  // Only a NEW slow frame updates the owner (review I1): re-applying the 1 Hz frame's older owner every tick would
  // undo an IR takeover reported by EVENT control_changed.
  const core::TankSnapshot& s = m_uno.link().snapshot();
  if (s.slow_count != m_last_slow) {
    m_last_slow = s.slow_count;
    onUnoOwner(s.slow.owner, now);
  }
  applyOwner(m_owner.tick(now), now);
  reapSilent(now);
}

}  // namespace app
}  // namespace cq

#include "core/link_owner.h"

namespace cq {
namespace core {

Actions LinkOwner::dropDriver(bool send_stop) {
  Actions a;
  if (m_driver == kNoFd) return a;
  m_driver = kNoFd;
  m_stop_hold = false;
  if (m_state == LinkState::kWebDriving) m_state = LinkState::kWebReady;
  a.bits |= Actions::kHintNone | Actions::kOwnerChanged;
  if (send_stop) a.bits |= Actions::kSendStop;
  return a;
}

Actions LinkOwner::onWsOpen(int /*fd*/) {
  Actions a;
  ++m_ws;
  if (m_state == LinkState::kIdle) {
    m_state = LinkState::kWebHandshake;
    m_synced = false;
    a.bits |= Actions::kStartMaster;
  }
  return a;
}

Actions LinkOwner::onWsClose(int fd) {
  Actions a;
  if (m_ws > 0) --m_ws;
  if (fd == m_driver) a = dropDriver(true);  // explicit STOP, belt and braces
  if (m_ws == 0 && webMaster()) {
    m_state = LinkState::kIdle;
    m_synced = false;
    a.bits |= Actions::kSendStop | Actions::kStopMaster;
  }
  return a;
}

Admit LinkOwner::onTcpConnect(Actions& out) {
  out = Actions();
  if (m_state == LinkState::kWebDriving || m_state == LinkState::kTcpPassthrough) {
    return Admit::kRefuseBusy;  // accept + close at once; the bench sees EOF
  }
  if (webMaster()) out.bits |= Actions::kStopMaster;
  m_state = LinkState::kTcpPassthrough;
  m_synced = false;
  out.bits |= Actions::kOwnerChanged;
  return Admit::kAccept;
}

Actions LinkOwner::onTcpClose() {
  Actions a;
  if (m_state != LinkState::kTcpPassthrough) return a;
  a.bits |= Actions::kInjectStop | Actions::kOwnerChanged;
  if (m_ws > 0) {
    m_state = LinkState::kWebHandshake;
    a.bits |= Actions::kStartMaster;  // always starts with HELLO_REQ (Uno synced or not)
  } else {
    m_state = LinkState::kIdle;
  }
  return a;
}

Decision LinkOwner::onDrive(int fd, uint32_t now_ms, Actions& out, bool moving) {
  out = Actions();
  if (m_state == LinkState::kTcpPassthrough) return Decision::kNackBusy;
  if (m_ir) return Decision::kNackNotOwner;
  if (m_state == LinkState::kIdle || m_state == LinkState::kWebHandshake) {
    return Decision::kNackBodyOffline;
  }
  if (m_state == LinkState::kWebDriving && fd != m_driver) return Decision::kNackNotOwner;
  if (m_state == LinkState::kWebReady) {
    m_state = LinkState::kWebDriving;
    m_driver = fd;
    out.bits |= Actions::kHintHuman | Actions::kOwnerChanged;
  }
  m_last_drive_ms = now_ms;  // also while held: the driver keeps control (a handback would let it retake and drive)
  if (m_stop_hold && fd == m_driver) {
    if (moving) return Decision::kNackStopped;
    m_stop_hold = false;  // stick released (0/0) or a deliberate new command
  }
  return Decision::kSend;
}

Decision LinkOwner::keepDriving(int fd, uint32_t now_ms) {
  if (m_state == LinkState::kTcpPassthrough) return Decision::kNackBusy;
  if (m_state == LinkState::kIdle || m_state == LinkState::kWebHandshake) return Decision::kNackBodyOffline;
  if (m_ir || m_state != LinkState::kWebDriving || fd != m_driver) return Decision::kNackNotOwner;
  m_last_drive_ms = now_ms;
  return Decision::kSend;
}

Decision LinkOwner::onCamera(int fd) const {
  if (m_state == LinkState::kTcpPassthrough) return Decision::kNackBusy;
  if (m_state == LinkState::kIdle || m_state == LinkState::kWebHandshake) {
    return Decision::kNackBodyOffline;
  }
  if (m_state == LinkState::kWebDriving && fd != m_driver) return Decision::kNackNotOwner;
  return Decision::kSend;
}

Actions LinkOwner::onStop(int fd) {
  Actions a;
  // design-notes §72: a STOP from anyone but the driver holds the driver's moving frames until it lets go;
  // the driver's own STOP (its page's STOP / stick release) clears a hold.
  if (m_state == LinkState::kWebDriving) m_stop_hold = fd != m_driver;
  a.bits |= (m_state == LinkState::kTcpPassthrough) ? Actions::kInjectStop : Actions::kSendStop;
  return a;  // STOP never changes ownership (the driver keeps it for the handback window)
}

Actions LinkOwner::onRelease(int fd) {
  if (fd != m_driver) return Actions();
  return dropDriver(true);
}

Actions LinkOwner::onSynced(bool synced) {
  Actions a;
  m_synced = synced;
  if (!webMaster()) return a;
  if (synced && m_state == LinkState::kWebHandshake) {
    m_state = LinkState::kWebReady;
    a.bits |= Actions::kOwnerChanged;
  } else if (!synced) {
    a = dropDriver(false);  // Uno is gone: nothing to stop
    m_state = LinkState::kWebHandshake;
    a.bits |= Actions::kOwnerChanged;
  }
  return a;
}

Actions LinkOwner::onUnoOwner(proto::Owner owner) {
  Actions a;
  const bool ir = owner == proto::Owner::kIr;
  if (ir == m_ir) return a;
  m_ir = ir;
  a.bits |= Actions::kOwnerChanged;
  if (ir) {
    const Actions d = dropDriver(false);  // the Uno already stopped for IR
    a.bits |= d.bits;
  }
  return a;
}

Actions LinkOwner::tick(uint32_t now_ms) {
  if (m_state == LinkState::kWebDriving && now_ms - m_last_drive_ms >= kHandbackMs) {
    return dropDriver(true);
  }
  return Actions();
}

}  // namespace core
}  // namespace cq
